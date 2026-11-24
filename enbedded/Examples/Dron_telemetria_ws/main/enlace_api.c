#include "enlace_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "estado_dron.h"
#include "net_wifi.h"
#include "ota_dron.h"
#include "sensores.h"

static const char *TAG = "enlace";

// Un envío que no termina a tiempo hace que el cliente dé la conexión por rota y
// reconecte (~7 s sin datos). Por internet (WiFi -72 dBm + TLS + Cloudflare) 1 s
// no alcanzaba a veces: con 5 s el envío lento se completa y la conexión sigue.
#define TIMEOUT_ENVIO     pdMS_TO_TICKS(5000)
#define TAM_TELEMETRIA    1024
#define SIN_CONEXION_MAX_MS 60000   // vigilante del enlace (ver tarea_enlace)

// Con TLS (API detrás del túnel de Cloudflare) cambia el esquema y se valida
// el certificado del servidor con el bundle de CAs de ESP-IDF.
#if CONFIG_DRON_API_TLS
#define ESQUEMA_HTTP      "https"
#define ESQUEMA_WS        "wss"
#define CRT_BUNDLE        esp_crt_bundle_attach
#else
#define ESQUEMA_HTTP      "http"
#define ESQUEMA_WS        "ws"
#define CRT_BUNDLE        NULL
#endif

#define BIT_AUTORIZADO    BIT0   // la API respondió auth_ok
#define BIT_TOKEN_MALO    BIT1   // la API cerró con 1008: pedir otro token

static EventGroupHandle_t eventos;
static esp_websocket_client_handle_t ws;
static char token[512];

bool enlace_api_conectado(void)
{
    return eventos && (xEventGroupGetBits(eventos) & BIT_AUTORIZADO);
}

/* ---------------------------------------------------------------------------
 *  Mini lector de JSON (solo para los mensajes cortos de la API)
 * ------------------------------------------------------------------------- */

// Devuelve un puntero al valor de "clave" (tolera espacios: Python manda "a": 1).
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

static int json_entero(const char *json, const char *clave, int defecto)
{
    const char *p = json_valor(json, clave);
    return (p && (*p == '-' || (*p >= '0' && *p <= '9'))) ? atoi(p) : defecto;
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
    snprintf(url, sizeof(url), ESQUEMA_HTTP "://%s:%d/machine_rt/token_machine", CONFIG_DRON_API_HOST,
             CONFIG_DRON_API_PORT);
    urlencode(CONFIG_DRON_MACHINE_NAME, usuario, sizeof(usuario));
    urlencode(CONFIG_DRON_MACHINE_PASSWORD, clave, sizeof(clave));
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
        if (!ok) {
            ESP_LOGE(TAG, "la API rechazó a la máquina '%s' (HTTP %d): %s", CONFIG_DRON_MACHINE_NAME, codigo,
                     respuesta);
        }
    } else {
        ESP_LOGE(TAG, "no se pudo conectar a %s", url);
    }
    esp_http_client_cleanup(http);
    return ok;
}

/* ---------------------------------------------------------------------------
 *  Mensajes que manda el dron
 * ------------------------------------------------------------------------- */

static void enviar(const char *texto, int largo)
{
    if (largo > 0 && ws != NULL && esp_websocket_client_is_connected(ws)) {
        esp_websocket_client_send_text(ws, texto, largo, TIMEOUT_ENVIO);
    }
}

void enlace_api_enviar(const char *texto, int largo)
{
    enviar(texto, largo);
}

const char *enlace_api_token(void)
{
    return token;
}

int enlace_api_url(char *salida, size_t tam, const char *ruta)
{
    int n = snprintf(salida, tam, ESQUEMA_HTTP "://%s:%d%s", CONFIG_DRON_API_HOST, CONFIG_DRON_API_PORT, ruta);
    return n < (int)tam ? n : -1;
}

static void enviar_estado_dron(void)
{
    // "acciones": lo que este firmware entiende. La API solo le reenvía eso
    // (drone_ws_rt.py) y solo le ofrece OTA si aparece "ota".
    // La versión sale del PROJECT_VER del CMakeLists: es la misma que lee la API del .bin.
    char m[320];
    int n = snprintf(m, sizeof(m),
                     "{\"tipo\":\"estado_dron\",\"version\":\"%s\",\"maquina\":\"%s\","
                     "\"sensores\":[\"acelerometer_gyroscope\",\"barometer\",\"gps\",\"lidar_distance\"],"
                     "\"acciones\":[\"iniciar_autoprueba\",\"cero_altura\",\"ota\"]}",
                     ota_version_actual(), CONFIG_DRON_MACHINE_NAME);
    enviar(m, n);
}

