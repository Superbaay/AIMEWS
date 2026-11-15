/*
 * ============================================================================
 *  Camara_25fps — Vídeo a 25 fps en XGA (1024x768) con la ESP32-CAM AI-Thinker
 * ============================================================================
 *
 *  Qué hace:
 *    1. Inicia la cámara (sensor OV3660) en XGA, JPEG calidad 12.
 *    2. Toma un frame cada 40 ms exactos (25 fps) en una tarea del núcleo 1.
 *    3. Una vez por segundo copia un frame y lo DECODIFICA en el núcleo 0 para
 *       comprobar que la imagen no está distorsionada.
 *    4. Cada segundo imprime: fps reales, intervalo máximo entre frames, edad
 *       máxima del frame, KB/s y frames verificados/corruptos.
 *
 *  Por qué así:
 *    - El SENSOR va a su ritmo: con el PLL de fábrica y XCLK de 19 MHz entrega
 *      26,4 fps en XGA (medido). Nosotros tomamos "el más reciente" cada 40 ms
 *      (modo CAMERA_GRAB_LATEST con 2 buffers): así salen 25 fps justos y el
 *      sensor descarta ~1 de cada 19 frames sin que se note. Margen: 5 %.
 *    - El ritmo se marca con PLAZOS ABSOLUTOS (siguiente += 40 ms), no con
 *      "espera 40 ms": si un frame tarda, el siguiente se recupera y la media
 *      sigue siendo exactamente 25 fps (no se acumula el retraso).
 *    - Que un frame LLEGUE no significa que esté BIEN. El OV3660 comprime él
 *      mismo el JPEG: si el bus pierde bytes (p. ej. con un PLL más rápido que
 *      el de fábrica) el archivo parece completo, pero la imagen sale con
 *      bandas desplazadas. Solo se nota al decodificarlo -> tarea_verificar.
 *      Prueba a poner PRUEBA_PLL_RAPIDO a 1 para verlo.
 *
 *  Placa: AI-Thinker ESP32-CAM + base ESP32-CAM-MB (USB, CH340). Los pines
 *  de la cámara ya van dentro del componente esp32cam (components/esp32cam).
 *
 *  Presupuesto por frame (40 ms):
 *      capturar (el frame ya está listo)     < 1 ms
 *      libre para tu aplicación             ~39 ms (guardar, enviar, analizar)
 *      tamaño típico del JPEG             ~20 KB (≈ 4 Mbit/s a 25 fps)
 *
 *  Compilar y flashear:   idf.py -p COM6 flash monitor   (salir: Ctrl+])
 * ============================================================================
 */

#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp32cam.h"         // librería de la cámara en components/esp32cam
#include "img_converters.h"   // jpg2rgb565_bounded(), del driver esp32-camera

static const char *TAG = "camara_25fps";

/* ---------------------------------------------------------------------------
 *  Configuración
 * ------------------------------------------------------------------------- */

#define FPS_OBJETIVO      25
#define RESOLUCION        FRAMESIZE_XGA     // 1024x768
#define CALIDAD_JPEG      12                // 4 = mejor calidad ... 63 = peor (y más pequeño)

// Verifica 1 frame por segundo. Decodificar un JPEG cuesta mucho más que
// capturarlo, así que se hace con una COPIA en otro núcleo.
#define VERIFICAR_CADA_N  FPS_OBJETIVO
#define TAM_COPIA_MAX     (256 * 1024)      // un XGA q12 ronda los 20 KB

// 1 = usar un PLL un 55 % más rápido (73,6 MHz) para VER cómo se detectan
// frames corruptos: llegan a tiempo, pero la verificación los marca todos.
#define PRUEBA_PLL_RAPIDO 0

/* ---------------------------------------------------------------------------
 *  Verificación de imagen (núcleo 0, prioridad baja)
 * ------------------------------------------------------------------------- */

static uint8_t *copia_jpeg;          // copia del frame a verificar (PSRAM)
static size_t copia_len;
static volatile bool copia_ocupada;  // true mientras tarea_verificar la usa
static uint8_t *salida_rgb;          // imagen decodificada a 1/8 de escala
static size_t salida_tam;
static TaskHandle_t tarea_verificar_handle;
static volatile uint32_t total_verificados;
static volatile uint32_t total_corruptos;

static void tarea_verificar(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        // A 1/8 de escala el decodificador solo calcula 1 píxel por bloque de
        // 8x8, pero tiene que leer TODOS los datos: si faltan bytes, falla.
        bool sana = jpg2rgb565_bounded(copia_jpeg, copia_len, salida_rgb, salida_tam,
                                       JPG_SCALE_8X);
        total_verificados++;
        if (!sana) {
            total_corruptos++;
            ESP_LOGW(TAG, "Frame CORRUPTO (%u bytes): la imagen sale distorsionada",
                     (unsigned)copia_len);
        }
        copia_ocupada = false;
    }
}

/* ---------------------------------------------------------------------------
 *  Captura a ritmo fijo (núcleo 1)
 * ------------------------------------------------------------------------- */

typedef struct {
    uint32_t frames;            // frames entregados en el último segundo
    uint32_t bytes;
    uint32_t fallos_captura;
    int64_t intervalo_max_us;   // mayor separación entre dos frames entregados
    int64_t edad_max_us;        // desde que el sensor empezó el frame hasta usarlo
} estadisticas_t;

