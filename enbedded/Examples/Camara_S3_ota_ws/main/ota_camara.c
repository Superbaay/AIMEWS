#include "ota_camara.h"

#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "enlace_api.h"

// Copia de Dron_telemetria_ws/main/ota_dron.c: los avisos salen por el WebSocket de
// control (enlace_api_enviar_control) porque es el que conoce ota_rt.py.

static const char *TAG = "ota";

#define OTA_TIEMPO_VERIFICAR_S 120   // sin auth_ok en este tiempo -> rollback
#define OTA_AVISO_CADA_PCT     10    // avance al panel cada 10 %
#define OTA_PILA               8192  // TLS + cliente HTTP

#if CONFIG_CAM_API_TLS
#define CRT_BUNDLE esp_crt_bundle_attach
#else
#define CRT_BUNDLE NULL
#endif

typedef struct {
    char url[160];
    char version[32];
    int tam;
} pedido_ota_t;

static volatile bool en_curso;
static esp_timer_handle_t temporizador_rollback;
static pedido_ota_t pedido;

const char *ota_version_actual(void)
{
    return esp_app_get_description()->version;
}

bool ota_en_curso(void)
{
    return en_curso;
}

/* {"tipo":"ota","estado":...,"version":...,"porcentaje":N,"detalle":"..."} al panel. */
static void avisar(const char *estado, const char *version, int porcentaje, const char *detalle)
{
    char m[256];
    int n = snprintf(m, sizeof(m), "{\"tipo\":\"ota\",\"estado\":\"%s\",\"version\":\"%s\",\"version_actual\":\"%s\"",
                     estado, version, ota_version_actual());
    if (porcentaje >= 0) n += snprintf(m + n, sizeof(m) - n, ",\"porcentaje\":%d", porcentaje);
    if (detalle) n += snprintf(m + n, sizeof(m) - n, ",\"detalle\":\"%s\"", detalle);
    n += snprintf(m + n, sizeof(m) - n, "}");
    if (n < (int)sizeof(m)) enlace_api_enviar_control(m, n);
}

/* ---------------------------------------------------------------------------
 *  Rollback
 * ------------------------------------------------------------------------- */

static void al_vencer_verificacion(void *arg)
{
    ESP_LOGE(TAG, "la versión nueva no se conectó a la API en %d s: volviendo a la anterior",
             OTA_TIEMPO_VERIFICAR_S);
    esp_ota_mark_app_invalid_rollback_and_reboot();  // no regresa
}

void ota_revisar_arranque(void)
{
    const esp_partition_t *corriendo = esp_ota_get_running_partition();
    esp_ota_img_states_t estado;
    ESP_LOGI(TAG, "firmware %s en la partición %s", ota_version_actual(), corriendo->label);
    if (esp_ota_get_state_partition(corriendo, &estado) != ESP_OK || estado != ESP_OTA_IMG_PENDING_VERIFY) {
        return;  // arranque normal (o flasheado por USB)
    }
    ESP_LOGW(TAG, "versión recién instalada por OTA: tiene %d s para conectarse a la API o se revierte",
             OTA_TIEMPO_VERIFICAR_S);
    const esp_timer_create_args_t args = {.callback = al_vencer_verificacion, .name = "ota_rollback"};
    if (esp_timer_create(&args, &temporizador_rollback) == ESP_OK) {
        esp_timer_start_once(temporizador_rollback, (uint64_t)OTA_TIEMPO_VERIFICAR_S * 1000000);
    }
}

void ota_confirmar(void)
{
    if (temporizador_rollback == NULL) return;
    esp_timer_stop(temporizador_rollback);
    esp_timer_delete(temporizador_rollback);
    temporizador_rollback = NULL;
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        ESP_LOGI(TAG, "versión %s confirmada: ya no hay rollback", ota_version_actual());
        avisar("confirmado", ota_version_actual(), -1, NULL);
    }
}

/* ---------------------------------------------------------------------------
 *  Descarga
 * ------------------------------------------------------------------------- */

// El token de máquina va en la cabecera: la API no entrega firmware sin él.
static esp_err_t poner_token(esp_http_client_handle_t http)
{
    char cabecera[560];
    snprintf(cabecera, sizeof(cabecera), "Bearer %s", enlace_api_token());
    return esp_http_client_set_header(http, "Authorization", cabecera);
}

