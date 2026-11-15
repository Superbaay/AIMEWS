/*
 * ============================================================================
 *  Camara_12fps_ws — El ejemplo Camara_12fps enviando su vídeo a la API
 * ============================================================================
 *
 *  Es el mismo ritmo de captura que Camara_12fps (UXGA, 12 fps con plazos
 *  absolutos, sensor OV3660, misma librería components/esp32cam), y en el
 *  hueco "AQUÍ va tu aplicación" se envía cada frame al router
 *  API/Routers_BM/camera_rt.py:
 *
 *    1. Se conecta al WiFi.
 *    2. Pide su token:  POST /machine_rt/token_machine  (usuario y contraseña
 *       de la máquina, en menuconfig).
 *    3. Abre  ws://<api>/camera_rt/ws/machine  y lo PRIMERO que manda es
 *       {"token": "..."}. Hasta recibir {"type": "auth_ok"} no envía vídeo.
 *    4. Cada JPEG sale como un mensaje BINARIO. Si el WiFi va más lento que la
 *       cámara, se envía siempre el frame MÁS RECIENTE y se saltan los demás
 *       (mejor 8 fps al día que 12 fps con segundos de retraso).
 *    5. Escucha comandos del panel: {"type":"set","name":"quality","value":20}
 *       y {"type":"flash","value":0..255}; después de cada uno manda su estado.
 *
 *  Configurar (WiFi, IP de la API, máquina, calidad):  idf.py menuconfig
 *      -> "Camara 12 fps por WebSocket"
 *  Compilar y flashear:   idf.py -p COM6 flash monitor   (salir: Ctrl+])
 * ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp32cam.h"
#include "net_wifi.h"

static const char *TAG = "camara_ws";

/* ---------------------------------------------------------------------------
 *  Configuración
 * ------------------------------------------------------------------------- */

#define FPS_OBJETIVO      12
#define RESOLUCION        FRAMESIZE_UXGA            // 1600x1200, igual que Camara_12fps
#define TAM_FRAME_MAX     (256 * 1024)              // = max_frame_bytes del router
#define TIMEOUT_ENVIO     pdMS_TO_TICKS(3000)

// Con TLS (API en internet, p. ej. tras un túnel de Cloudflare) cambia el esquema
// y se valida el certificado del servidor con el bundle de CAs de ESP-IDF.
#if CONFIG_CAM_API_TLS
#define ESQUEMA_HTTP      "https"
#define ESQUEMA_WS        "wss"
#define CRT_BUNDLE        esp_crt_bundle_attach
#else
#define ESQUEMA_HTTP      "http"
#define ESQUEMA_WS        "ws"
#define CRT_BUNDLE        NULL
#endif

// Bits del grupo de eventos
#define BIT_AUTORIZADO    BIT1   // la API respondió auth_ok: ya se puede enviar vídeo
#define BIT_TOKEN_MALO    BIT2   // la API cerró con 1008: hay que pedir otro token

static EventGroupHandle_t eventos;
static esp_websocket_client_handle_t ws;
static char token[512];

/* ---------------------------------------------------------------------------
 *  WiFi: librería net_wifi (esp32-https-ws). Reconecta sola con backoff,
 *  desactiva el ahorro de energía y registra el motivo de cada desconexión.
 * ------------------------------------------------------------------------- */

static void iniciar_wifi(void)
{
    const net_wifi_config_t cfg = {
        .ssid = CONFIG_CAM_WIFI_SSID,
        .password = CONFIG_CAM_WIFI_PASSWORD,
        .hostname = CONFIG_CAM_MACHINE_NAME,
    };
    ESP_ERROR_CHECK(net_wifi_start(&cfg));
}

/* ---------------------------------------------------------------------------
 *  Mini lector de JSON (solo para los comandos cortos que manda el router)
 * ------------------------------------------------------------------------- */

// Devuelve un puntero al valor de "clave" (tolera espacios: Python manda "a": 1).
static const char *json_valor(const char *json, const char *clave)
{
    char patron[40];
    snprintf(patron, sizeof(patron), "\"%s\"", clave);
    const char *p = strstr(json, patron);
    if (p == NULL) {
        return NULL;
    }
    p += strlen(patron);
    while (*p == ' ' || *p == ':') {
        p++;
    }
    return p;
}

