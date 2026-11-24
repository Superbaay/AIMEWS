#include "enlace_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp32s3cam.h"
#include "net_wifi.h"
#include "ota_camara.h"

static const char *TAG = "enlace";

// Un envío que no termina a tiempo hace que el cliente dé la conexión por rota y
// reconecte. Por internet (hotspot + TLS + Cloudflare) 1 s no siempre alcanza.
#define TIMEOUT_ENVIO        pdMS_TO_TICKS(5000)
#define SIN_CONEXION_MAX_MS  60000   // vigilante del enlace (ver tarea_enlace)
#define TAM_FRAME_MAX        (256 * 1024)   // = max_frame_bytes de camera_rt.py

#if CONFIG_CAM_API_TLS
#define ESQUEMA_HTTP "https"
#define ESQUEMA_WS   "wss"
#define CRT_BUNDLE   esp_crt_bundle_attach
#else
#define ESQUEMA_HTTP "http"
#define ESQUEMA_WS   "ws"
#define CRT_BUNDLE   NULL
#endif

#define BIT_VIDEO_OK    BIT0   // camera_rt respondió auth_ok
#define BIT_CONTROL_OK  BIT1   // drone_ws_rt respondió auth_ok
#define BIT_TOKEN_MALO  BIT2   // alguna cerró con 1008: pedir otro token

static EventGroupHandle_t eventos;
static esp_websocket_client_handle_t ws_video, ws_control;
static char token[512];
// La tarea de vídeo envía por ws_video mientras el vigilante puede destruirlo:
// los dos pasan por este candado (si no, el envío usaría un cliente ya liberado).
static SemaphoreHandle_t candado_video;

bool enlace_api_video_listo(void)
{
    return eventos && (xEventGroupGetBits(eventos) & BIT_VIDEO_OK);
}

bool enlace_api_control_listo(void)
{
    return eventos && (xEventGroupGetBits(eventos) & BIT_CONTROL_OK);
}

const char *enlace_api_token(void)
{
    return token;
}

int enlace_api_url(char *salida, size_t tam, const char *ruta)
{
    int n = snprintf(salida, tam, ESQUEMA_HTTP "://%s:%d%s", CONFIG_CAM_API_HOST, CONFIG_CAM_API_PORT, ruta);
    return n < (int)tam ? n : -1;
}

/* ---------------------------------------------------------------------------
 *  Mini lector de JSON (solo para los mensajes cortos de la API)
 * ------------------------------------------------------------------------- */

// Puntero al valor de "clave" (tolera espacios: Python manda "a": 1).
static const char *json_valor(const char *json, const char *clave)
{
    char patron[40];
    snprintf(patron, sizeof(patron), "\"%s\"", clave);
    const char *p = strstr(json, patron);
    if (p == NULL) return NULL;
    p += strlen(patron);
    while (*p == ' ' || *p == ':') p++;
    return p;
}

static bool json_entero(const char *json, const char *clave, int *valor)
{
    const char *p = json_valor(json, clave);
    if (p == NULL || !(*p == '-' || (*p >= '0' && *p <= '9'))) return false;
    *valor = atoi(p);
    return true;
}

