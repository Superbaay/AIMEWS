#include "neo6m.h"

#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "neo6m";

#define EV_UPDATE BIT0
#define EV_ACK BIT1
#define EV_NAK BIT2
#define EV_POLL BIT3
#define EV_STOPPED BIT4

#define UART_RX_BUF 4096
#define ACK_TIMEOUT_MS 1000

struct neo6m_dev {
    int uart;
    uint32_t baud;
    uint16_t idle_ms;
    TaskHandle_t task;
    volatile bool stop;
    SemaphoreHandle_t lock;      // protege pub, cb
    SemaphoreHandle_t cmd_lock;  // un comando UBX a la vez
    EventGroupHandle_t ev;
    neo6m_data_t cur;            // época en construcción (solo la toca la tarea)
    neo6m_data_t pub;            // última época publicada
    neo6m_callback_t cb;
    void *cb_arg;
    bool nmea_since_pub;

    // Intérprete de bytes
    char line[128];
    size_t line_len;
    bool in_line;
    uint8_t ubx_state, ubx_cls, ubx_id, ck_a, ck_b;
    uint16_t ubx_len, ubx_pos;
    uint8_t ubx_buf[256];

    // Esperas de comandos UBX
    volatile bool wait_ack, wait_poll;
    uint8_t wait_cls, wait_id;
    uint8_t *poll_buf;
    size_t poll_max, poll_len;
};

/* ------------------------------------------------------------------------ */
/* Intérprete del flujo de bytes: mezcla texto NMEA y tramas binarias UBX   */
/* ------------------------------------------------------------------------ */

static void handle_line(struct neo6m_dev *d)
{
    esp_err_t r = neo6m_nmea_parse(&d->cur, d->line);
    if (r == ESP_ERR_INVALID_CRC) {
        d->cur.nmea_bad++;
        return;
    }
    if (r == ESP_OK || r == ESP_ERR_NOT_SUPPORTED) {
        d->cur.nmea_ok++;
        d->nmea_since_pub = true;
    }
}

static void handle_ubx(struct neo6m_dev *d)
{
    // ACK-ACK (05 01) / ACK-NAK (05 00): el payload dice a qué comando responde.
    if (d->wait_ack && d->ubx_cls == 0x05 && d->ubx_len == 2 && d->ubx_buf[0] == d->wait_cls &&
        d->ubx_buf[1] == d->wait_id) {
        xEventGroupSetBits(d->ev, d->ubx_id == 0x01 ? EV_ACK : EV_NAK);
        return;
    }
    if (d->wait_poll && d->ubx_cls == d->wait_cls && d->ubx_id == d->wait_id) {
        d->poll_len = d->ubx_len;
        memcpy(d->poll_buf, d->ubx_buf, d->ubx_len < d->poll_max ? d->ubx_len : d->poll_max);
        xEventGroupSetBits(d->ev, EV_POLL);
    }
}

static void ck_add(struct neo6m_dev *d, uint8_t c)
{
    d->ck_a += c;
    d->ck_b += d->ck_a;
}

static void feed(struct neo6m_dev *d, uint8_t c)
{
    switch (d->ubx_state) {
    case 0:
        if (c == 0xB5) {
            d->ubx_state = 1;
            return;
        }
        break;  // no es UBX: a NMEA
    case 1:
        if (c == 0x62) {
            d->ubx_state = 2;
            d->ck_a = d->ck_b = 0;
            return;
        }
        d->ubx_state = 0;
        break;
    case 2: d->ubx_cls = c; ck_add(d, c); d->ubx_state = 3; return;
    case 3: d->ubx_id = c; ck_add(d, c); d->ubx_state = 4; return;
    case 4: d->ubx_len = c; ck_add(d, c); d->ubx_state = 5; return;
    case 5:
        d->ubx_len |= (uint16_t)c << 8;
        ck_add(d, c);
        d->ubx_pos = 0;
        if (d->ubx_len > sizeof(d->ubx_buf)) d->ubx_state = 0;  // demasiado grande: descartar
        else d->ubx_state = d->ubx_len ? 6 : 7;
        return;
    case 6:
        d->ubx_buf[d->ubx_pos++] = c;
        ck_add(d, c);
        if (d->ubx_pos == d->ubx_len) d->ubx_state = 7;
        return;
    case 7: d->ubx_state = (c == d->ck_a) ? 8 : 0; return;
    case 8:
        d->ubx_state = 0;
        if (c == d->ck_b) handle_ubx(d);
        return;
    }

    if (c == '$') {
        d->in_line = true;
        d->line_len = 0;
    }
    if (!d->in_line || c == '\r') return;
    if (c == '\n') {
        d->line[d->line_len] = '\0';
        d->in_line = false;
        handle_line(d);
        return;
    }
    if (d->line_len < sizeof(d->line) - 1) d->line[d->line_len++] = (char)c;
    else d->in_line = false;  // frase demasiado larga: basura
}