static bool json_texto(const char *json, const char *clave, char *salida, size_t tam)
{
    const char *p = json_valor(json, clave);
    if (p == NULL || *p != '"') {
        return false;
    }
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < tam) {
        salida[n++] = *p++;
    }
    salida[n] = '\0';
    return *p == '"';
}

static bool json_entero(const char *json, const char *clave, int *valor)
{
    const char *p = json_valor(json, clave);
    char *fin;
    if (p == NULL) {
        return false;
    }
    long v = strtol(p, &fin, 10);
    if (fin == p) {
        return false;
    }
    *valor = (int)v;
    return true;
}

/* ---------------------------------------------------------------------------
 *  Token de la máquina: POST /machine_rt/token_machine
 * ------------------------------------------------------------------------- */

// application/x-www-form-urlencoded: una contraseña con '&' o '=' rompería el formulario.
static void urlencode(const char *entrada, char *salida, size_t tam)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (; *entrada && n + 4 < tam; entrada++) {
        unsigned char c = (unsigned char)*entrada;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == '-' || c == '_' || c == '.' || c == '~') {
            salida[n++] = (char)c;
        } else {
            salida[n++] = '%';
            salida[n++] = hex[c >> 4];
            salida[n++] = hex[c & 15];
        }
    }
    salida[n] = '\0';
}

static bool pedir_token(void)
{
    char url[128], usuario[96], clave[192], cuerpo[320], respuesta[640];
    snprintf(url, sizeof(url), ESQUEMA_HTTP "://%s:%d/machine_rt/token_machine",
             CONFIG_CAM_API_HOST, CONFIG_CAM_API_PORT);
    urlencode(CONFIG_CAM_MACHINE_NAME, usuario, sizeof(usuario));
    urlencode(CONFIG_CAM_MACHINE_PASSWORD, clave, sizeof(clave));
    int largo = snprintf(cuerpo, sizeof(cuerpo), "username=%s&password=%s", usuario, clave);

    esp_http_client_config_t cfg = {
        .url = url, .method = HTTP_METHOD_POST, .timeout_ms = 10000, .crt_bundle_attach = CRT_BUNDLE,
    };
    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    esp_http_client_set_header(http, "Content-Type", "application/x-www-form-urlencoded");
    bool ok = false;
    if (esp_http_client_open(http, largo) == ESP_OK
        && esp_http_client_write(http, cuerpo, largo) == largo
        && esp_http_client_fetch_headers(http) >= 0) {
        int codigo = esp_http_client_get_status_code(http);
        int n = esp_http_client_read_response(http, respuesta, sizeof(respuesta) - 1);
        respuesta[n > 0 ? n : 0] = '\0';
        // La API responde {"token": "eyJ..."}
        ok = codigo == 200 && json_texto(respuesta, "token", token, sizeof(token));
        if (!ok) {
            ESP_LOGE(TAG, "La API rechazó a la máquina '%s' (HTTP %d): %s",
                     CONFIG_CAM_MACHINE_NAME, codigo, respuesta);
        }
    } else {
        ESP_LOGE(TAG, "No se pudo conectar a %s", url);
    }
    esp_http_client_cleanup(http);
    return ok;
}

/* ---------------------------------------------------------------------------
 *  WebSocket
 * ------------------------------------------------------------------------- */

static void enviar_estado(void)
{
    char estado[640];
    int n = esp32cam_status_json(estado, sizeof(estado));
    if (n > 0) {
        esp_websocket_client_send_text(ws, estado, n, TIMEOUT_ENVIO);
    }
}

static void atender_texto(const char *json)
{
    char tipo[16], nombre[24];
    int valor;
    if (!json_texto(json, "type", tipo, sizeof(tipo))) {
        return;
    }
    if (strcmp(tipo, "auth_ok") == 0) {
        ESP_LOGI(TAG, "Token aceptado: empieza el envío de vídeo");
        xEventGroupSetBits(eventos, BIT_AUTORIZADO);
        enviar_estado();
    } else if (strcmp(tipo, "set") == 0 && json_texto(json, "name", nombre, sizeof(nombre))
               && json_entero(json, "value", &valor)) {
        esp_err_t err = esp32cam_set(nombre, valor);
        ESP_LOGI(TAG, "Panel: %s = %d -> %s", nombre, valor, esp_err_to_name(err));
        enviar_estado();
    } else if (strcmp(tipo, "flash") == 0 && json_entero(json, "value", &valor)) {
        esp32cam_flash((uint8_t)(valor < 0 ? 0 : valor > 255 ? 255 : valor));
    } else if (strcmp(tipo, "error") == 0) {
        ESP_LOGW(TAG, "La API respondió: %s", json);
    }
}

