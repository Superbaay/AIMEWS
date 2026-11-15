/*
 * neo6m — driver ESP-IDF 6 para GPS u-blox NEO-6M (módulos GY-GPS6MV2,
 * "NEO-6M-0-001", V1.2.2...) por UART.
 *
 * El GPS manda texto NMEA sin parar (por defecto 9600 baud, 1 vez/s):
 *   $GPRMC  posición, velocidad, rumbo, fecha/hora, válido sí/no
 *   $GPGGA  posición, altitud, calidad del fix, satélites usados, HDOP
 *   $GPGSA  tipo de fix (2D/3D), PDOP/HDOP/VDOP
 *   $GPGSV  satélites a la vista con su señal (SNR)
 * y acepta comandos binarios UBX (cambiar baudios, frecuencia, mensajes...).
 *
 * Una tarea interna lee el UART, verifica el checksum de cada frase y
 * agrupa las frases de una misma "época" (una medida). Cuando el GPS deja
 * de hablar ~30 ms, la época se da por terminada y se publica:
 *   - neo6m_get()          última época (no bloquea)
 *   - neo6m_wait_update()  espera a la siguiente
 *   - neo6m_set_callback() función llamada en cada época (desde la tarea)
 *
 * Conexión cruzada: TX del GPS -> rx_gpio del ESP, RX del GPS -> tx_gpio.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NEO6M_MAX_SATS 16

typedef struct {
    int uart_num;          // 1 o 2 (el UART0 lo usa el arranque del chip)
    int tx_gpio;           // ESP TX -> RX del GPS
    int rx_gpio;           // ESP RX <- TX del GPS
    uint32_t baud;         // baudios actuales del GPS; 0 = autodetectar
    uint32_t baud_target;  // 0 = no tocar; si no, se reconfigura el GPS (UBX CFG-PRT)
    uint16_t rate_ms;      // 0 = no tocar; 1000 = 1 Hz, 200 = 5 Hz (UBX CFG-RATE)
    uint16_t idle_ms;      // silencio que cierra una época
    int task_core;         // núcleo de la tarea lectora (tskNO_AFFINITY = cualquiera)
    int task_prio;
} neo6m_config_t;

// Cableado propuesto en la ESP32-S3 del proyecto: TX=GPIO17, RX=GPIO18 (UART1).
#define NEO6M_CONFIG_DEFAULT()                                                         \
    {                                                                                  \
        .uart_num = 1, .tx_gpio = 17, .rx_gpio = 18, .baud = 0, .baud_target = 0,      \
        .rate_ms = 0, .idle_ms = 30, .task_core = 0, .task_prio = 5,                   \
    }

typedef struct {
    uint8_t prn;    // número del satélite
    int8_t elev;    // elevación en grados (0..90)
    uint16_t azim;  // azimut en grados (0..359)
    uint8_t snr;    // señal en dBHz; 0 = visible pero sin seguir
} neo6m_sat_t;

typedef struct {
    bool valid;          // RMC: 'A' = posición válida
    uint8_t fix_type;    // GSA: 1 = sin fix, 2 = 2D, 3 = 3D
    uint8_t quality;     // GGA: 0 sin fix, 1 GPS, 2 DGPS/SBAS, 6 estimado
    double lat, lon;     // grados decimales (+ = Norte / Este)
    float alt_m;         // altitud sobre el nivel medio del mar
    float geoid_sep_m;   // separación geoide-elipsoide
    float speed_kmh;
    float course_deg;    // rumbo sobre el suelo
    float pdop, hdop, vdop;  // dilución de precisión (menos = mejor; <2 es bueno)
    uint8_t sats_used;   // GGA
    uint8_t sats_view;   // GSV
    neo6m_sat_t sats[NEO6M_MAX_SATS];
    uint8_t n_sats;      // entradas válidas en sats[]
    bool time_valid, date_valid;
    uint16_t year;
    uint8_t month, day, hour, minute, second;  // UTC
    uint16_t millis;
    int64_t update_us;   // esp_timer_get_time() al publicar la época
    uint32_t epochs;     // épocas publicadas
    uint32_t nmea_ok;    // frases con checksum correcto
    uint32_t nmea_bad;   // frases con checksum incorrecto (ruido, baudios mal)
} neo6m_data_t;

typedef struct neo6m_dev *neo6m_handle_t;
typedef void (*neo6m_callback_t)(const neo6m_data_t *data, void *arg);

typedef enum {
    NEO6M_NMEA_GGA = 0x00,
    NEO6M_NMEA_GLL = 0x01,
    NEO6M_NMEA_GSA = 0x02,
    NEO6M_NMEA_GSV = 0x03,
    NEO6M_NMEA_RMC = 0x04,
    NEO6M_NMEA_VTG = 0x05,
} neo6m_nmea_t;

typedef enum {
    NEO6M_START_HOT = 0x0000,   // conserva efemérides: fix en ~1 s
    NEO6M_START_WARM = 0x0001,  // borra efemérides: ~30 s
    NEO6M_START_COLD = 0xFFFF,  // borra todo: ~30 s a varios minutos
} neo6m_start_t;

/* --- Ciclo de vida -------------------------------------------------------- */
/* Instala el UART, detecta los baudios si baud = 0, comprueba que llegan
 * frases NMEA válidas y arranca la tarea lectora. Después aplica baud_target
 * y rate_ms si se pidieron (eso necesita también el cable TX del ESP).
 * Errores: ESP_ERR_NOT_FOUND = no llega NMEA (revisa TX del GPS -> rx_gpio),
 *          ESP_ERR_TIMEOUT   = el GPS no confirma comandos (revisa tx_gpio). */
