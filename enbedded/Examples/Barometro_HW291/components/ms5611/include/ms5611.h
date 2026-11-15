/*
 * ms5611 — driver ESP-IDF 6 para el barómetro MS5611 (módulos HW-291 / GY-63)
 * y su hermano MS5607, por I2C con el driver nuevo `esp_driver_i2c`.
 *
 * El chip no tiene registros: se le mandan comandos.
 *   0x1E          reset (recarga la PROM)
 *   0xA0..0xAE    leer PROM (C0..C7, 16 bit cada uno; C7 lleva el CRC4)
 *   0x40 + 2*osr  convertir D1 (presión sin compensar)
 *   0x50 + 2*osr  convertir D2 (temperatura sin compensar)
 *   0x00          leer el resultado de 24 bit de la última conversión
 *
 * Tres niveles de uso:
 *   1. ms5611_read()                     bloqueante: D1 + D2 + compensación
 *   2. ms5611_start() / ms5611_fetch()   asíncrono: la tarea hace otra cosa
 *                                        mientras el ADC convierte (hasta 9 ms)
 *   3. ms5611_calc()                     solo matemáticas (sin I2C): útil para
 *                                        pruebas o para compensar datos guardados
 *
 * Unidades: presión en Pa (1 hPa = 1 mbar = 100 Pa), temperatura en °C.
 * Un mismo handle no debe usarse desde dos tareas a la vez (la secuencia
 * start → esperar → fetch no es atómica). El bus I2C sí puede compartirse.
 */
#pragma once

#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Sobremuestreo: más OSR = menos ruido pero conversión más lenta.
 * Tiempo máximo de conversión (datasheet) y ruido típico de presión:
 *   256: 0.60 ms / 6.5 Pa     512: 1.17 ms / 4.2 Pa    1024: 2.28 ms / 2.7 Pa
 *   2048: 4.54 ms / 1.8 Pa    4096: 9.04 ms / 1.2 Pa   (1.2 Pa ≈ 10 cm de altura) */
typedef enum {
    MS5611_OSR_256 = 0,
    MS5611_OSR_512,
    MS5611_OSR_1024,
    MS5611_OSR_2048,
    MS5611_OSR_4096,
} ms5611_osr_t;

/* MS5607 y MS5611 son idénticos por I2C (mismos comandos, PROM y CRC), pero
 * en el MS5607 OFF y SENS valen el doble: con la fórmula equivocada la presión
 * sale exactamente a la mitad o al doble. El HW-291 de esta placa resultó ser
 * MS5607 (rotulado como MS5611): con la fórmula 5611 daba 507 hPa.
 *
 * AUTO: en init hace una lectura y elige la variante que da una presión de
 * superficie (> 540 hPa). Válido mientras el equipo arranque por debajo de
 * ~5000 m de altitud; para globos/aviones fija la variante a mano. */
typedef enum {
    MS5611_VARIANT_MS5611 = 0,
    MS5611_VARIANT_MS5607,
    MS5611_VARIANT_AUTO,
} ms5611_variant_t;

typedef enum {
    MS5611_D1_PRESSURE = 0x40,
    MS5611_D2_TEMPERATURE = 0x50,
} ms5611_channel_t;

typedef struct {
    /* Bus ya creado (p. ej. compartido con un MPU6050). Si es NULL, el driver
     * crea su propio bus con sda_gpio/scl_gpio/port y lo borra en deinit. */
    i2c_master_bus_handle_t bus;
    int sda_gpio;
    int scl_gpio;
    int port;                 // -1 = el driver elige un puerto libre
    uint8_t addr;             // 0x77 (CSB a GND, HW-291 por defecto) o 0x76
    uint32_t i2c_hz;          // hasta 400 kHz
    ms5611_osr_t osr_pressure;
    ms5611_osr_t osr_temperature;
    ms5611_variant_t variant;
} ms5611_config_t;