static void publish(struct neo6m_dev *d)
{
    neo6m_data_t copia;
    neo6m_callback_t cb;
    void *arg;
    d->cur.update_us = esp_timer_get_time();
    d->cur.epochs++;
    xSemaphoreTake(d->lock, portMAX_DELAY);
    d->pub = d->cur;
    copia = d->cur;
    cb = d->cb;
    arg = d->cb_arg;
    xSemaphoreGive(d->lock);
    // La lista de satélites se rehace en cada época: si GSV se desactiva o no
    // llega, mejor 0 que mostrar satélites de hace minutos.
    d->cur.n_sats = 0;
    d->cur.sats_view = 0;
    xEventGroupSetBits(d->ev, EV_UPDATE);
    if (cb) cb(&copia, arg);
}

static void reader_task(void *arg)
{
    struct neo6m_dev *d = arg;
    uint8_t buf[256];
    while (!d->stop) {
        int n = uart_read_bytes(d->uart, buf, sizeof(buf), pdMS_TO_TICKS(d->idle_ms));
        for (int i = 0; i < n; i++) feed(d, buf[i]);
        // Un hueco de silencio tras frases NMEA = fin de la época.
        if (n <= 0 && d->nmea_since_pub) {
            d->nmea_since_pub = false;
            publish(d);
        }
    }
    xEventGroupSetBits(d->ev, EV_STOPPED);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------------ */
/* Recepción síncrona (antes de arrancar la tarea): autodetección de baudios */
/* ------------------------------------------------------------------------ */

static bool listen_nmea(struct neo6m_dev *d, uint32_t ms, uint32_t need)
{
    uint8_t buf[128];
    uint32_t start = d->cur.nmea_ok;
    int64_t fin = esp_timer_get_time() + (int64_t)ms * 1000;
    while (esp_timer_get_time() < fin) {
        int n = uart_read_bytes(d->uart, buf, sizeof(buf), pdMS_TO_TICKS(20));
        for (int i = 0; i < n; i++) feed(d, buf[i]);
        if (d->cur.nmea_ok - start >= need) return true;
    }
    return false;
}

/* Con la tarea en marcha: espera a que el contador de frases buenas suba. */
static bool wait_nmea(struct neo6m_dev *d, uint32_t ms, uint32_t need)
{
    uint32_t start = d->cur.nmea_ok;
    for (uint32_t t = 0; t < ms; t += 20) {
        vTaskDelay(pdMS_TO_TICKS(20));
        if (d->cur.nmea_ok - start >= need) return true;
    }
    return false;
}

/* ------------------------------------------------------------------------ */
/* Ciclo de vida                                                            */
/* ------------------------------------------------------------------------ */

esp_err_t neo6m_init(const neo6m_config_t *cfg, neo6m_handle_t *out)
{
    ESP_RETURN_ON_FALSE(cfg && out, ESP_ERR_INVALID_ARG, TAG, "argumento nulo");
    esp_err_t ret = ESP_OK;
    struct neo6m_dev *d = calloc(1, sizeof(*d));
    ESP_RETURN_ON_FALSE(d, ESP_ERR_NO_MEM, TAG, "sin memoria");
    d->uart = -1;
    d->idle_ms = cfg->idle_ms ? cfg->idle_ms : 30;
    d->lock = xSemaphoreCreateMutex();
    d->cmd_lock = xSemaphoreCreateMutex();
    d->ev = xEventGroupCreate();
    ESP_GOTO_ON_FALSE(d->lock && d->cmd_lock && d->ev, ESP_ERR_NO_MEM, fail, TAG, "sin memoria");

    uart_config_t uc = {
        .baud_rate = cfg->baud ? (int)cfg->baud : 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_GOTO_ON_ERROR(uart_driver_install(cfg->uart_num, UART_RX_BUF, 0, 0, NULL, 0), fail, TAG,
                      "instalar UART%d", cfg->uart_num);
    d->uart = cfg->uart_num;
    ESP_GOTO_ON_ERROR(uart_param_config(d->uart, &uc), fail, TAG, "configurar UART");
    ESP_GOTO_ON_ERROR(uart_set_pin(d->uart, cfg->tx_gpio, cfg->rx_gpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                      fail, TAG, "asignar pines");
    // Pull-up en RX: la línea UART en reposo está en alto. Sin él, un TX del GPS
    // suelto (o sin GND común) deja el pin flotando, capta ruido y dispara
    // ráfagas de interrupciones del UART que frenan al WiFi y al resto del núcleo.
    gpio_pullup_en(cfg->rx_gpio);

    // Baudios: el indicado o, si es 0, probar los habituales del NEO-6M.
    static const uint32_t bauds[] = {9600, 38400, 115200, 57600, 19200, 4800, 230400};
    bool ok = false;
    for (size_t i = 0; i < sizeof(bauds) / sizeof(bauds[0]) && !ok; i++) {
        uint32_t b = cfg->baud ? cfg->baud : bauds[i];
        uart_set_baudrate(d->uart, b);
        uart_flush_input(d->uart);
        d->in_line = false;
        d->ubx_state = 0;
        // 2 frases buenas seguidas: al azar con baudios erróneos es prácticamente imposible.
        if (listen_nmea(d, 1500, 2)) {
            d->baud = b;
            ok = true;
        }
        if (cfg->baud) break;
    }
    ESP_GOTO_ON_FALSE(ok, ESP_ERR_NOT_FOUND, fail, TAG,
                      "no llega NMEA: revisa TX del GPS -> GPIO%d, GND común y alimentación", cfg->rx_gpio);
    ESP_LOGI(TAG, "GPS a %lu baud en UART%d (TX=%d, RX=%d)", (unsigned long)d->baud, d->uart, cfg->tx_gpio,
             cfg->rx_gpio);

    // Época nueva limpia (sin las frases de la autodetección a medias).
    memset(&d->cur, 0, sizeof(d->cur));
    d->nmea_since_pub = false;
    ESP_GOTO_ON_FALSE(xTaskCreatePinnedToCore(reader_task, "neo6m", 6144, d, cfg->task_prio, &d->task,
                                              cfg->task_core) == pdPASS,
                      ESP_ERR_NO_MEM, fail, TAG, "crear tarea");

    if (cfg->baud_target && cfg->baud_target != d->baud)
        ESP_GOTO_ON_ERROR(neo6m_set_baud(d, cfg->baud_target), fail, TAG, "cambiar baudios");
    if (cfg->rate_ms) ESP_GOTO_ON_ERROR(neo6m_set_rate(d, cfg->rate_ms), fail, TAG, "cambiar frecuencia");

    *out = d;
    return ESP_OK;

fail:
    neo6m_deinit(d);
    return ret;
}

esp_err_t neo6m_deinit(neo6m_handle_t d)
{
    if (!d) return ESP_ERR_INVALID_ARG;
    if (d->task) {
        d->stop = true;
        xEventGroupWaitBits(d->ev, EV_STOPPED, pdFALSE, pdTRUE, pdMS_TO_TICKS(1000));
    }
    if (d->uart >= 0) uart_driver_delete(d->uart);
    if (d->ev) vEventGroupDelete(d->ev);
    if (d->lock) vSemaphoreDelete(d->lock);
    if (d->cmd_lock) vSemaphoreDelete(d->cmd_lock);
    free(d);
    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Datos                                                                    */
/* ------------------------------------------------------------------------ */

void neo6m_get(neo6m_handle_t d, neo6m_data_t *out)
{
    xSemaphoreTake(d->lock, portMAX_DELAY);
    *out = d->pub;
    xSemaphoreGive(d->lock);
}

bool neo6m_wait_update(neo6m_handle_t d, neo6m_data_t *out, TickType_t timeout)
{
    EventBits_t b = xEventGroupWaitBits(d->ev, EV_UPDATE, pdTRUE, pdTRUE, timeout);
    if (!(b & EV_UPDATE)) return false;
    if (out) neo6m_get(d, out);
    return true;
}

void neo6m_set_callback(neo6m_handle_t d, neo6m_callback_t cb, void *arg)
{
    xSemaphoreTake(d->lock, portMAX_DELAY);
    d->cb = cb;
    d->cb_arg = arg;
    xSemaphoreGive(d->lock);
}

uint32_t neo6m_get_baud(neo6m_handle_t d)
{
    return d->baud;
}

/* ------------------------------------------------------------------------ */
/* UBX                                                                      */
/* ------------------------------------------------------------------------ */

static esp_err_t write_frame(struct neo6m_dev *d, uint8_t cls, uint8_t id, const void *payload, uint16_t len)
{
    uint8_t frame[8 + 64];
    size_t n = neo6m_ubx_frame(cls, id, payload, len, frame, sizeof(frame));
    ESP_RETURN_ON_FALSE(n, ESP_ERR_INVALID_SIZE, TAG, "payload UBX demasiado grande");
    ESP_RETURN_ON_FALSE(uart_write_bytes(d->uart, frame, n) == (int)n, ESP_FAIL, TAG, "escribir UART");
    return uart_wait_tx_done(d->uart, pdMS_TO_TICKS(500));
}

esp_err_t neo6m_ubx_send(neo6m_handle_t d, uint8_t cls, uint8_t id, const void *payload, uint16_t len,
                         bool wait_ack)
{
    ESP_RETURN_ON_FALSE(d, ESP_ERR_INVALID_ARG, TAG, "handle nulo");
    xSemaphoreTake(d->cmd_lock, portMAX_DELAY);
    d->wait_cls = cls;
    d->wait_id = id;
    xEventGroupClearBits(d->ev, EV_ACK | EV_NAK);
    d->wait_ack = wait_ack;
    esp_err_t ret = write_frame(d, cls, id, payload, len);
    if (ret == ESP_OK && wait_ack) {
        EventBits_t b =
            xEventGroupWaitBits(d->ev, EV_ACK | EV_NAK, pdTRUE, pdFALSE, pdMS_TO_TICKS(ACK_TIMEOUT_MS));
        ret = (b & EV_ACK) ? ESP_OK : (b & EV_NAK) ? ESP_FAIL : ESP_ERR_TIMEOUT;
        if (ret == ESP_ERR_TIMEOUT)
            ESP_LOGW(TAG, "sin ACK para UBX %02X-%02X: ¿RX del GPS conectado al TX del ESP?", cls, id);
    }
    d->wait_ack = false;
    xSemaphoreGive(d->cmd_lock);
    return ret;
}

esp_err_t neo6m_ubx_poll(neo6m_handle_t d, uint8_t cls, uint8_t id, uint8_t *resp, size_t max,
                         size_t *resp_len, TickType_t timeout)
{
    ESP_RETURN_ON_FALSE(d && resp, ESP_ERR_INVALID_ARG, TAG, "argumento nulo");
    xSemaphoreTake(d->cmd_lock, portMAX_DELAY);
    d->wait_cls = cls;
    d->wait_id = id;
    d->poll_buf = resp;
    d->poll_max = max;
    d->poll_len = 0;
    xEventGroupClearBits(d->ev, EV_POLL);
    d->wait_poll = true;
    esp_err_t ret = write_frame(d, cls, id, NULL, 0);
    if (ret == ESP_OK) {
        EventBits_t b = xEventGroupWaitBits(d->ev, EV_POLL, pdTRUE, pdTRUE, timeout);
        ret = (b & EV_POLL) ? ESP_OK : ESP_ERR_TIMEOUT;
    }
    d->wait_poll = false;
    if (resp_len) *resp_len = d->poll_len;
    xSemaphoreGive(d->cmd_lock);
    return ret;
}

esp_err_t neo6m_set_baud(neo6m_handle_t d, uint32_t baud)
{
    ESP_RETURN_ON_FALSE(d && baud >= 4800 && baud <= 230400, ESP_ERR_INVALID_ARG, TAG, "baudios inválidos");
    // CFG-PRT (06 00) para el UART1 del GPS: 8N1, entrada/salida UBX+NMEA.
    uint8_t p[20] = {0};
    p[0] = 1;                                  // portID = UART1
    p[4] = 0xD0; p[5] = 0x08;                  // mode = 0x000008D0 (8 bits, sin paridad, 1 stop)
    memcpy(&p[8], &baud, 4);                   // little-endian, igual que el ESP32
    p[12] = 0x03;                              // inProtoMask  = UBX + NMEA
    p[14] = 0x03;                              // outProtoMask = UBX + NMEA
    // El ACK de CFG-PRT a veces sale ya a la velocidad nueva y se pierde:
    // no se espera; se confirma escuchando NMEA válido con los baudios nuevos.
    uint32_t antes = d->baud;
    ESP_RETURN_ON_ERROR(neo6m_ubx_send(d, 0x06, 0x00, p, sizeof(p), false), TAG, "CFG-PRT");
    vTaskDelay(pdMS_TO_TICKS(100));
    uart_set_baudrate(d->uart, baud);
    if (wait_nmea(d, 2500, 2)) {
        d->baud = baud;
        ESP_LOGI(TAG, "baudios: %lu -> %lu", (unsigned long)antes, (unsigned long)baud);
        return ESP_OK;
    }
    uart_set_baudrate(d->uart, antes);
    ESP_LOGW(TAG, "el GPS no respondió a %lu baud (¿RX del GPS sin conectar?)", (unsigned long)baud);
    return ESP_ERR_TIMEOUT;
}

esp_err_t neo6m_set_rate(neo6m_handle_t d, uint16_t period_ms)
{
    ESP_RETURN_ON_FALSE(period_ms >= 200 && period_ms <= 10000, ESP_ERR_INVALID_ARG, TAG,
                        "periodo 200..10000 ms (el NEO-6M llega a 5 Hz)");
    if (d->baud <= 9600 && period_ms < 500)
        ESP_LOGW(TAG, "a %lu baud no caben todas las frases a %u ms: sube baudios o quita GSV/GLL/VTG",
                 (unsigned long)d->baud, period_ms);
    // CFG-RATE (06 08): measRate, navRate = 1, timeRef = 1 (hora GPS).
    uint8_t p[6] = {(uint8_t)(period_ms & 0xFF), (uint8_t)(period_ms >> 8), 1, 0, 1, 0};
    return neo6m_ubx_send(d, 0x06, 0x08, p, sizeof(p), true);
}

esp_err_t neo6m_set_nmea(neo6m_handle_t d, neo6m_nmea_t msg, bool enable)
{
    // CFG-MSG (06 01) versión corta: clase F0 (NMEA), id, frecuencia en el puerto actual.
    uint8_t p[3] = {0xF0, (uint8_t)msg, enable ? 1 : 0};
    return neo6m_ubx_send(d, 0x06, 0x01, p, sizeof(p), true);
}

esp_err_t neo6m_save_config(neo6m_handle_t d)
{
    // CFG-CFG (06 09): guardar todo en BBR + flash + EEPROM (deviceMask 0x17).
    uint8_t p[13] = {0, 0, 0, 0, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0x17};
    return neo6m_ubx_send(d, 0x06, 0x09, p, sizeof(p), true);
}

esp_err_t neo6m_restart(neo6m_handle_t d, neo6m_start_t mode)
{
    // CFG-RST (06 04): resetMode 2 = reinicio controlado del GNSS. No responde ACK.
    uint8_t p[4] = {(uint8_t)(mode & 0xFF), (uint8_t)(mode >> 8), 0x02, 0};
    return neo6m_ubx_send(d, 0x06, 0x04, p, sizeof(p), false);
}

esp_err_t neo6m_get_version(neo6m_handle_t d, char *sw, size_t sw_len, char *hw, size_t hw_len)
{
    // MON-VER (0A 04): 30 B versión software + 10 B versión hardware + extensiones.
    uint8_t r[256];
    size_t n = 0;
    ESP_RETURN_ON_ERROR(neo6m_ubx_poll(d, 0x0A, 0x04, r, sizeof(r), &n, pdMS_TO_TICKS(ACK_TIMEOUT_MS)), TAG,
                        "MON-VER");
    ESP_RETURN_ON_FALSE(n >= 40, ESP_ERR_INVALID_RESPONSE, TAG, "MON-VER corto");
    if (sw && sw_len) {
        strncpy(sw, (const char *)r, sw_len - 1);
        sw[sw_len - 1] = '\0';
    }
    if (hw && hw_len) {
        strncpy(hw, (const char *)r + 30, hw_len - 1);
        hw[hw_len - 1] = '\0';
    }
    return ESP_OK;
}