static void al_evento_ws(void *arg, esp_event_base_t base, int32_t id, void *datos)
{
    esp_websocket_event_data_t *d = datos;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED: {
        // Verificación inicial: el token tiene que ser el PRIMER mensaje (la API espera 5 s).
        char primero[sizeof(token) + 16];
        int n = snprintf(primero, sizeof(primero), "{\"token\":\"%s\"}", token);
        esp_websocket_client_send_text(ws, primero, n, TIMEOUT_ENVIO);
        break;
    }
    case WEBSOCKET_EVENT_DATA:
        if (d->op_code == 0x1 && d->payload_offset == 0 && d->data_len == d->payload_len
            && d->data_len < 512) {
            char texto[512];
            memcpy(texto, d->data_ptr, d->data_len);
            texto[d->data_len] = '\0';
            atender_texto(texto);
        } else if (d->op_code == 0x8 && d->data_len >= 2) {
            int codigo = ((uint8_t)d->data_ptr[0] << 8) | (uint8_t)d->data_ptr[1];
            ESP_LOGW(TAG, "La API cerró la conexión (código %d)", codigo);
            if (codigo == 1008) {   // policy violation = token inválido o vencido (dura 24 h)
                xEventGroupSetBits(eventos, BIT_TOKEN_MALO);
            }
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        xEventGroupClearBits(eventos, BIT_AUTORIZADO);
        break;
    default:
        break;
    }
}

static void iniciar_websocket(void)
{
    char uri[128];
    snprintf(uri, sizeof(uri), ESQUEMA_WS "://%s:%d/camera_rt/ws/machine", CONFIG_CAM_API_HOST,
             CONFIG_CAM_API_PORT);
    const esp_websocket_client_config_t cfg = {
        .uri = uri,
        .crt_bundle_attach = CRT_BUNDLE,
        .buffer_size = 16384,          // trozos por escritura: = registro TLS de 16 KB (menos viajes)
        .task_stack = 8192,            // con TLS el handshake de mbedTLS pide bastante pila
        .reconnect_timeout_ms = 3000,  // se reconecta solo y vuelve a mandar el token
        .network_timeout_ms = 10000,
    };
    ws = esp_websocket_client_init(&cfg);
    esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, al_evento_ws, NULL);
    ESP_ERROR_CHECK(esp_websocket_client_start(ws));
    ESP_LOGI(TAG, "Conectando a %s como '%s'", uri, CONFIG_CAM_MACHINE_NAME);
}

/* ---------------------------------------------------------------------------
 *  Envío (núcleo 0, junto al WiFi): siempre el frame más reciente
 * ------------------------------------------------------------------------- */

static uint8_t *copia_jpeg;              // frame pendiente de enviar (PSRAM)
static size_t copia_len;
static volatile bool copia_ocupada;      // true mientras tarea_envio la usa
static TaskHandle_t tarea_envio_handle;
static volatile uint32_t enviados, fallos_envio;

static void tarea_envio(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (esp_websocket_client_send_bin(ws, (const char *)copia_jpeg, copia_len, TIMEOUT_ENVIO)
            == (int)copia_len) {
            enviados++;
        } else {
            fallos_envio++;
        }
        copia_ocupada = false;
    }
}

/* ---------------------------------------------------------------------------
 *  Captura a ritmo fijo (núcleo 1): idéntica a Camara_12fps
 * ------------------------------------------------------------------------- */