// Cableado verificado en esta placa (ESP32-S3, diagnóstico): SDA=8, SCL=9, 0x77.
#define MS5611_CONFIG_DEFAULT()                                                         \
    {                                                                                   \
        .bus = NULL, .sda_gpio = 8, .scl_gpio = 9, .port = -1, .addr = 0x77,            \
        .i2c_hz = 400000, .osr_pressure = MS5611_OSR_4096,                              \
        .osr_temperature = MS5611_OSR_4096, .variant = MS5611_VARIANT_AUTO,             \
    }

typedef struct {
    int32_t temp_c100;    // temperatura en centésimas de °C (2007 = 20.07 °C)
    int32_t pressure_pa;  // presión en Pa (100009 = 1000.09 mbar)
    float temp_c;         // lo mismo en float, por comodidad
    float pressure_hpa;
} ms5611_data_t;

typedef struct ms5611_dev *ms5611_handle_t;

/* Reset + lectura de PROM + verificación de CRC4.
 * Errores: ESP_ERR_NOT_FOUND (nadie responde en addr), ESP_ERR_INVALID_CRC. */
esp_err_t ms5611_init(const ms5611_config_t *cfg, ms5611_handle_t *out);
esp_err_t ms5611_deinit(ms5611_handle_t dev);

esp_err_t ms5611_reset(ms5611_handle_t dev);                // vuelve a leer la PROM
void ms5611_get_prom(ms5611_handle_t dev, uint16_t prom[8]); // C0..C7 tal cual
/* Variante en uso (nunca AUTO: tras init ya está resuelta). */
ms5611_variant_t ms5611_get_variant(ms5611_handle_t dev);
esp_err_t ms5611_set_osr(ms5611_handle_t dev, ms5611_osr_t pressure, ms5611_osr_t temperature);

/* Tiempo máximo de conversión en µs para un OSR (600..9040). */
uint32_t ms5611_conversion_time_us(ms5611_osr_t osr);

/* --- Nivel 1: bloqueante ------------------------------------------------- */
esp_err_t ms5611_read_raw(ms5611_handle_t dev, uint32_t *d1, uint32_t *d2);
esp_err_t ms5611_read(ms5611_handle_t dev, ms5611_data_t *out);

/* --- Nivel 2: asíncrono -------------------------------------------------- */
/* Lanza una conversión con el OSR configurado para ese canal. Hay que esperar
 * ms5611_conversion_time_us() antes de ms5611_fetch(); si se lee antes, el
 * chip devuelve 0 y fetch da ESP_ERR_INVALID_RESPONSE. */
esp_err_t ms5611_start(ms5611_handle_t dev, ms5611_channel_t ch);
esp_err_t ms5611_fetch(ms5611_handle_t dev, uint32_t *adc);
/* Compensa con la PROM de este chip (sin I2C). */
void ms5611_compensate(ms5611_handle_t dev, uint32_t d1, uint32_t d2, ms5611_data_t *out);

/* --- Nivel 3: solo cálculo ----------------------------------------------- */
/* Compensación de 1er y 2º orden del datasheet (AN520 / hoja MS5611-01BA03). */
void ms5611_calc(const uint16_t prom[8], ms5611_variant_t variant, uint32_t d1, uint32_t d2,
                 ms5611_data_t *out);
/* CRC4 de la PROM (algoritmo AN520). Devuelve el CRC calculado (0..15). */
uint8_t ms5611_crc4(const uint16_t prom[8]);

/* --- Utilidades de altitud (atmósfera estándar ISA) ---------------------- */
/* Altitud en m sobre el nivel donde la presión vale ref_pa.
 * ref_pa = 101325 → altitud aproximada sobre el mar;
 * ref_pa = presión medida al arrancar → altura relativa al punto de arranque. */
float ms5611_altitude_m(float pressure_pa, float ref_pa);
/* Presión equivalente a nivel del mar sabiendo la altitud real del lugar (QNH). */
float ms5611_sea_level_pa(float pressure_pa, float altitude_m);

#ifdef __cplusplus
}
#endif