/* Autoprueba: cada campo bool de auto_test_machine (base_model_bm.py).
 * Un sensor pasa si está iniciado Y trabaja a su ritmo. Lo que este dron
 * aún no tiene montado (magnetómetro, LiDAR, servos, cámara, ESC) sale false. */
static void enviar_autoprueba(void)
{
    estado_dron_t e;
    estado_leer(&e);
    // IMU: antes se exigía "0 pérdidas" en el último segundo y fallaba al azar.
    // El comando llega por WebSocket justo cuando hay tráfico de red/TLS, y
    // un solo reinicio del FIFO (desalineado o desbordado) suma 36 "pérdidas"
    // de golpe aunque el sensor esté perfecto. Se tolera hasta el 5 %.
    bool imu_ok = e.imu.listo && e.imu.hz > 900.0f && e.imu.perdidas < 50;
    bool baro_ok = e.baro.listo && e.baro.hz > 50.0f && e.baro.presion_hpa > 300.0f && e.baro.presion_hpa < 1100.0f;
    // El GPS "funciona" si entrega medidas (5 Hz normal, 1 Hz en modo solo
    // lectura); el fix depende del cielo (en interiores no hay): va aparte.
    bool gps_ok = e.gps.listo && e.gps.hz > 0.5f;
    // El LiDAR pasa si llegan tramas a su ritmo (100 Hz pedidos). Que la medida sea válida depende de a qué apunte (cielo o más de
    // 12 m = sin medida): va aparte, como el fix del GPS.
    bool lidar_ok = e.lidar.listo && e.lidar.hz > 80.0f;

    char m[512];
    int n = snprintf(m, sizeof(m),
                     "{\"tipo\":\"autoprueba\",\"resultado\":{\"microcontroller\":true,"
                     "\"acelerometer_gyroscope\":%s,\"magnetometer\":false,\"barometer\":%s,\"gps\":%s,"
                     "\"lidar_distance\":%s,\"servo_motor\":false,\"camera\":false,\"esc_motor\":false},"
                     "\"detalle\":{\"imu_hz\":%.0f,\"imu_perdidas\":%lu,\"barometro_hz\":%.1f,\"gps_hz\":%.1f,"
                     "\"gps_fix\":%s,\"lidar_hz\":%.0f,\"lidar_valido\":%s}}",
                     imu_ok ? "true" : "false", baro_ok ? "true" : "false", gps_ok ? "true" : "false",
                     lidar_ok ? "true" : "false", e.imu.hz, (unsigned long)e.imu.perdidas, e.baro.hz, e.gps.hz,
                     e.gps.fix ? "true" : "false",
                     e.lidar.hz, e.lidar.valido ? "true" : "false");
    ESP_LOGI(TAG, "autoprueba: IMU %s (%.0f Hz, %lu perdidas), barómetro %s, GPS %s, LiDAR %s",
             imu_ok ? "OK" : "FALLA", e.imu.hz, (unsigned long)e.imu.perdidas,
             baro_ok ? "OK" : "FALLA", gps_ok ? "OK" : "FALLA", lidar_ok ? "OK" : "FALLA");
    enviar(m, n);
}

/* Telemetría con la forma del BaseModel sensor_data de la API. Solo van los
 * sensores que ya tienen datos: el panel pinta "—" en los que faltan. */
