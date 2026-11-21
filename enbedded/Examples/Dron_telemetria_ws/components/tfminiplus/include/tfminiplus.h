/*
 * tfminiplus — driver ESP-IDF 6 para el LiDAR Benewake TF Mini Plus por UART.
 *
 * Alimentación 5 V (4.5–5.5 V, ~110 mA), señales TX/RX de 3.3 V: va directo
 * a los GPIO del ESP32 sin convertidor de nivel.
 *
 *   Rojo   +5 V            -> 5V / VBUS de la placa
 *   Negro  GND             -> GND
 *   Verde  TX del sensor   -> rx_gpio del ESP   (por defecto GPIO4)
 *   Blanco RX del sensor   -> tx_gpio del ESP   (por defecto GPIO5)
 *
 * El sensor manda sin parar (por defecto 100 veces/s, 115200 baud) tramas de
 * 9 bytes:
 *   59 59 | dist L H | fuerza L H | temp L H | checksum
 * y acepta comandos con el formato  5A | longitud | ID | datos... | checksum
 * (checksum = 8 bits bajos de la suma de los bytes anteriores).
 *
 * Una tarea interna lee el UART, valida cada trama y publica la última
 * medida:
 *   - tfminiplus_get()          última medida (no bloquea)
 *   - tfminiplus_wait_update()  espera a la siguiente
 *   - tfminiplus_set_callback() función llamada en cada medida
 *
 * Los cambios de configuración (frecuencia, unidad...) viven en la RAM del
 * sensor: se pierden al quitarle la corriente salvo tfminiplus_save_settings().
 * Al reiniciar solo el ESP, el sensor los conserva.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int uart_num;            // 1 o 2 (el UART0 es la consola de arranque)
    int tx_gpio;             // ESP TX -> RX del sensor (cable blanco)
    int rx_gpio;             // ESP RX <- TX del sensor (cable verde)
    uint32_t baud;           // baudios del sensor; 0 = autodetectar (de fábrica: 115200)
    bool auto_swap;          // si no responde, probar también con TX/RX intercambiados
    uint16_t frame_rate_hz;  // 0 = no tocar; 1..1000 (mejor divisores de 1000)
    bool unit_mm;            // false = el sensor manda cm, true = mm
    uint16_t min_strength;   // fuerza mínima para dar una medida por buena (datasheet: 100)
    int task_core;           // núcleo de la tarea lectora (tskNO_AFFINITY = cualquiera)
    int task_prio;
} tfminiplus_config_t;

// Cableado propuesto en la ESP32-S3 del proyecto: TX=GPIO5, RX=GPIO4 en el
// UART2 (el UART1 lo usa la librería neo6m en GPIO17/18, así conviven).
#define TFMINIPLUS_CONFIG_DEFAULT()                                                      \
    {                                                                                    \
        .uart_num = 2, .tx_gpio = 5, .rx_gpio = 4, .baud = 0, .auto_swap = true,         \
        .frame_rate_hz = 0, .unit_mm = false, .min_strength = 100, .task_core = 0,       \
        .task_prio = 6,                                                                  \
    }

typedef struct {
    bool valid;            // fuerza suficiente y distancia dentro de rango
    uint16_t dist_cm;      // distancia (0 si no es válida)
    uint16_t dist_mm;      // la misma en mm (resolución real 1 cm en modo cm)
    uint16_t strength;     // fuerza de la señal: <100 poco fiable, 65535 = saturado
    float temp_c;          // temperatura interna del sensor (°C)
    uint16_t raw_dist;     // valor tal cual llegó (en la unidad configurada)
    int64_t update_us;     // esp_timer_get_time() al recibir la trama
    float rate_hz;         // tramas/s medidas en el último segundo
    uint32_t frames;       // tramas de medida válidas (checksum OK)
    uint32_t bad_frames;   // tramas con checksum incorrecto
    uint32_t junk_bytes;   // bytes descartados buscando la cabecera
} tfminiplus_data_t;

typedef struct tfminiplus_dev *tfminiplus_handle_t;
typedef void (*tfminiplus_callback_t)(const tfminiplus_data_t *data, void *arg);

/* --- Ciclo de vida -------------------------------------------------------- */
/* Instala el UART, busca los baudios del sensor (si baud = 0) y, si auto_swap,
 * también prueba con TX/RX intercambiados. Fija la unidad, activa la salida y
 * aplica frame_rate_hz si se pidió.
 * Errores: ESP_ERR_NOT_FOUND = el sensor no responde en ningún baudio/pines
 *          (revisa 5 V, GND y los cables verde/blanco). */