esp_err_t neo6m_init(const neo6m_config_t *cfg, neo6m_handle_t *out);
esp_err_t neo6m_deinit(neo6m_handle_t dev);

/* --- Datos ---------------------------------------------------------------- */
void neo6m_get(neo6m_handle_t dev, neo6m_data_t *out);
/* Espera la siguiente época. false = timeout. Pensado para UNA tarea lectora. */
bool neo6m_wait_update(neo6m_handle_t dev, neo6m_data_t *out, TickType_t timeout);
/* Callback por época, ejecutado en la tarea del driver: que sea corto. */
void neo6m_set_callback(neo6m_handle_t dev, neo6m_callback_t cb, void *arg);

/* --- Configuración del GPS (UBX) ------------------------------------------ */
/* Los cambios viven en la RAM del GPS: se pierden al quitarle la corriente
 * salvo neo6m_save_config(). Al reiniciar solo el ESP, el GPS los conserva
 * (por eso init autodetecta los baudios). */
uint32_t neo6m_get_baud(neo6m_handle_t dev);
esp_err_t neo6m_set_baud(neo6m_handle_t dev, uint32_t baud);
/* A 9600 baud caben ~2 épocas/s con los mensajes por defecto: para 5 Hz sube
 * a 38400 o desactiva GSV/GLL/VTG. */
esp_err_t neo6m_set_rate(neo6m_handle_t dev, uint16_t period_ms);
esp_err_t neo6m_set_nmea(neo6m_handle_t dev, neo6m_nmea_t msg, bool enable);
esp_err_t neo6m_save_config(neo6m_handle_t dev);  // a la EEPROM/BBR del módulo
esp_err_t neo6m_restart(neo6m_handle_t dev, neo6m_start_t mode);
esp_err_t neo6m_get_version(neo6m_handle_t dev, char *sw, size_t sw_len, char *hw, size_t hw_len);

/* UBX genérico. wait_ack: espera ACK-ACK (ESP_OK), ACK-NAK (ESP_FAIL) o timeout. */
esp_err_t neo6m_ubx_send(neo6m_handle_t dev, uint8_t cls, uint8_t id, const void *payload, uint16_t len,
                         bool wait_ack);
/* Envía una consulta (payload vacío) y copia la respuesta con el mismo cls/id. */
esp_err_t neo6m_ubx_poll(neo6m_handle_t dev, uint8_t cls, uint8_t id, uint8_t *resp, size_t max,
                         size_t *resp_len, TickType_t timeout);

/* --- Funciones puras (sin hardware, útiles para pruebas) ------------------ */
bool neo6m_nmea_checksum_ok(const char *line);
/* Aplica una frase NMEA completa ("$GPRMC,...*6A") sobre *d.
 * ESP_OK, ESP_ERR_INVALID_CRC, ESP_ERR_NOT_SUPPORTED (tipo no usado), ESP_ERR_INVALID_ARG. */
esp_err_t neo6m_nmea_parse(neo6m_data_t *d, const char *line);
/* Arma una trama UBX (sync + cabecera + payload + checksum). Devuelve su
 * longitud o 0 si no cabe en out. */
size_t neo6m_ubx_frame(uint8_t cls, uint8_t id, const void *payload, uint16_t len, uint8_t *out,
                       size_t out_size);
/* Distancia (m) y rumbo inicial (grados, 0 = Norte) entre dos puntos. */
double neo6m_distance_m(double lat1, double lon1, double lat2, double lon2);
double neo6m_bearing_deg(double lat1, double lon1, double lat2, double lon2);
/* Hora UTC como tiempo Unix; -1 si fecha u hora no son válidas. */
time_t neo6m_unix_time(const neo6m_data_t *d);

#ifdef __cplusplus
}
#endif