static int armar_telemetria(char *m, size_t tam)
{
    estado_dron_t e;
    estado_leer(&e);
    int n = snprintf(m, tam, "{\"tipo\":\"telemetria\",\"datos\":{");
    const char *coma = "";

    if (e.imu.listo) {
        n += snprintf(m + n, tam - n,
                      "%s\"acelerometer_gyroscope\":{\"pitch_angle\":%.2f,\"roll_angle\":%.2f,\"yaw_angle\":%.2f,"
                      "\"pitch_velocity\":%.2f,\"roll_velocity\":%.2f,\"yaw_velocity\":%.2f}",
                      coma, e.imu.cabeceo, e.imu.alabeo, e.imu.guinada, e.imu.vel_cabeceo, e.imu.vel_alabeo,
                      e.imu.vel_guinada);
        coma = ",";
    }
    if (e.baro.listo) {
        n += snprintf(m + n, tam - n,
                      "%s\"barometer\":{\"pressure\":%.2f,\"temperature\":%.2f,\"altitude\":%.2f}", coma,
                      e.baro.presion_hpa, e.baro.temp_c, e.baro.altura_m);
        coma = ",";
    }
    // El bloque gps va siempre que el GPS responda: los satélites y el HDOP
    // sirven justo cuando aún NO hay fix (para ver si está encontrando cielo).
    // Sin fix la posición va en null, nunca 0,0 (eso es un punto real del mar).
    if (e.gps.listo) {
        if (e.gps.fix) {
            n += snprintf(m + n, tam - n,
                          "%s\"gps\":{\"latitude\":%.7f,\"longitude\":%.7f,\"altitude\":%.1f,\"speed\":%.2f,", coma,
                          e.gps.latitud, e.gps.longitud, e.gps.altitud_m, e.gps.velocidad_ms);
        } else {
            n += snprintf(m + n, tam - n,
                          "%s\"gps\":{\"latitude\":null,\"longitude\":null,\"altitude\":null,\"speed\":null,", coma);
        }
        n += snprintf(m + n, tam - n,
                      "\"fix\":%s,\"fix_type\":%u,\"satellites\":%u,\"satellites_in_view\":%u,\"hdop\":%.1f}",
                      e.gps.fix ? "true" : "false", e.gps.tipo_fix, e.gps.satelites_usados, e.gps.satelites_vista,
                      e.gps.hdop);
        coma = ",";
    }
    // Sin medida válida no se manda distancia: el panel pinta "—" (mejor que un 0 falso).
    if (e.lidar.listo && e.lidar.valido) {
        n += snprintf(m + n, tam - n, "%s\"lidar_distance\":{\"distance\":%.2f,\"strength\":%u}", coma,
                      e.lidar.distancia_m, e.lidar.fuerza);
    }
    // Extras que el panel no muestra hoy, útiles para diagnosticar.
    n += snprintf(m + n, tam - n,
                  "},\"frecuencias\":{\"imu_hz\":%.0f,\"barometro_hz\":%.1f,\"gps_hz\":%.1f,\"lidar_hz\":%.0f},"
                  "\"diagnostico\":{\"imu_perdidas\":%lu,\"imu_lectura_max_us\":%lu,\"imu_errores_i2c\":%lu,"
                  "\"barometro_errores_i2c\":%lu,\"gps_frases_malas\":%lu,\"rssi\":%d}}",
                  e.imu.hz, e.baro.hz, e.gps.hz, e.lidar.hz, (unsigned long)e.imu.perdidas,
                  (unsigned long)e.imu.lectura_max_us, (unsigned long)e.imu.errores_i2c,
                  (unsigned long)e.baro.errores_i2c, (unsigned long)e.gps.frases_malas, net_wifi_get_rssi());
    return n < (int)tam ? n : -1;
}

/* ---------------------------------------------------------------------------
 *  WebSocket
 * ------------------------------------------------------------------------- */