static void tarea_ota(void *arg)
{
    const char *error = NULL;
    esp_https_ota_handle_t ota = NULL;
    esp_http_client_config_t http = {
        .url = pedido.url,
        .crt_bundle_attach = CRT_BUNDLE,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,   // la cabecera Authorization lleva el JWT (~200 B)
    };
    esp_https_ota_config_t cfg = {
        .http_config = &http,
        .http_client_init_cb = poner_token,
    };

    ESP_LOGI(TAG, "descargando %s (versión %s, %d bytes)", pedido.url, pedido.version, pedido.tam);
    avisar("descargando", pedido.version, 0, NULL);

    if (esp_https_ota_begin(&cfg, &ota) != ESP_OK) {
        error = "no se pudo descargar (red, token o ruta)";
        goto fin;
    }

    // La cabecera de la imagen llega primero: se revisa ANTES de escribir todo.
    esp_app_desc_t nueva;
    if (esp_https_ota_get_img_desc(ota, &nueva) != ESP_OK) {
        error = "el archivo no es un firmware válido";
        goto fin;
    }
    if (strcmp(nueva.version, pedido.version) != 0) {
        error = "la versión del archivo no coincide con la anunciada";
        goto fin;
    }
    if (strcmp(nueva.project_name, esp_app_get_description()->project_name) != 0) {
        error = "el firmware es de otro proyecto";
        goto fin;
    }

    int siguiente_aviso = OTA_AVISO_CADA_PCT;
    esp_err_t err;
    while ((err = esp_https_ota_perform(ota)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int total = esp_https_ota_get_image_size(ota);
        if (total <= 0) total = pedido.tam;
        int pct = total > 0 ? (int)(100LL * esp_https_ota_get_image_len_read(ota) / total) : 0;
        if (pct >= siguiente_aviso) {
            avisar("descargando", pedido.version, pct, NULL);
            siguiente_aviso = pct - pct % OTA_AVISO_CADA_PCT + OTA_AVISO_CADA_PCT;
        }
    }
    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(ota)) {
        error = "la descarga se cortó";
        goto fin;
    }
    // finish valida la imagen completa (checksum + SHA-256 interno) y la marca
    // como la que arrancará la próxima vez.
    err = esp_https_ota_finish(ota);
    ota = NULL;
    if (err != ESP_OK) {
        error = err == ESP_ERR_OTA_VALIDATE_FAILED ? "la imagen llegó dañada" : "no se pudo instalar";
        goto fin;
    }

    ESP_LOGI(TAG, "versión %s instalada: reiniciando", pedido.version);
    avisar("instalado", pedido.version, 100, "reiniciando");
    vTaskDelay(pdMS_TO_TICKS(1500));  // que el aviso alcance a salir por el WebSocket
    esp_restart();

fin:
    if (ota) esp_https_ota_abort(ota);
    ESP_LOGE(TAG, "OTA fallida: %s", error);
    avisar("error", pedido.version, -1, error);
    en_curso = false;
    vTaskDelete(NULL);
}

bool ota_iniciar(const char *ruta, const char *version, int tam)
{
    if (en_curso) {
        avisar("error", version, -1, "ya hay una actualización en curso");
        return false;
    }
    // Solo rutas de la propia API: un comando no puede mandar a la cámara a otro servidor.
    if (strncmp(ruta, "/ota/firmware/", 14) != 0 || version[0] == '\0') {
        avisar("error", version, -1, "pedido de OTA inválido");
        return false;
    }
    if (enlace_api_url(pedido.url, sizeof(pedido.url), ruta) < 0) return false;
    strlcpy(pedido.version, version, sizeof(pedido.version));
    pedido.tam = tam;
    en_curso = true;
    // Núcleo 0 (el de la red): la captura de la cámara (núcleo 1) no se entera; main.c
    // deja de mandar vídeo mientras dura la descarga.
    if (xTaskCreatePinnedToCore(tarea_ota, "ota", OTA_PILA, NULL, PRIORIDAD_ENLACE, NULL, NUCLEO_SISTEMA) != pdPASS) {
        en_curso = false;
        avisar("error", version, -1, "sin memoria para la tarea OTA");
        return false;
    }
    return true;
}
