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
 *  Tarea de vídeo: un frame cada 1/CONFIG_CAM_FPS s, con plazos absolutos
 * ------------------------------------------------------------------------- */
static void tarea_video(void *arg)
{
    const TickType_t periodo = pdMS_TO_TICKS(1000 / CONFIG_CAM_FPS);
    TickType_t siguiente = xTaskGetTickCount();
    uint32_t enviados = 0, fallidos = 0, saltados = 0;
    uint64_t bytes = 0;
    int64_t informe = esp_timer_get_time();

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
        } else if (enlace_api_enviar_frame(fb->buf, fb->len) == ESP_OK) {
            enviados++;
            bytes += fb->len;
        } else {
            fallidos++;
        }
        esp32s3cam_release(fb);

        int64_t ahora = esp_timer_get_time();
        if (ahora - informe >= INFORME_CADA_S * 1000000LL) {
            float s = (ahora - informe) / 1e6f;
            ESP_LOGI(TAG, "vídeo %.1f fps, %.0f kbit/s, %.1f KB/frame, %" PRIu32 " fallidos, %" PRIu32
                     " sin API | RSSI %d dBm | control %s",
                     enviados / s, bytes * 8 / 1000.0f / s, enviados ? bytes / 1024.0f / enviados : 0.0f,
                     fallidos, saltados, net_wifi_get_rssi(), enlace_api_control_listo() ? "OK" : "--");
            enviados = fallidos = saltados = 0;
            bytes = 0;
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