static void atender_texto(const char *json)
{
    char tipo[24], accion[32];
    if (!json_texto(json, "tipo", tipo, sizeof(tipo))) return;

    if (strcmp(tipo, "auth_ok") == 0) {
        ESP_LOGI(TAG, "token aceptado: empieza la telemetría");
        xEventGroupSetBits(eventos, BIT_AUTORIZADO);
        enviar_estado_dron();
        ota_confirmar();  // si veníamos de una OTA: la versión nueva sí llega a la API
    } else if (strcmp(tipo, "comando") == 0 && json_texto(json, "action", accion, sizeof(accion))) {
        ESP_LOGI(TAG, "comando del panel: %s", accion);
        if (strcmp(accion, "iniciar_autoprueba") == 0) {
            enviar_autoprueba();
        } else if (strcmp(accion, "cero_altura") == 0) {
            estado_pedir_cero_altura();
        } else if (strcmp(accion, "ota") == 0) {
            // TODO cuando haya motores: rechazar la OTA si el dron está armado o volando.
            char ruta[96], version[32];
            if (json_texto(json, "url", ruta, sizeof(ruta)) && json_texto(json, "version", version, sizeof(version))) {
                ota_iniciar(ruta, version, json_entero(json, "size", 0));
            }
        }
    } else if (strcmp(tipo, "error") == 0) {
        ESP_LOGW(TAG, "la API respondió: %s", json);
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
        if (d->op_code == 0x1 && d->payload_offset == 0 && d->data_len == d->payload_len && d->data_len < 512) {
            char texto[512];
            memcpy(texto, d->data_ptr, d->data_len);
            texto[d->data_len] = '\0';
            atender_texto(texto);
        } else if (d->op_code == 0x8 && d->data_len >= 2) {
            int codigo = ((uint8_t)d->data_ptr[0] << 8) | (uint8_t)d->data_ptr[1];
            ESP_LOGW(TAG, "la API cerró la conexión (código %d)", codigo);
            if (codigo == 1008) {  // policy violation = token inválido o vencido (dura 24 h)
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
    snprintf(uri, sizeof(uri), ESQUEMA_WS "://%s:%d/ws/machine", CONFIG_DRON_API_HOST, CONFIG_DRON_API_PORT);
    const esp_websocket_client_config_t cfg = {
        .uri = uri,
        .crt_bundle_attach = CRT_BUNDLE,
        .task_stack = 8192,            // con TLS el handshake de mbedTLS pide bastante pila
        .task_prio = PRIORIDAD_ENLACE,
        .reconnect_timeout_ms = 3000,  // se reconecta solo y vuelve a mandar el token
        .network_timeout_ms = 10000,
        .ping_interval_sec = 10,
    };
    ws = esp_websocket_client_init(&cfg);
    esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, al_evento_ws, NULL);
    ESP_ERROR_CHECK(esp_websocket_client_start(ws));
    ESP_LOGI(TAG, "conectando a %s como '%s'", uri, CONFIG_DRON_MACHINE_NAME);
}

/* ---------------------------------------------------------------------------
 *  Tarea del enlace (núcleo 0, prioridad baja)
 * ------------------------------------------------------------------------- */

static void tarea_enlace(void *arg)
{
    const net_wifi_config_t wifi = {
        .ssid = CONFIG_DRON_WIFI_SSID,
        .password = CONFIG_DRON_WIFI_PASSWORD,
        .ssid2 = CONFIG_DRON_WIFI_SSID_2,          // "" = solo la primera
        .password2 = CONFIG_DRON_WIFI_PASSWORD_2,
        .hostname = CONFIG_DRON_MACHINE_NAME,
    };
    ESP_ERROR_CHECK(net_wifi_start(&wifi));
    while (!net_wifi_wait_connected(10000)) {
        ESP_LOGW(TAG, "esperando WiFi \"%s\"...", CONFIG_DRON_WIFI_SSID);
    }
    while (!pedir_token()) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    iniciar_websocket();

    char *mensaje = malloc(TAM_TELEMETRIA);
    const TickType_t periodo = pdMS_TO_TICKS(1000 / CONFIG_DRON_TELEMETRIA_HZ);
    TickType_t siguiente = xTaskGetTickCount();
    TickType_t ultimo_conectado = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&siguiente, periodo);

        // Vigilante del enlace: medido en la placa, tras horas con la API caída
        // (PC reiniciada, túnel sin origen) el cliente dejó de reintentar y no
        // volvió solo al regresar la API. Si pasa SIN_CONEXION_MAX_MS sin estar
        // autorizado, se destruye el cliente y se crea de cero con un token nuevo.
        if (enlace_api_conectado()) {
            ultimo_conectado = xTaskGetTickCount();
        } else if (xTaskGetTickCount() - ultimo_conectado > pdMS_TO_TICKS(SIN_CONEXION_MAX_MS)) {
            ESP_LOGW(TAG, "%d s sin conexión con la API: reiniciando el cliente WebSocket",
                     SIN_CONEXION_MAX_MS / 1000);
            esp_websocket_client_destroy(ws);
            ws = NULL;
            while (!net_wifi_wait_connected(10000)) {
                ESP_LOGW(TAG, "esperando WiFi \"%s\"...", CONFIG_DRON_WIFI_SSID);
            }
            while (!pedir_token()) vTaskDelay(pdMS_TO_TICKS(5000));
            xEventGroupClearBits(eventos, BIT_TOKEN_MALO);
            iniciar_websocket();
            ultimo_conectado = siguiente = xTaskGetTickCount();
            continue;
        }

        // Si la API rechazó el token (vencido a las 24 h o máquina dada de baja), pide otro.
        // El cliente se reconecta solo y entrará con el token nuevo.
        if (xEventGroupGetBits(eventos) & BIT_TOKEN_MALO) {
            xEventGroupClearBits(eventos, BIT_TOKEN_MALO);
            ESP_LOGW(TAG, "pidiendo un token nuevo");
            while (!pedir_token()) vTaskDelay(pdMS_TO_TICKS(5000));
            siguiente = xTaskGetTickCount();
            continue;
        }
        if (!enlace_api_conectado()) continue;

        int n = armar_telemetria(mensaje, TAM_TELEMETRIA);
        if (n > 0 && esp_websocket_client_send_text(ws, mensaje, n, TIMEOUT_ENVIO) < 0) {
            ESP_LOGW(TAG, "no se pudo enviar la telemetría");
        }
        // Si el envío tardó más de un periodo, no "recuperar" los perdidos con una
        // ráfaga: se sigue desde ahora (el panel solo quiere el dato más reciente).
        if (xTaskGetTickCount() - siguiente > periodo) siguiente = xTaskGetTickCount();
    }
}

void enlace_api_iniciar(void)
{
    eventos = xEventGroupCreate();
    // 6 KB: el envío cifra (TLS) dentro de esta tarea.
    xTaskCreatePinnedToCore(tarea_enlace, "enlace", 6144, NULL, PRIORIDAD_ENLACE, NULL, NUCLEO_SISTEMA);
}