static bool json_texto(const char *json, const char *clave, char *salida, size_t tam)
{
    const char *p = json_valor(json, clave);
    if (p == NULL || *p != '"') return false;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < tam) salida[n++] = *p++;
    salida[n] = '\0';
    return *p == '"';
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
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'
            || c == '.' || c == '~') {
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
    enlace_api_url(url, sizeof(url), "/machine_rt/token_machine");
    urlencode(CONFIG_CAM_MACHINE_NAME, usuario, sizeof(usuario));
    urlencode(CONFIG_CAM_MACHINE_PASSWORD, clave, sizeof(clave));
    int largo = snprintf(cuerpo, sizeof(cuerpo), "username=%s&password=%s", usuario, clave);

    esp_http_client_config_t cfg = {
        .url = url, .method = HTTP_METHOD_POST, .timeout_ms = 10000, .crt_bundle_attach = CRT_BUNDLE,
    };
    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    esp_http_client_set_header(http, "Content-Type", "application/x-www-form-urlencoded");
    bool ok = false;
    if (esp_http_client_open(http, largo) == ESP_OK && esp_http_client_write(http, cuerpo, largo) == largo
        && esp_http_client_fetch_headers(http) >= 0) {
        int codigo = esp_http_client_get_status_code(http);
        int n = esp_http_client_read_response(http, respuesta, sizeof(respuesta) - 1);
        respuesta[n > 0 ? n : 0] = '\0';
        ok = codigo == 200 && json_texto(respuesta, "token", token, sizeof(token));
        if (!ok && (codigo == 502 || codigo == 503 || codigo == 530)) {
            // 530 / "error code: 1033" = Cloudflare no encuentra el túnel: la PC de la
            // API (o cloudflared) está apagada. No es culpa de la máquina.
            ESP_LOGW(TAG, "API o túnel apagados (HTTP %d): reintentando", codigo);
        } else if (!ok) {
            ESP_LOGE(TAG, "la API rechazó a la máquina '%s' (HTTP %d): %s", CONFIG_CAM_MACHINE_NAME, codigo,
                     respuesta);
        }
    } else {
        ESP_LOGE(TAG, "no se pudo conectar a %s (¿API o túnel apagados?)", url);
    }
    esp_http_client_cleanup(http);
    return ok;
}

/* ---------------------------------------------------------------------------
 *  Mensajes que manda la cámara
 * ------------------------------------------------------------------------- */

static void enviar_texto(esp_websocket_client_handle_t ws, const char *texto, int largo)
{
    if (largo > 0 && ws != NULL && esp_websocket_client_is_connected(ws)) {
        esp_websocket_client_send_text(ws, texto, largo, TIMEOUT_ENVIO);
    }
}

void enlace_api_enviar_control(const char *texto, int largo)
{
    enviar_texto(ws_control, texto, largo);
}

esp_err_t enlace_api_enviar_frame(const uint8_t *jpeg, size_t largo)
{
    if (!enlace_api_video_listo() || largo > TAM_FRAME_MAX) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(candado_video, 0) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;   // el vigilante está rehaciendo el cliente
    }
    int n = ws_video ? esp_websocket_client_send_bin(ws_video, (const char *)jpeg, largo, TIMEOUT_ENVIO) : -1;
    xSemaphoreGive(candado_video);
    return n == (int)largo ? ESP_OK : ESP_FAIL;
}

/* Estado de la cámara (status de la librería) para los visores del panel. */
static void enviar_estado_camara(void)
{
    char j[900];
    int n = esp32s3cam_status_json(j, sizeof(j));
    enviar_texto(ws_video, j, n);
}

/* Lo que la cámara entiende por el canal de control: solo "ota". La API solo le
 * reenvía las acciones declaradas (drone_ws_rt.py) y solo le ofrece OTA así. */
static void enviar_estado_dron(void)
{
    char m[256];
    int n = snprintf(m, sizeof(m),
                     "{\"tipo\":\"estado_dron\",\"version\":\"%s\",\"maquina\":\"%s\","
                     "\"sensores\":[\"camera\"],\"acciones\":[\"ota\"]}",
                     ota_version_actual(), CONFIG_CAM_MACHINE_NAME);
    enviar_texto(ws_control, m, n);
}

/* ---------------------------------------------------------------------------
 *  Mensajes que llegan
 * ------------------------------------------------------------------------- */

static void atender_video(const char *json)
{
    char tipo[16], nombre[24];
    int valor;
    if (!json_texto(json, "type", tipo, sizeof(tipo))) return;
    if (strcmp(tipo, "auth_ok") == 0) {
        ESP_LOGI(TAG, "vídeo: token aceptado, empieza el envío");
        xEventGroupSetBits(eventos, BIT_VIDEO_OK);
        enviar_estado_camara();
    } else if (strcmp(tipo, "set") == 0 && json_texto(json, "name", nombre, sizeof(nombre))
               && json_entero(json, "value", &valor)) {
        // camera_rt.py ya filtró el nombre; la librería vuelve a validar el valor.
        esp_err_t err = esp32s3cam_set(nombre, valor);
        ESP_LOGI(TAG, "panel: %s = %d -> %s", nombre, valor, esp_err_to_name(err));
        enviar_estado_camara();
    } else if (strcmp(tipo, "flash") == 0) {
        ESP_LOGI(TAG, "panel: flash ignorado (esta placa no tiene LED de flash)");
        enviar_estado_camara();
    } else if (strcmp(tipo, "error") == 0) {
        ESP_LOGW(TAG, "vídeo: la API respondió %s", json);
    }
}

