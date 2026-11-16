/**
 * @file net_wifi.c
 * @brief Implementación de la conexión WiFi con reconexión automática.
 *
 * Flujo de eventos:
 *   WIFI_EVENT_STA_START        -> esp_wifi_connect()
 *   IP_EVENT_STA_GOT_IP         -> marcar conectado, reiniciar la espera
 *   WIFI_EVENT_STA_DISCONNECTED -> marcar desconectado y programar un
 *                                  reintento con un temporizador (no se
 *                                  bloquea nunca el bucle de eventos)
 */
#include "net_wifi.h"

#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "net_wifi";

#define BIT_CONNECTED      BIT0
#define BACKOFF_MIN_MS     1000
#define BACKOFF_DEFAULT_MAX_MS 30000

static EventGroupHandle_t s_events;          // Bit BIT_CONNECTED = hay IP.
static esp_timer_handle_t s_retry_timer;     // Dispara el siguiente esp_wifi_connect().
static esp_netif_t *s_netif;
static uint32_t s_backoff_ms = BACKOFF_MIN_MS;
static uint32_t s_backoff_max_ms = BACKOFF_DEFAULT_MAX_MS;
// Momento (ms desde el arranque) en que se perdió la red. Es de 32 bits a
// propósito: en este chip se lee/escribe de forma atómica, así otra tarea nunca
// ve un valor a medias. La resta sin signo funciona aunque dé la vuelta (49 días).
static volatile uint32_t s_disconnected_since_ms;
static bool s_started;

/** Tiempo actual en milisegundos desde el arranque. */
static inline uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/** Callback del temporizador: intenta conectar de nuevo. */
static void retry_timer_cb(void *arg)
{
    (void)arg;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        // No es grave: llegará otro evento DISCONNECTED y se reprogramará.
        ESP_LOGW(TAG, "esp_wifi_connect falló: %s", esp_err_to_name(err));
    }
}

/** Programa el siguiente intento duplicando la espera (backoff exponencial). */
static void schedule_retry(void)
{
    esp_timer_stop(s_retry_timer);  // Por si ya había uno pendiente (ignora error).
    ESP_LOGI(TAG, "Reintentando en %lu ms", (unsigned long)s_backoff_ms);
    esp_timer_start_once(s_retry_timer, (uint64_t)s_backoff_ms * 1000ULL);

    s_backoff_ms *= 2;
    if (s_backoff_ms > s_backoff_max_ms) {
        s_backoff_ms = s_backoff_max_ms;
    }
}

/** Único manejador para todos los eventos WiFi e IP que nos interesan. */
static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi iniciado, conectando...");
        esp_wifi_connect();

    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = data;
        // Solo registramos el instante de la PRIMERA desconexión.
        if (xEventGroupGetBits(s_events) & BIT_CONNECTED) {
            s_disconnected_since_ms = now_ms();
        }
        xEventGroupClearBits(s_events, BIT_CONNECTED);
        ESP_LOGW(TAG, "Desconectado (motivo %d)", ev ? ev->reason : -1);
        schedule_retry();

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "Conectado, IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_backoff_ms = BACKOFF_MIN_MS;   // La próxima caída vuelve a empezar rápido.
        s_disconnected_since_ms = 0;
        xEventGroupSetBits(s_events, BIT_CONNECTED);

    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        // El router dejó de darnos IP sin desasociarnos: forzamos reconexión.
        ESP_LOGW(TAG, "IP perdida, forzando reconexión");
        if (xEventGroupGetBits(s_events) & BIT_CONNECTED) {
            s_disconnected_since_ms = now_ms();
        }
        xEventGroupClearBits(s_events, BIT_CONNECTED);
        esp_wifi_disconnect();  // Generará DISCONNECTED -> schedule_retry().
    }
}

/** Inicializa esp_netif y el bucle de eventos tolerando que ya existan. */
static esp_err_t init_netif_and_loop(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    return ESP_OK;
}

esp_err_t net_wifi_start(const net_wifi_config_t *config)
{
    if (config == NULL || config->ssid == NULL || config->ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (config->backoff_max_ms > 0) {
        s_backoff_max_ms = config->backoff_max_ms;
    }
    s_disconnected_since_ms = now_ms();  // Arrancamos "desconectados".

    // ESP_RETURN_ON_ERROR sería otra opción; aquí lo hacemos explícito para leerlo mejor.
    esp_err_t err = init_netif_and_loop();
    if (err != ESP_OK) {
        return err;
    }

    s_netif = esp_netif_create_default_wifi_sta();
    if (s_netif == NULL) {
        return ESP_FAIL;
    }
    if (config->hostname != NULL) {
        esp_netif_set_hostname(s_netif, config->hostname);
    }

    const esp_timer_create_args_t timer_args = {
        .callback = retry_timer_cb,
        .name = "wifi_retry",
    };
    err = esp_timer_create(&timer_args, &s_retry_timer);
    if (err != ESP_OK) {
        return err;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        return err;
    }

    // Registramos los eventos ANTES de arrancar para no perder ninguno.
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_LOST_IP, on_event, NULL, NULL);

    wifi_config_t wifi_cfg = { 0 };
    strlcpy((char *)wifi_cfg.sta.ssid, config->ssid, sizeof(wifi_cfg.sta.ssid));
    if (config->password != NULL) {
        strlcpy((char *)wifi_cfg.sta.password, config->password, sizeof(wifi_cfg.sta.password));
    }
    // Sin contraseña aceptamos red abierta; con contraseña exigimos al menos WPA2.
    wifi_cfg.sta.threshold.authmode =
        (wifi_cfg.sta.password[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    wifi_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;          // Soporta WPA3 si el router lo usa.
    wifi_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;      // Busca en todos los canales...
    wifi_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;  // ...y elige el AP con mejor señal.

    // El almacenamiento en RAM evita escribir la flash en cada arranque.
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        return err;
    }

    // Sin ahorro de energía: más consumo, pero menos latencia y menos cortes
    // en conexiones largas como el WebSocket.
    esp_wifi_set_ps(WIFI_PS_NONE);

    s_started = true;
    ESP_LOGI(TAG, "Conectando a \"%s\"", config->ssid);
    return ESP_OK;
}

bool net_wifi_is_connected(void)
{
    return s_events != NULL && (xEventGroupGetBits(s_events) & BIT_CONNECTED);
}

bool net_wifi_wait_connected(uint32_t timeout_ms)
{
    if (s_events == NULL) {
        return false;
    }
    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_CONNECTED, pdFALSE, pdTRUE, ticks);
    return (bits & BIT_CONNECTED) != 0;
}

int8_t net_wifi_get_rssi(void)
{
    wifi_ap_record_t ap;
    if (!net_wifi_is_connected() || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return 0;
    }
    return ap.rssi;
}

uint32_t net_wifi_get_disconnected_ms(void)
{
    uint32_t since = s_disconnected_since_ms;  // Copia local: puede cambiar en otra tarea.
    if (net_wifi_is_connected() || since == 0) {
        return 0;
    }
    return now_ms() - since;  // Correcto aunque el contador dé la vuelta.
}
