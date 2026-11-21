#include "tfminiplus.h"

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

static const char *TAG = "tfminiplus";

#define UART_RX_BUF 1024  // debe ser > SOC_UART_FIFO_LEN (128)

#define BIT_DATA BIT0
#define BIT_RESP BIT1
#define BIT_STOPPED BIT2

// IDs de comando (datasheet TFmini Plus, "Custom parameter configuration")
#define ID_VERSION 0x01
#define ID_RESET 0x02
#define ID_RATE 0x03
#define ID_TRIGGER 0x04
#define ID_FORMAT 0x05
#define ID_OUTPUT 0x07
#define ID_FACTORY 0x10
#define ID_SAVE 0x11

#define FORMAT_CM 0x01
#define FORMAT_MM 0x06

#define CMD_TIMEOUT pdMS_TO_TICKS(200)

struct tfminiplus_dev {
    int uart;
    int tx_gpio, rx_gpio;  // tal como vienen en la config
    bool swapped;          // true = se usan intercambiados
    uint32_t baud;
    volatile bool unit_mm;
    uint16_t min_strength;

    TaskHandle_t task;
    volatile bool run;
    volatile bool reset_parser;
    bool loopback;               // en la búsqueda volvió nuestro propio comando
    uint16_t rate_set;           // última frecuencia pedida (0xFFFF = desconocida)
    SemaphoreHandle_t lock;     // protege data, resp, want_id, cb
    SemaphoreHandle_t cmd_lock;  // un comando a la vez
    EventGroupHandle_t ev;

    tfminiplus_data_t data;
    tfminiplus_callback_t cb;
    void *cb_arg;

    uint8_t want_id;  // ID de la respuesta que se espera (0 = ninguna)
    uint8_t resp[TFMINIPLUS_MAX_CMD];
    size_t resp_len;

    tfminiplus_parser_t parser;  // solo lo toca la tarea lectora
    int64_t rate_t0;
    uint32_t rate_n0;
};

/* --- Tarea lectora --------------------------------------------------------- */

static void on_frame(const uint8_t *f, size_t len, void *arg)
{
    struct tfminiplus_dev *d = arg;

    if (f[0] == 0x59) {  // medida
        tfminiplus_data_t m = {0};
        tfminiplus_decode(f, d->unit_mm, d->min_strength, &m);  // el checksum ya lo comprobó el parser
        int64_t now = esp_timer_get_time();

        xSemaphoreTake(d->lock, portMAX_DELAY);
        d->data.valid = m.valid;
        d->data.dist_cm = m.dist_cm;
        d->data.dist_mm = m.dist_mm;
        d->data.strength = m.strength;
        d->data.temp_c = m.temp_c;
        d->data.raw_dist = m.raw_dist;
        d->data.update_us = now;
        d->data.frames++;
        tfminiplus_data_t snap = d->data;
        tfminiplus_callback_t cb = d->cb;
        void *cb_arg = d->cb_arg;
        xSemaphoreGive(d->lock);

        xEventGroupSetBits(d->ev, BIT_DATA);
        if (cb) cb(&snap, cb_arg);
        return;
    }

    // Respuesta a un comando (5A len id ...)
    bool match = false;
    xSemaphoreTake(d->lock, portMAX_DELAY);
    if (d->want_id != 0 && len >= 3 && f[2] == d->want_id) {
        memcpy(d->resp, f, len);
        d->resp_len = len;
        match = true;
    }
    xSemaphoreGive(d->lock);
    if (match) xEventGroupSetBits(d->ev, BIT_RESP);
}