static void atender_control(const char *json)
{
    char tipo[24], accion[32];
    if (!json_texto(json, "tipo", tipo, sizeof(tipo))) return;
    if (strcmp(tipo, "auth_ok") == 0) {
        ESP_LOGI(TAG, "control: token aceptado (OTA disponible)");
        xEventGroupSetBits(eventos, BIT_CONTROL_OK);
        enviar_estado_dron();
        ota_confirmar();   // si veníamos de una OTA: la versión nueva sí llega a la API
    } else if (strcmp(tipo, "comando") == 0 && json_texto(json, "action", accion, sizeof(accion))) {
        ESP_LOGI(TAG, "comando del panel: %s", accion);
        char ruta[96], version[32];
        int tam = 0;
        if (strcmp(accion, "ota") == 0 && json_texto(json, "url", ruta, sizeof(ruta))
            && json_texto(json, "version", version, sizeof(version))) {
            json_entero(json, "size", &tam);
            ota_iniciar(ruta, version, tam);
        }
    } else if (strcmp(tipo, "error") == 0) {
        ESP_LOGW(TAG, "control: la API respondió %s", json);
    }
}

/* Mismo manejador para los dos clientes: arg dice cuál es. */
static void al_evento_ws(void *arg, esp_event_base_t base, int32_t id, void *datos)
{
    const bool es_video = arg == (void *)1;
    esp_websocket_client_handle_t ws = es_video ? ws_video : ws_control;
    esp_websocket_event_data_t *d = datos;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED: {
        // El token tiene que ser el PRIMER mensaje (la API espera 5 s).
        char primero[sizeof(token) + 16];
        int n = snprintf(primero, sizeof(primero), "{\"token\":\"%s\"}", token);
        esp_websocket_client_send_text(ws, primero, n, TIMEOUT_ENVIO);
        break;
    }
    case WEBSOCKET_EVENT_DATA:
        if (d->op_code == 0x1 && d->payload_offset == 0 && d->data_len == d->payload_len && d->data_len < 512) {
            char texto[512];
            memcpy(texto, d->data_ptr, d->data_len);
            texto[d->data_len] = '\0';
            es_video ? atender_video(texto) : atender_control(texto);
        } else if (d->op_code == 0x8 && d->data_len >= 2) {
            int codigo = ((uint8_t)d->data_ptr[0] << 8) | (uint8_t)d->data_ptr[1];
            ESP_LOGW(TAG, "%s: la API cerró la conexión (código %d)", es_video ? "vídeo" : "control", codigo);
            if (codigo == 1008) {   // token inválido o vencido (dura 24 h)
                xEventGroupSetBits(eventos, BIT_TOKEN_MALO);
            }
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        xEventGroupClearBits(eventos, es_video ? BIT_VIDEO_OK : BIT_CONTROL_OK);
        break;
    default:
        break;
    }
}

static esp_websocket_client_handle_t abrir_ws(const char *ruta, bool es_video)
{
    char uri[128];
    snprintf(uri, sizeof(uri), ESQUEMA_WS "://%s:%d%s", CONFIG_CAM_API_HOST, CONFIG_CAM_API_PORT, ruta);
    const esp_websocket_client_config_t cfg = {
        .uri = uri,
        .crt_bundle_attach = CRT_BUNDLE,
        .task_stack = 8192,            // con TLS el handshake de mbedTLS pide bastante pila
        .task_prio = PRIORIDAD_ENLACE,
        .buffer_size = es_video ? 8192 : 1024,   // frames de ~20 KB: menos vueltas por envío
        .reconnect_timeout_ms = 3000,  // se reconecta solo y vuelve a mandar el token
        .network_timeout_ms = 10000,
        .ping_interval_sec = 10,
    };
    esp_websocket_client_handle_t ws = esp_websocket_client_init(&cfg);
    esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, al_evento_ws, (void *)(es_video ? 1 : 0));
    ESP_ERROR_CHECK(esp_websocket_client_start(ws));
    ESP_LOGI(TAG, "conectando a %s como '%s'", uri, CONFIG_CAM_MACHINE_NAME);
    return ws;
}