esp_err_t tfminiplus_init(const tfminiplus_config_t *cfg, tfminiplus_handle_t *out);
esp_err_t tfminiplus_deinit(tfminiplus_handle_t dev);

/* --- Datos ---------------------------------------------------------------- */
void tfminiplus_get(tfminiplus_handle_t dev, tfminiplus_data_t *out);
/* Espera la siguiente medida. false = timeout. Pensado para UNA tarea lectora. */
bool tfminiplus_wait_update(tfminiplus_handle_t dev, tfminiplus_data_t *out, TickType_t timeout);
/* Callback por medida, ejecutado en la tarea del driver: que sea corto. */
void tfminiplus_set_callback(tfminiplus_handle_t dev, tfminiplus_callback_t cb, void *arg);

/* Baudios y pines con los que se encontró el sensor. */
uint32_t tfminiplus_get_baud(tfminiplus_handle_t dev);
bool tfminiplus_pins_swapped(tfminiplus_handle_t dev);

/* --- Configuración del sensor --------------------------------------------- */
/* Versión del firmware del sensor, p. ej. "2.1.4". */
esp_err_t tfminiplus_get_version(tfminiplus_handle_t dev, char *out, size_t out_len);
/* 1..1000 Hz. 0 = modo disparo: el sensor solo mide al llamar tfminiplus_trigger(). */
esp_err_t tfminiplus_set_frame_rate(tfminiplus_handle_t dev, uint16_t hz);
/* En modo disparo (frame rate 0) pide una medida y la espera. */
esp_err_t tfminiplus_trigger(tfminiplus_handle_t dev, tfminiplus_data_t *out, TickType_t timeout);
esp_err_t tfminiplus_set_unit_mm(tfminiplus_handle_t dev, bool mm);
esp_err_t tfminiplus_enable_output(tfminiplus_handle_t dev, bool enable);
esp_err_t tfminiplus_save_settings(tfminiplus_handle_t dev);    // guarda en la flash del sensor
esp_err_t tfminiplus_factory_reset(tfminiplus_handle_t dev);    // valores de fábrica (luego save)
esp_err_t tfminiplus_system_reset(tfminiplus_handle_t dev);     // reinicia el sensor

/* Comando genérico: manda 5A len id payload cksum y copia la respuesta cuyo ID
 * coincida (trama completa, cabecera incluida). */
esp_err_t tfminiplus_command(tfminiplus_handle_t dev, uint8_t id, const void *payload, uint8_t len,
                             uint8_t *resp, size_t resp_max, size_t *resp_len, TickType_t timeout);

/* --- Funciones puras (sin hardware, útiles para pruebas) ------------------ */
#define TFMINIPLUS_FRAME_LEN 9
#define TFMINIPLUS_MAX_CMD 16

/* 8 bits bajos de la suma de los bytes. */
uint8_t tfminiplus_checksum(const uint8_t *buf, size_t len);
/* Arma un comando. Devuelve su longitud o 0 si no cabe en out. */
size_t tfminiplus_cmd_frame(uint8_t id, const void *payload, uint8_t len, uint8_t *out, size_t out_size);
/* Decodifica una trama de medida de 9 bytes (cabecera 59 59 y checksum ya
 * comprobados o no: devuelve ESP_ERR_INVALID_CRC / ESP_ERR_INVALID_ARG).
 * Solo rellena valid, dist_cm, dist_mm, strength, temp_c y raw_dist. */
esp_err_t tfminiplus_decode(const uint8_t frame[TFMINIPLUS_FRAME_LEN], bool unit_mm, uint16_t min_strength,
                            tfminiplus_data_t *out);

/* Parser de flujo: separa tramas de medida (59 59 ...) y respuestas (5A ...)
 * de un flujo de bytes cualquiera, resincronizando tras basura o checksum
 * malo. Llama cb(frame, len, arg) por cada trama completa y correcta. */
typedef struct {
    uint8_t buf[2 * TFMINIPLUS_MAX_CMD];
    size_t n;
    uint32_t bad;   // tramas con checksum incorrecto
    uint32_t junk;  // bytes descartados
} tfminiplus_parser_t;

typedef void (*tfminiplus_frame_cb_t)(const uint8_t *frame, size_t len, void *arg);
void tfminiplus_parser_reset(tfminiplus_parser_t *p);
void tfminiplus_parser_feed(tfminiplus_parser_t *p, const uint8_t *data, size_t len, tfminiplus_frame_cb_t cb,
                            void *arg);

#ifdef __cplusplus
}
#endif