static void reader_task(void *arg)
{
    struct tfminiplus_dev *d = arg;
    uint8_t buf[256];

    while (d->run) {
        if (d->reset_parser) {  // tras cambiar baudios/pines: tirar lo que hubiera a medias
            // El vaciado lo hace ESTA tarea: uart_flush_input() usa el mismo
            // mutex de RX que uart_read_bytes(), y si lo llamara otra tarea de
            // menor prioridad no lo conseguiría nunca (esta lo retoma enseguida).
            uart_flush_input(d->uart);
            d->parser.n = 0;
            d->reset_parser = false;
        }

        // Esperar el primer byte y luego llevarse todo lo que ya esté en el
        // búfer: así la latencia es de ~1 byte y no de un timeout fijo.
        int n = uart_read_bytes(d->uart, buf, 1, pdMS_TO_TICKS(20));
        if (n == 1) {
            size_t avail = 0;
            uart_get_buffered_data_len(d->uart, &avail);
            if (avail > sizeof(buf) - 1) avail = sizeof(buf) - 1;
            if (avail) {
                int m = uart_read_bytes(d->uart, buf + 1, avail, 0);
                if (m > 0) n += m;
            }
            tfminiplus_parser_feed(&d->parser, buf, (size_t)n, on_frame, d);
        }

        int64_t now = esp_timer_get_time();
        xSemaphoreTake(d->lock, portMAX_DELAY);
        d->data.bad_frames = d->parser.bad;
        d->data.junk_bytes = d->parser.junk;
        if (now - d->rate_t0 >= 1000000) {  // tramas/s en ventanas de 1 s
            d->data.rate_hz = (float)(d->data.frames - d->rate_n0) * 1e6f / (float)(now - d->rate_t0);
            d->rate_t0 = now;
            d->rate_n0 = d->data.frames;
        }
        xSemaphoreGive(d->lock);
    }

    xEventGroupSetBits(d->ev, BIT_STOPPED);
    vTaskDelete(NULL);
}

/* --- Comandos -------------------------------------------------------------- */

esp_err_t tfminiplus_command(tfminiplus_handle_t d, uint8_t id, const void *payload, uint8_t len, uint8_t *resp,
                             size_t resp_max, size_t *resp_len, TickType_t timeout)
{
    ESP_RETURN_ON_FALSE(d, ESP_ERR_INVALID_ARG, TAG, "handle nulo");
    uint8_t frame[TFMINIPLUS_MAX_CMD];
    size_t n = tfminiplus_cmd_frame(id, payload, len, frame, sizeof(frame));
    ESP_RETURN_ON_FALSE(n > 0, ESP_ERR_INVALID_ARG, TAG, "comando demasiado largo");

    xSemaphoreTake(d->cmd_lock, portMAX_DELAY);
    xSemaphoreTake(d->lock, portMAX_DELAY);
    d->want_id = id;
    d->resp_len = 0;
    xSemaphoreGive(d->lock);
    xEventGroupClearBits(d->ev, BIT_RESP);

    esp_err_t err = ESP_OK;
    if (uart_write_bytes(d->uart, frame, n) != (int)n) {
        err = ESP_FAIL;
    } else {
        EventBits_t b = xEventGroupWaitBits(d->ev, BIT_RESP, pdTRUE, pdFALSE, timeout);
        if (!(b & BIT_RESP)) err = ESP_ERR_TIMEOUT;
    }

    xSemaphoreTake(d->lock, portMAX_DELAY);
    if (err == ESP_OK && resp) {
        size_t c = d->resp_len < resp_max ? d->resp_len : resp_max;
        memcpy(resp, d->resp, c);
        if (resp_len) *resp_len = c;
    }
    d->want_id = 0;
    xSemaphoreGive(d->lock);
    xSemaphoreGive(d->cmd_lock);
    return err;
}