static void tarea_camara(void *arg)
{
    const int64_t periodo_us = 1000000LL / FPS_OBJETIVO;
    int64_t siguiente_us = esp_timer_get_time();
    int64_t inicio_segundo_us = siguiente_us;
    uint32_t capturados = 0, saltados = 0, bytes = 0;

    for (;;) {
        camera_fb_t *fb = esp32cam_capture();
        int64_t ahora_us = esp_timer_get_time();

        if (fb != NULL) {
            capturados++;
            bytes += fb->len;
            // AQUÍ va tu aplicación (ver Camara_12fps): copiar y soltar el buffer
            // enseguida, porque enviar por WiFi tarda más que los ~70 ms que el
            // driver nos deja quedarnos con él.
            bool autorizado = xEventGroupGetBits(eventos) & BIT_AUTORIZADO;
            if (autorizado && !copia_ocupada && fb->len <= TAM_FRAME_MAX) {
                memcpy(copia_jpeg, fb->buf, fb->len);
                copia_len = fb->len;
                copia_ocupada = true;
                xTaskNotifyGive(tarea_envio_handle);
            } else if (autorizado) {
                saltados++;   // el envío anterior aún no termina: este frame no sale
            }
            esp32cam_release(fb);
        }

        if (ahora_us - inicio_segundo_us >= 1000000) {
            float s = (ahora_us - inicio_segundo_us) / 1e6f;
            static uint32_t enviados_antes;
            uint32_t enviados_ahora = enviados;
            ESP_LOGI(TAG, "cámara %4.1f fps | enviados %4.1f fps | saltados %lu | %4lu KB/s | "
                     "fallos de envío %lu%s",
                     capturados / s, (enviados_ahora - enviados_antes) / s,
                     (unsigned long)saltados, (unsigned long)(bytes / s / 1024),
                     (unsigned long)fallos_envio,
                     (xEventGroupGetBits(eventos) & BIT_AUTORIZADO) ? "" : " | sin conexión con la API");
            enviados_antes = enviados_ahora;
            capturados = saltados = bytes = 0;
            inicio_segundo_us = ahora_us;
        }

        siguiente_us += periodo_us;
        int64_t espera_us = siguiente_us - esp_timer_get_time();
        if (espera_us > 0) {
            vTaskDelay(pdMS_TO_TICKS((espera_us + 500) / 1000));
        } else if (espera_us < -periodo_us) {
            siguiente_us = esp_timer_get_time();
        }
    }
}

/* ---------------------------------------------------------------------------
 *  Arranque
 * ------------------------------------------------------------------------- */

void app_main(void)
{
    esp_err_t err = nvs_flash_init();   // el WiFi guarda su calibración en NVS
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    eventos = xEventGroupCreate();

    const esp32cam_config_t cfg = {
        .frame_size = RESOLUCION,
        .jpeg_quality = CONFIG_CAM_JPEG_QUALITY,
        .fb_count = 2,
        // .xclk_hz = 0 -> 19 MHz: a 20 MHz el WiFi pierde 30-40 % de paquetes (esp32cam.h)
    };
    ESP_ERROR_CHECK(esp32cam_init(&cfg));
    copia_jpeg = heap_caps_malloc(TAM_FRAME_MAX, MALLOC_CAP_SPIRAM);
    if (copia_jpeg == NULL) {
        ESP_LOGE(TAG, "Sin PSRAM para el buffer de envío");
        return;
    }
    ESP_LOGI(TAG, "Sensor %s, %s, calidad %d, objetivo %d fps", esp32cam_sensor_name(),
             esp32cam_framesize_name(RESOLUCION), CONFIG_CAM_JPEG_QUALITY, FPS_OBJETIVO);

    iniciar_wifi();
    while (!net_wifi_wait_connected(10000)) {
        ESP_LOGW(TAG, "Esperando WiFi \"%s\"...", CONFIG_CAM_WIFI_SSID);
    }
    while (!pedir_token()) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    iniciar_websocket();

    // 6 KB: send_bin cifra el frame (TLS) dentro de esta tarea
    xTaskCreatePinnedToCore(tarea_envio, "envio", 6144, NULL, 4, &tarea_envio_handle, 0);
    xTaskCreatePinnedToCore(tarea_camara, "camara", 4096, NULL, 5, NULL, 1);

    // Si la API rechaza el token (vencido a las 24 h o máquina dada de baja), pide otro.
    for (;;) {
        xEventGroupWaitBits(eventos, BIT_TOKEN_MALO, pdTRUE, pdTRUE, portMAX_DELAY);
        ESP_LOGW(TAG, "Pidiendo un token nuevo");
        while (!pedir_token()) {
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
        // El cliente ya se está reconectando solo; con el token nuevo entrará bien.
    }
}