static void tarea_camara(void *arg)
{
    const int64_t periodo_us = 1000000LL / FPS_OBJETIVO;
    int64_t siguiente_us = esp_timer_get_time();
    int64_t inicio_segundo_us = siguiente_us;
    int64_t ultimo_frame_us = 0;
    uint32_t contador = 0;
    estadisticas_t est = { 0 };

    for (;;) {
        camera_fb_t *fb = esp32cam_capture();   // el más reciente que tenga el driver
        int64_t ahora_us = esp_timer_get_time();

        if (fb == NULL) {
            est.fallos_captura++;
        } else {
            // 1) Métricas de ritmo.
            if (ultimo_frame_us != 0 && ahora_us - ultimo_frame_us > est.intervalo_max_us) {
                est.intervalo_max_us = ahora_us - ultimo_frame_us;
            }
            ultimo_frame_us = ahora_us;
            int64_t inicio_frame_us = (int64_t)fb->timestamp.tv_sec * 1000000LL
                                      + fb->timestamp.tv_usec;
            if (ahora_us - inicio_frame_us > est.edad_max_us) {
                est.edad_max_us = ahora_us - inicio_frame_us;
            }
            est.frames++;
            est.bytes += fb->len;

            // 2) AQUÍ va tu aplicación: guardar en SD, enviar por WiFi, buscar
            //    movimiento... fb->buf (fb->len bytes) es un JPEG de fb->width x
            //    fb->height. El buffer es del driver hasta esp32cam_release():
            //    si vas a tardar más de ~30 ms, cópialo y suéltalo antes.

            // 3) Muestra para la verificación (si la anterior ya terminó).
            if (++contador % VERIFICAR_CADA_N == 0 && !copia_ocupada
                && fb->len <= TAM_COPIA_MAX) {
                memcpy(copia_jpeg, fb->buf, fb->len);
                copia_len = fb->len;
                copia_ocupada = true;
                xTaskNotifyGive(tarea_verificar_handle);
            }
            esp32cam_release(fb);   // sin esto el driver se queda sin buffers
        }

        // 4) Estadísticas cada segundo.
        if (ahora_us - inicio_segundo_us >= 1000000) {
            float s = (ahora_us - inicio_segundo_us) / 1e6f;
            ESP_LOGI(TAG, "%5.1f fps | intervalo máx %3lld ms | edad máx %3lld ms | "
                     "%4lu KB/s | verificados %lu, corruptos %lu%s",
                     est.frames / s, est.intervalo_max_us / 1000, est.edad_max_us / 1000,
                     (unsigned long)(est.bytes / s / 1024), (unsigned long)total_verificados,
                     (unsigned long)total_corruptos,
                     est.fallos_captura ? " | ¡fallos de captura!" : "");
            memset(&est, 0, sizeof(est));
            inicio_segundo_us = ahora_us;
        }

        // 5) Esperar al siguiente plazo absoluto.
        siguiente_us += periodo_us;
        int64_t espera_us = siguiente_us - esp_timer_get_time();
        if (espera_us > 0) {
            vTaskDelay(pdMS_TO_TICKS((espera_us + 500) / 1000));
        } else if (espera_us < -periodo_us) {
            siguiente_us = esp_timer_get_time();   // muy atrasados: sin ráfagas
        }
    }
}

/* ---------------------------------------------------------------------------
 *  Arranque
 * ------------------------------------------------------------------------- */

void app_main(void)
{
    const esp32cam_config_t cfg = {
        .frame_size = RESOLUCION,
        .jpeg_quality = CALIDAD_JPEG,
        .fb_count = 2,       // 2 buffers: el sensor llena uno mientras usamos el otro
        // .xclk_hz = 0 -> 19 MHz (20 MHz exactos interfieren con el WiFi)
    };
    ESP_ERROR_CHECK(esp32cam_init(&cfg));

#if PRUEBA_PLL_RAPIDO
    const esp32cam_pll_t pll_rapido = {
        .multiplier = 31, .sys_div = 1, .pre_div = 2, .pclk_manual = 1, .pclk_div = 15,
    };
    ESP_ERROR_CHECK(esp32cam_set_pll(&pll_rapido));
    ESP_LOGW(TAG, "PRUEBA: PLL a 73,6 MHz -> espera frames corruptos");
#endif

    int ancho = 0, alto = 0;
    esp32cam_framesize_dims(RESOLUCION, &ancho, &alto);
    salida_tam = (size_t)(ancho / 8) * (alto / 8) * 2;   // RGB565 = 2 bytes/píxel
    copia_jpeg = heap_caps_malloc(TAM_COPIA_MAX, MALLOC_CAP_SPIRAM);
    salida_rgb = heap_caps_malloc(salida_tam, MALLOC_CAP_SPIRAM);
    if (copia_jpeg == NULL || salida_rgb == NULL) {
        ESP_LOGE(TAG, "Sin PSRAM para la verificación");
        return;
    }

    ESP_LOGI(TAG, "Sensor %s, %s (%dx%d), calidad %d, objetivo %d fps (cada %lld us)",
             esp32cam_sensor_name(), esp32cam_framesize_name(RESOLUCION), ancho, alto,
             CALIDAD_JPEG, FPS_OBJETIVO, 1000000LL / FPS_OBJETIVO);

    xTaskCreatePinnedToCore(tarea_verificar, "verificar", 6144, NULL, 2,
                            &tarea_verificar_handle, 0);
    xTaskCreatePinnedToCore(tarea_camara, "camara", 4096, NULL, 5, NULL, 1);
}