/* ---------------------------------------------------------------------------
 *  Tarea del enlace (núcleo 0)
 * ------------------------------------------------------------------------- */

static void esperar_red_y_token(void)
{
    while (!net_wifi_wait_connected(10000)) {
        ESP_LOGW(TAG, "esperando WiFi (\"%s\"%s)...", CONFIG_CAM_WIFI_SSID,
                 CONFIG_CAM_WIFI_SSID_2[0] ? " o \"" CONFIG_CAM_WIFI_SSID_2 "\"" : "");
    }
    while (!pedir_token()) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static void tarea_enlace(void *arg)
{
    const net_wifi_config_t wifi = {
        .ssid = CONFIG_CAM_WIFI_SSID,
        .password = CONFIG_CAM_WIFI_PASSWORD,
        .ssid2 = CONFIG_CAM_WIFI_SSID_2,          // "" = solo la primera
        .password2 = CONFIG_CAM_WIFI_PASSWORD_2,
        .hostname = CONFIG_CAM_MACHINE_NAME,
    };
    ESP_ERROR_CHECK(net_wifi_start(&wifi));
    // Sin ahorro de energía: con él el AP agrupa los paquetes por beacon y el vídeo
    // sostenido cae a una fracción.
    esp_wifi_set_ps(WIFI_PS_NONE);
    esperar_red_y_token();
    ws_video = abrir_ws("/camera_rt/ws/machine", true);
    ws_control = abrir_ws("/ws/machine", false);

    TickType_t ultimo_conectado = xTaskGetTickCount();
    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(eventos, BIT_TOKEN_MALO, pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
        if (bits & BIT_TOKEN_MALO) {
            // Token vencido (24 h) o máquina dada de baja: los clientes se reconectan
            // solos y mandan el token que haya en ese momento.
            ESP_LOGW(TAG, "pidiendo un token nuevo");
            while (!pedir_token()) {
                vTaskDelay(pdMS_TO_TICKS(5000));
            }
        }
        // Vigilante: en el dron se midió que, tras horas con la API caída, el cliente
        // dejó de reintentar. Sin los DOS canales autorizados por SIN_CONEXION_MAX_MS
        // se destruyen y se crean de cero con un token nuevo.
        if (enlace_api_video_listo() && enlace_api_control_listo()) {
            ultimo_conectado = xTaskGetTickCount();
        } else if (!ota_en_curso()
                   && xTaskGetTickCount() - ultimo_conectado > pdMS_TO_TICKS(SIN_CONEXION_MAX_MS)) {
            ESP_LOGW(TAG, "%d s sin los dos canales: reiniciando los WebSockets", SIN_CONEXION_MAX_MS / 1000);
            xEventGroupClearBits(eventos, BIT_VIDEO_OK | BIT_CONTROL_OK);
            xSemaphoreTake(candado_video, portMAX_DELAY);   // espera a que termine un envío en curso
            esp_websocket_client_destroy(ws_video);
            ws_video = NULL;
            xSemaphoreGive(candado_video);
            esp_websocket_client_destroy(ws_control);
            ws_control = NULL;
            esperar_red_y_token();
            xSemaphoreTake(candado_video, portMAX_DELAY);
            ws_video = abrir_ws("/camera_rt/ws/machine", true);
            xSemaphoreGive(candado_video);
            ws_control = abrir_ws("/ws/machine", false);
            ultimo_conectado = xTaskGetTickCount();
        }
    }
}

void enlace_api_iniciar(void)
{
    eventos = xEventGroupCreate();
    candado_video = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(tarea_enlace, "enlace", 8192, NULL, PRIORIDAD_ENLACE, NULL, NUCLEO_SISTEMA);
}
