/*
 * ============================================================================
 *  Camara_S3_ota_ws — Cámara OV3660 de la ESP32-S3 CAM al panel SAMATI, con OTA
 * ============================================================================
 *
 *  Placa: ESP32-S3-WROOM CAM N16R8 (patillaje Freenove) con sensor OV3660,
 *  puerto UART CH343 (COM5). Librería de cámara: components/esp32s3cam (copia de
 *  Cloudcode/esp32s3-cam-ov3660, con modos afinados y verificados en la placa).
 *
 *    1. WiFi: la red de casa o el hotspot del teléfono, la que vea mejor
 *       (components/net_wifi escanea y cambia sola si una desaparece).
 *    2. Token de la máquina y DOS WebSockets con la API (ver enlace_api.h):
 *         vídeo   -> /camera_rt/ws/machine   (JPEG binarios, ajustes del panel)
 *         control -> /ws/machine             (estado_dron con "ota", órdenes de OTA)
 *    3. Vídeo: HD30 (1280x720) y se manda a CONFIG_CAM_FPS (12 por defecto; la API
 *       acepta hasta 15). Si la red va lenta se manda siempre el frame más reciente.
 *    4. OTA: enbedded/tools/ota_subir.py --proyecto <esta carpeta> --maquina <nombre>
 *       compila, sube el .bin a la API y se lo manda a la cámara. Si la versión nueva
 *       no llega a la API en 2 minutos, el bootloader vuelve a la anterior.
 *
 *  Configurar:  sdkconfig.defaults.local (WiFi, hotspot, máquina) o idf.py menuconfig
 *               -> "Camara S3 SAMATI: video + OTA"
 *  Primer flasheo (USB, instala bootloader con rollback y la tabla OTA):
 *               idf.py -p COM5 flash monitor
 *  Después:     python enbedded/tools/ota_subir.py --proyecto enbedded/Examples/Camara_S3_ota_ws
 *                      --maquina dron_camera_01
 * ============================================================================
 */

#include <inttypes.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp32s3cam.h"
#include "enlace_api.h"
#include "net_wifi.h"
#include "ota_camara.h"

static const char *TAG = "camara";

#define NUCLEO_VIDEO     1   // junto a la tarea del driver de la cámara
#define PRIORIDAD_VIDEO  4
#define INFORME_CADA_S   10

/* ---------------------------------------------------------------------------
 *  Imagen adaptativa: la red (no la cámara) decide cuántos bytes caben
 * ------------------------------------------------------------------------- */
// Medido 2026-11-23: por el WiFi de casa + túnel de Cloudflare caben ~350 kbit/s
// (RTT ~160 ms con pérdidas): HD q25 salía a 4-5 fps. Mejor la imagen más grande
// que quepa A LOS fps pedidos. Cada escalón tiene ~la mitad de bytes que el anterior
// (la calidad JPEG del OV3660 cambia poco el tamaño; la resolución sí).
typedef struct {
    framesize_t fs;    // FRAMESIZE_INVALID = el modo de CONFIG_CAM_MODE
    int calidad;
} escalon_t;

static const escalon_t ESCALONES[] = {
    { FRAMESIZE_INVALID, CONFIG_CAM_JPEG_QUALITY },   // lo configurado (HD30 q25)
    { FRAMESIZE_SVGA, 30 },                           // 800x600, campo completo
    { FRAMESIZE_VGA, 35 },                            // 640x480: 3,7 KB (escena oscura) a 7,5 KB (de día)
    { FRAMESIZE_QVGA, 25 },                           // 320x240, ~3 KB: último recurso
};
#define N_ESCALONES       (sizeof(ESCALONES) / sizeof(ESCALONES[0]))
#define BAJAR_TRAS_S      2      // segundos seguidos por debajo del 80 % de los fps pedidos
#define SUBIR_TRAS_S      5      // segundos seguidos a tope y con margen para el doble de bytes
#define OCUPACION_SUBIR   0.45f  // fracción del segundo enviando: < 45 % = cabe el escalón de arriba
#define SUBIDA_FALLIDA_US (10 * 1000000LL)   // bajar antes de esto tras subir = la subida no cupo
#define REINTENTO_US      (60 * 1000000LL)   // una subida fallida no se repite antes de esto