/* Comandos cuya respuesta repite los mismos datos (frecuencia, formato, salida). */
static esp_err_t cmd_echo(tfminiplus_handle_t d, uint8_t id, const void *payload, uint8_t len)
{
    uint8_t r[TFMINIPLUS_MAX_CMD];
    size_t rl = 0;
    ESP_RETURN_ON_ERROR(tfminiplus_command(d, id, payload, len, r, sizeof(r), &rl, CMD_TIMEOUT), TAG,
                        "sin respuesta al comando 0x%02X", id);
    if (rl != 4u + len || memcmp(&r[3], payload, len) != 0) {
        ESP_LOGW(TAG, "respuesta inesperada al comando 0x%02X (len %u)", id, (unsigned)rl);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

/* Comandos que responden 5A 05 id estado cksum (0 = éxito). */
static esp_err_t cmd_status(tfminiplus_handle_t d, uint8_t id)
{
    uint8_t r[TFMINIPLUS_MAX_CMD];
    size_t rl = 0;
    ESP_RETURN_ON_ERROR(tfminiplus_command(d, id, NULL, 0, r, sizeof(r), &rl, pdMS_TO_TICKS(1000)), TAG,
                        "sin respuesta al comando 0x%02X", id);
    ESP_RETURN_ON_FALSE(rl == 5, ESP_ERR_INVALID_RESPONSE, TAG, "respuesta rara al comando 0x%02X", id);
    ESP_RETURN_ON_FALSE(r[3] == 0, ESP_FAIL, TAG, "el sensor rechazó el comando 0x%02X (estado %u)", id, r[3]);
    return ESP_OK;
}

esp_err_t tfminiplus_get_version(tfminiplus_handle_t d, char *out, size_t out_len)
{
    uint8_t r[TFMINIPLUS_MAX_CMD];
    size_t rl = 0;
    ESP_RETURN_ON_ERROR(tfminiplus_command(d, ID_VERSION, NULL, 0, r, sizeof(r), &rl, CMD_TIMEOUT), TAG,
                        "sin respuesta a la versión");
    ESP_RETURN_ON_FALSE(rl == 7, ESP_ERR_INVALID_RESPONSE, TAG, "respuesta de versión rara");
    // 5A 07 01 V1 V2 V3 SU  ->  versión V3.V2.V1
    if (out && out_len) snprintf(out, out_len, "%u.%u.%u", r[5], r[4], r[3]);
    return ESP_OK;
}

esp_err_t tfminiplus_set_frame_rate(tfminiplus_handle_t d, uint16_t hz)
{
    ESP_RETURN_ON_FALSE(hz <= 1000, ESP_ERR_INVALID_ARG, TAG, "máximo 1000 Hz");
    uint8_t p[2] = {(uint8_t)(hz & 0xFF), (uint8_t)(hz >> 8)};
    xSemaphoreTake(d->lock, portMAX_DELAY);
    uint32_t f0 = d->data.frames;
    xSemaphoreGive(d->lock);
    ESP_RETURN_ON_ERROR(cmd_echo(d, ID_RATE, p, sizeof(p)), TAG, "frecuencia");
    uint16_t antes = d->rate_set;
    d->rate_set = hz;
    if (hz == 0 || antes != 0) return ESP_OK;

    // Firmware 2.1.5 (medido): al salir del modo disparo (0 Hz) acepta la
    // frecuencia al instante pero tarda ~1.1 s en volver a mandar tramas,
    // reciba o no otros comandos. Se espera aquí para que quien llame pueda
    // leer en cuanto la función vuelve.
    TickType_t t0 = xTaskGetTickCount();
    while (xTaskGetTickCount() - t0 < pdMS_TO_TICKS(1500)) {
        xEventGroupWaitBits(d->ev, BIT_DATA, pdTRUE, pdFALSE, pdMS_TO_TICKS(10));
        xSemaphoreTake(d->lock, portMAX_DELAY);
        bool fluye = d->data.frames != f0;
        xSemaphoreGive(d->lock);
        if (fluye) {
            ESP_LOGD(TAG, "flujo reanudado a los %lu ms", (unsigned long)pdTICKS_TO_MS(xTaskGetTickCount() - t0));
            return ESP_OK;
        }
    }
    ESP_LOGW(TAG, "frecuencia %u Hz aceptada pero no llegan tramas", hz);
    return ESP_ERR_TIMEOUT;
}

esp_err_t tfminiplus_set_unit_mm(tfminiplus_handle_t d, bool mm)
{
    uint8_t p = mm ? FORMAT_MM : FORMAT_CM;
    ESP_RETURN_ON_ERROR(cmd_echo(d, ID_FORMAT, &p, 1), TAG, "cambiar unidad");
    d->unit_mm = mm;
    return ESP_OK;
}

esp_err_t tfminiplus_enable_output(tfminiplus_handle_t d, bool enable)
{
    uint8_t p = enable ? 1 : 0;
    return cmd_echo(d, ID_OUTPUT, &p, 1);
}

esp_err_t tfminiplus_save_settings(tfminiplus_handle_t d)
{
    return cmd_status(d, ID_SAVE);
}

/* Tras un reinicio el sensor vuelve a lo guardado en su flash: se vuelve a
 * fijar la unidad para que el driver siga interpretando bien la distancia. */
static esp_err_t wait_alive_and_reapply(tfminiplus_handle_t d)
{
    char v[16];
    for (int i = 0; i < 20; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (tfminiplus_get_version(d, v, sizeof(v)) == ESP_OK) return tfminiplus_set_unit_mm(d, d->unit_mm);
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t tfminiplus_factory_reset(tfminiplus_handle_t d)
{
    ESP_RETURN_ON_ERROR(cmd_status(d, ID_FACTORY), TAG, "restaurar fábrica");
    return tfminiplus_set_unit_mm(d, d->unit_mm);
}

esp_err_t tfminiplus_system_reset(tfminiplus_handle_t d)
{
    ESP_RETURN_ON_ERROR(cmd_status(d, ID_RESET), TAG, "reiniciar sensor");
    return wait_alive_and_reapply(d);
}

esp_err_t tfminiplus_trigger(tfminiplus_handle_t d, tfminiplus_data_t *out, TickType_t timeout)
{
    ESP_RETURN_ON_FALSE(d, ESP_ERR_INVALID_ARG, TAG, "handle nulo");
    uint8_t frame[4];
    size_t n = tfminiplus_cmd_frame(ID_TRIGGER, NULL, 0, frame, sizeof(frame));

    xSemaphoreTake(d->cmd_lock, portMAX_DELAY);
    xSemaphoreTake(d->lock, portMAX_DELAY);
    uint32_t f0 = d->data.frames;
    xSemaphoreGive(d->lock);
    xEventGroupClearBits(d->ev, BIT_DATA);

    esp_err_t err = ESP_ERR_TIMEOUT;
    if (uart_write_bytes(d->uart, frame, n) == (int)n) {
        TickType_t t0 = xTaskGetTickCount();
        // El disparo no tiene respuesta 5A: contesta con una trama de medida.
        while (xTaskGetTickCount() - t0 < timeout) {
            TickType_t left = timeout - (xTaskGetTickCount() - t0);
            xEventGroupWaitBits(d->ev, BIT_DATA, pdTRUE, pdFALSE, left);
            xSemaphoreTake(d->lock, portMAX_DELAY);
            bool got = d->data.frames != f0;
            if (got && out) *out = d->data;
            xSemaphoreGive(d->lock);
            if (got) {
                err = ESP_OK;
                break;
            }
        }
    } else {
        err = ESP_FAIL;
    }
    xSemaphoreGive(d->cmd_lock);
    return err;
}

/* --- Datos ----------------------------------------------------------------- */

void tfminiplus_get(tfminiplus_handle_t d, tfminiplus_data_t *out)
{
    xSemaphoreTake(d->lock, portMAX_DELAY);
    *out = d->data;
    xSemaphoreGive(d->lock);
}

bool tfminiplus_wait_update(tfminiplus_handle_t d, tfminiplus_data_t *out, TickType_t timeout)
{
    xSemaphoreTake(d->lock, portMAX_DELAY);
    uint32_t prev = d->data.frames;
    xSemaphoreGive(d->lock);

    TickType_t t0 = xTaskGetTickCount();
    for (;;) {
        TickType_t used = xTaskGetTickCount() - t0;
        if (used >= timeout) return false;
        xEventGroupWaitBits(d->ev, BIT_DATA, pdTRUE, pdFALSE, timeout - used);
        xSemaphoreTake(d->lock, portMAX_DELAY);
        bool fresh = d->data.frames != prev;
        if (fresh) *out = d->data;
        xSemaphoreGive(d->lock);
        if (fresh) return true;
    }
}

void tfminiplus_set_callback(tfminiplus_handle_t d, tfminiplus_callback_t cb, void *arg)
{
    xSemaphoreTake(d->lock, portMAX_DELAY);
    d->cb = cb;
    d->cb_arg = arg;
    xSemaphoreGive(d->lock);
}

uint32_t tfminiplus_get_baud(tfminiplus_handle_t d)
{
    return d->baud;
}

bool tfminiplus_pins_swapped(tfminiplus_handle_t d)
{
    return d->swapped;
}

/* --- Inicio ---------------------------------------------------------------- */

static esp_err_t set_line(struct tfminiplus_dev *d, uint32_t baud, bool swapped)
{
    int tx = swapped ? d->rx_gpio : d->tx_gpio;
    int rx = swapped ? d->tx_gpio : d->rx_gpio;
    // Desconectar antes ambos pines de la matriz GPIO: si no, el pin que
    // antes era TX seguiría empujando la señal aunque ahora sea RX.
    gpio_reset_pin(d->tx_gpio);
    gpio_reset_pin(d->rx_gpio);
    ESP_RETURN_ON_ERROR(uart_set_pin(d->uart, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG, "pines");
    ESP_RETURN_ON_ERROR(uart_set_baudrate(d->uart, baud), TAG, "baudios");
    d->baud = baud;
    d->swapped = swapped;
    d->reset_parser = true;  // la tarea lectora vacía el RX (ver reader_task)
    vTaskDelay(pdMS_TO_TICKS(30));  // > 20 ms del timeout de lectura: ya lo hizo
    return ESP_OK;
}

/* ¿Hay un TF Mini Plus al otro lado con estos baudios/pines? Vale la
 * respuesta a "versión" o, si no, al menos 3 tramas de medida correctas
 * (por si la salida está activada pero el RX del sensor no llega). */
static bool probe(struct tfminiplus_dev *d)
{
    xSemaphoreTake(d->lock, portMAX_DELAY);
    uint32_t f0 = d->data.frames;
    xSemaphoreGive(d->lock);

    // tfminiplus_command no escribe en el log: así la búsqueda no llena la
    // consola de errores por cada baudio que no es.
    uint8_t r[TFMINIPLUS_MAX_CMD];
    size_t rl = 0;
    if (tfminiplus_command(d, ID_VERSION, NULL, 0, r, sizeof(r), &rl, pdMS_TO_TICKS(150)) == ESP_OK) {
        // La respuesta auténtica mide 7 bytes. Si vuelven los mismos 4 bytes
        // que mandamos (5A 04 01 5F) es un eco: TX y RX conectados entre sí
        // (corto, o a través de un sensor sin alimentación). Ojo: los demás
        // comandos responden con un eco idéntico, así que solo "versión"
        // sirve para distinguirlo.
        if (rl == 7) return true;
        if (rl == 4) d->loopback = true;
    }

    xSemaphoreTake(d->lock, portMAX_DELAY);
    uint32_t got = d->data.frames - f0;
    xSemaphoreGive(d->lock);
    return got >= 3;
}

static void destroy(struct tfminiplus_dev *d)
{
    if (d->task) {
        d->run = false;
        xEventGroupWaitBits(d->ev, BIT_STOPPED, pdFALSE, pdFALSE, pdMS_TO_TICKS(500));
    }
    if (d->uart >= 0) uart_driver_delete(d->uart);
    if (d->lock) vSemaphoreDelete(d->lock);
    if (d->cmd_lock) vSemaphoreDelete(d->cmd_lock);
    if (d->ev) vEventGroupDelete(d->ev);
    free(d);
}

esp_err_t tfminiplus_init(const tfminiplus_config_t *cfg, tfminiplus_handle_t *out)
{
    ESP_RETURN_ON_FALSE(cfg && out, ESP_ERR_INVALID_ARG, TAG, "argumento nulo");
    ESP_RETURN_ON_FALSE(cfg->uart_num > 0 && cfg->uart_num < UART_NUM_MAX, ESP_ERR_INVALID_ARG, TAG,
                        "usa el UART1 o el UART2");
    ESP_RETURN_ON_FALSE(cfg->frame_rate_hz <= 1000, ESP_ERR_INVALID_ARG, TAG, "máximo 1000 Hz");

    struct tfminiplus_dev *d = calloc(1, sizeof(*d));
    ESP_RETURN_ON_FALSE(d, ESP_ERR_NO_MEM, TAG, "sin memoria");
    d->uart = -1;
    d->tx_gpio = cfg->tx_gpio;
    d->rx_gpio = cfg->rx_gpio;
    d->unit_mm = false;  // de fábrica; se fija abajo
    d->min_strength = cfg->min_strength;
    d->rate_set = 0xFFFF;

    esp_err_t ret = ESP_OK;
    d->lock = xSemaphoreCreateMutex();
    d->cmd_lock = xSemaphoreCreateMutex();
    d->ev = xEventGroupCreate();
    ESP_GOTO_ON_FALSE(d->lock && d->cmd_lock && d->ev, ESP_ERR_NO_MEM, fail, TAG, "sin memoria");

    uart_config_t uc = {
        .baud_rate = 115200,
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

    d->run = true;
    d->rate_t0 = esp_timer_get_time();
    ESP_GOTO_ON_FALSE(xTaskCreatePinnedToCore(reader_task, "tfminiplus", 4096, d, cfg->task_prio, &d->task,
                                              cfg->task_core) == pdPASS,
                      ESP_ERR_NO_MEM, fail, TAG, "crear tarea");

    // Baudios a probar: el pedido (o el de fábrica) primero, luego los
    // demás que admite el sensor.
    static const uint32_t bauds[] = {115200, 9600, 19200, 38400, 57600, 230400, 256000, 460800, 921600};
    uint32_t list[1 + sizeof(bauds) / sizeof(bauds[0])];
    size_t nl = 0;
    list[nl++] = cfg->baud ? cfg->baud : 115200;
    if (cfg->baud == 0) {
        for (size_t i = 0; i < sizeof(bauds) / sizeof(bauds[0]); i++)
            if (bauds[i] != list[0]) list[nl++] = bauds[i];
    }

    bool found = false;
    for (int s = 0; s < (cfg->auto_swap ? 2 : 1) && !found; s++) {
        for (size_t i = 0; i < nl && !found; i++) {
            ESP_GOTO_ON_ERROR(set_line(d, list[i], s == 1), fail, TAG, "configurar línea");
            found = probe(d);
        }
    }
    if (!found) {
        ESP_LOGE(TAG, "el TF Mini Plus no responde (TX ESP=GPIO%d, RX ESP=GPIO%d%s). Revisa: 5 V en el "
                      "cable rojo, GND común, verde -> GPIO%d, blanco -> GPIO%d",
                 cfg->tx_gpio, cfg->rx_gpio, cfg->auto_swap ? ", también probados al revés" : "", cfg->rx_gpio,
                 cfg->tx_gpio);
        // Pista extra: un TX de UART en reposo está en 1. Si ambos pines se
        // quedan en 0 V incluso con el pull-up interno, el sensor está
        // conectado pero apagado (sus diodos de protección los llevan a GND).
        int bajos = 0;
        const int pines[2] = {cfg->tx_gpio, cfg->rx_gpio};
        for (int i = 0; i < 2; i++) {
            gpio_reset_pin(pines[i]);  // entrada con pull-up
            gpio_set_direction(pines[i], GPIO_MODE_INPUT);
            gpio_set_pull_mode(pines[i], GPIO_PULLUP_ONLY);
        }
        vTaskDelay(pdMS_TO_TICKS(5));
        for (int i = 0; i < 2; i++) bajos += gpio_get_level(pines[i]) == 0;
        if (d->loopback) {
            ESP_LOGE(TAG, "lo que manda el ESP vuelve tal cual por el RX: GPIO%d y GPIO%d están unidos (corto entre "
                          "verde y blanco, o el sensor sin alimentación deja pasar la señal).",
                     cfg->tx_gpio, cfg->rx_gpio);
        }
        if (bajos == 2) {
            ESP_LOGE(TAG, "GPIO%d y GPIO%d están en 0 V aun con pull-up: el sensor está cableado pero SIN "
                          "ALIMENTACIÓN. El rojo debe ir a 5V/VBUS (no a 3V3) y medir ~5 V contra el negro.",
                     cfg->tx_gpio, cfg->rx_gpio);
        }
        ret = ESP_ERR_NOT_FOUND;
        goto fail;
    }
    if (d->swapped) {
        ESP_LOGW(TAG, "funciona con TX/RX INTERCAMBIADOS: el cable verde está en GPIO%d y el blanco en GPIO%d "
                      "(al revés de la config). Sigue funcionando así.",
                 cfg->tx_gpio, cfg->rx_gpio);
    }

    char ver[16] = "?";
    tfminiplus_get_version(d, ver, sizeof(ver));  // puede fallar si solo llega la salida (sin RX del sensor)
    ESP_LOGI(TAG, "sensor encontrado: firmware %s, %lu baud, UART%d", ver, (unsigned long)d->baud, d->uart);

    ESP_GOTO_ON_ERROR(tfminiplus_set_unit_mm(d, cfg->unit_mm), fail, TAG, "fijar unidad (¿llega el blanco?)");
    ESP_GOTO_ON_ERROR(tfminiplus_enable_output(d, true), fail, TAG, "activar salida");
    if (cfg->frame_rate_hz) {
        ESP_GOTO_ON_ERROR(tfminiplus_set_frame_rate(d, cfg->frame_rate_hz), fail, TAG, "frecuencia");
    } else {
        // "No tocar", salvo que el sensor esté callado: suele ser que quedó en
        // modo disparo (0 Hz) de una ejecución anterior, porque al reiniciar
        // solo el ESP el sensor conserva la configuración.
        xSemaphoreTake(d->lock, portMAX_DELAY);
        uint32_t f0 = d->data.frames;
        xSemaphoreGive(d->lock);
        vTaskDelay(pdMS_TO_TICKS(150));
        xSemaphoreTake(d->lock, portMAX_DELAY);
        bool callado = d->data.frames == f0;
        xSemaphoreGive(d->lock);
        if (callado) {
            ESP_LOGW(TAG, "el sensor no manda medidas (¿modo disparo?): vuelvo a 100 Hz");
            d->rate_set = 0;  // para que set_frame_rate espere el ~1 s que tarda en reanudar
            ESP_GOTO_ON_ERROR(tfminiplus_set_frame_rate(d, 100), fail, TAG, "frecuencia");
        }
    }

    *out = d;
    return ESP_OK;

fail:
    destroy(d);
    return ret == ESP_OK ? ESP_FAIL : ret;
}

esp_err_t tfminiplus_deinit(tfminiplus_handle_t d)
{
    ESP_RETURN_ON_FALSE(d, ESP_ERR_INVALID_ARG, TAG, "handle nulo");
    destroy(d);
    return ESP_OK;
}