static int escalon_actual;
// Medido: sin esto, cada ~25 s probaba a subir y a los 2 s tenía que bajar otra vez.
static int64_t no_subir_hasta_us[N_ESCALONES];
static int64_t subio_us;

static void aplicar_escalon(int e)
{
    const escalon_t *s = &ESCALONES[e];
    esp_err_t err = s->fs == FRAMESIZE_INVALID ? esp32s3cam_set_mode(CONFIG_CAM_MODE)
                                               : esp32s3cam_set("framesize", s->fs);
    if (err == ESP_OK) {
        err = esp32s3cam_set("quality", s->calidad);
    }
    escalon_actual = e;
    ESP_LOGI(TAG, "imagen: escalón %d (%s, q%d) %s", e,
             s->fs == FRAMESIZE_INVALID ? CONFIG_CAM_MODE : esp32s3cam_framesize_name(s->fs), s->calidad,
             esp_err_to_name(err));
    enlace_api_enviar_estado();   // el panel ve la resolución nueva
}

/* Decide cada segundo si bajar o subir un escalón. fps = enviados ese segundo,
 * ocupacion = fracción del segundo que la tarea pasó dentro de los envíos. */
static void adaptar(float fps, float ocupacion)
{
    static int malos, buenos;
    const float objetivo = CONFIG_CAM_FPS;
    const int e = escalon_actual;
    const int64_t ahora = esp_timer_get_time();

    malos = fps < 0.8f * objetivo ? malos + 1 : 0;
    buenos = (fps >= 0.95f * objetivo && ocupacion < OCUPACION_SUBIR) ? buenos + 1 : 0;

    int nuevo = e;
    if (buenos >= SUBIR_TRAS_S && e > 0 && ahora >= no_subir_hasta_us[e]) {
        nuevo = e - 1;
        subio_us = ahora;
    } else if (malos >= BAJAR_TRAS_S && e + 1 < (int)N_ESCALONES) {
        if (ahora - subio_us < SUBIDA_FALLIDA_US) {
            no_subir_hasta_us[e + 1] = ahora + REINTENTO_US;   // la subida a "e" no cupo
        }
        nuevo = e + 1;
    }
    if (nuevo != e) {
        aplicar_escalon(nuevo);
        malos = buenos = 0;
    }
}

/* ---------------------------------------------------------------------------
 *  Tarea de vídeo: un frame cada 1/CONFIG_CAM_FPS s, con plazos absolutos
 * ------------------------------------------------------------------------- */
static void tarea_video(void *arg)
{
    const TickType_t periodo = pdMS_TO_TICKS(1000 / CONFIG_CAM_FPS);
    TickType_t siguiente = xTaskGetTickCount();
    uint32_t enviados = 0, fallidos = 0, saltados = 0;
    uint64_t bytes = 0;
    int64_t envio_total_us = 0, envio_max_us = 0;   // cuánto tarda cada envío (diagnóstico)
    int64_t informe = esp_timer_get_time();
    // Ventana de 1 s para la adaptación
    uint32_t v_enviados = 0;
    int64_t v_inicio = informe, v_enviando_us = 0;

    for (;;) {
        // Plazos absolutos: si un envío tardó de más, el siguiente sale antes y el
        // promedio se mantiene. Con un retraso mayor a un periodo no se "recupera"
        // en ráfaga (la API descarta lo que pase de 15 fps): se reanuda desde ahora.
        if (xTaskDelayUntil(&siguiente, periodo) == pdFALSE) {
            siguiente = xTaskGetTickCount();
        }
        // GRAB_LATEST: el driver entrega siempre el frame más reciente.
        camera_fb_t *fb = esp32s3cam_capture();
        if (fb == NULL) {
            continue;
        }
        if (!enlace_api_video_listo() || ota_en_curso()) {
            saltados++;   // sin API todavía, o la OTA necesita la red para ella
        } else if (!esp32s3cam_jpeg_ok(fb)) {
            fallidos++;   // sin SOI/EOI: no se manda un frame roto
        } else {
            int64_t t0 = esp_timer_get_time();
            if (enlace_api_enviar_frame(fb->buf, fb->len) == ESP_OK) {
                enviados++;
                v_enviados++;
                bytes += fb->len;
            } else {
                fallidos++;
            }
            int64_t dt = esp_timer_get_time() - t0;
            v_enviando_us += dt;
            envio_total_us += dt;
            envio_max_us = dt > envio_max_us ? dt : envio_max_us;
        }
        esp32s3cam_release(fb);

        int64_t ahora = esp_timer_get_time();
        if (ahora - v_inicio >= 1000000) {
            // Solo se adapta con vídeo fluyendo, sin OTA y sin ajuste manual del panel.
#if CONFIG_CAM_FPS_ADAPTATIVO
            if (enlace_api_video_listo() && !ota_en_curso() && !enlace_api_ajuste_manual()) {
                float s = (ahora - v_inicio) / 1e6f;
                adaptar(v_enviados / s, v_enviando_us / 1e6f / s);
            }
#endif
            v_enviados = 0;
            v_enviando_us = 0;
            v_inicio = ahora;
        }
        if (ahora - informe >= INFORME_CADA_S * 1000000LL) {
            float s = (ahora - informe) / 1e6f;
            uint32_t intentos = enviados + fallidos;
            ESP_LOGI(TAG, "vídeo %.1f fps, %.0f kbit/s, %.1f KB/frame, envío %.0f ms (máx %.0f), escalón %d%s, %"
                     PRIu32 " fallidos, %" PRIu32 " sin API | RSSI %d dBm | control %s",
                     enviados / s, bytes * 8 / 1000.0f / s, enviados ? bytes / 1024.0f / enviados : 0.0f,
                     intentos ? envio_total_us / 1000.0f / intentos : 0.0f, envio_max_us / 1000.0f,
                     escalon_actual, enlace_api_ajuste_manual() ? " (manual)" : "", fallidos, saltados,
                     net_wifi_get_rssi(), enlace_api_control_listo() ? "OK" : "--");
            enviados = fallidos = saltados = 0;
            bytes = 0;
            envio_total_us = envio_max_us = 0;
            informe = ahora;
        }
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // Lo primero: si esta versión acaba de llegar por OTA, empieza la cuenta atrás
    // del rollback (se cancela cuando la API acepta el token).
    ota_revisar_arranque();

    const esp32s3cam_config_t cfg = { .frame_size = FRAMESIZE_HD, .jpeg_quality = CONFIG_CAM_JPEG_QUALITY };
    err = esp32s3cam_init(&cfg);
    if (err != ESP_OK) {
        // Sin cámara se sigue igual: el enlace y la OTA funcionan, y así se puede
        // mandar un firmware que lo arregle sin tocar el cable.
        ESP_LOGE(TAG, "cámara no disponible (%s): solo enlace y OTA", esp_err_to_name(err));
    } else if (esp32s3cam_set_mode(CONFIG_CAM_MODE) != ESP_OK) {
        ESP_LOGW(TAG, "modo %s no existe: se queda en HD de serie", CONFIG_CAM_MODE);
    }
    ESP_LOGI(TAG, "firmware %s, cámara %s, modo %s, %d fps hacia la API", ota_version_actual(),
             esp32s3cam_sensor_name(), CONFIG_CAM_MODE, CONFIG_CAM_FPS);

    enlace_api_iniciar();
    if (err == ESP_OK) {
        // 8 KB: el cifrado TLS de cada envío corre en esta tarea (mbedTLS pide pila).
        xTaskCreatePinnedToCore(tarea_video, "video", 8192, NULL, PRIORIDAD_VIDEO, NULL, NUCLEO_VIDEO);
    }
}
