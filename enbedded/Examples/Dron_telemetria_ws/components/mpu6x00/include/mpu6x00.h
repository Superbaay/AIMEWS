/*
 * mpu6x00 — driver mínimo para MPU6050 / MPU6500 (y clones) por I2C en ESP-IDF 6.
 *
 * Tres formas de leer:
 *   1. Bajo demanda:      mpu_read()                       (simple, sin sincronizar)
 *   2. Por interrupción:  mpu_int_attach() + mpu_wait_sample() + mpu_read()
 *                         (una muestra cada 1 ms, latencia mínima: control de vuelo)
 *   3. Por FIFO:          mpu_fifo_start() + mpu_fifo_read()  (ráfagas, tolera retrasos)
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { MPU_ACCEL_2G = 0, MPU_ACCEL_4G, MPU_ACCEL_8G, MPU_ACCEL_16G } mpu_accel_fs_t;
typedef enum { MPU_GYRO_250 = 0, MPU_GYRO_500, MPU_GYRO_1000, MPU_GYRO_2000 } mpu_gyro_fs_t;

/*
 * Filtro paso bajo interno (registro CONFIG, DLPF_CFG). Ancho de banda del giro
 * y retardo aproximados (datasheet MPU6050):
 *   1: 188 Hz / 1.9 ms   2: 98 Hz / 2.8 ms   3: 42 Hz / 4.8 ms
 *   4:  20 Hz / 8.3 ms   5: 10 Hz / 13.4 ms  6:  5 Hz / 18.6 ms
 * Con 1..6 la frecuencia base es 1 kHz. (0 = sin filtro y base 8 kHz: no soportado aquí.)
 */
typedef struct {
    /* Bus ya creado (p. ej. compartido con el barómetro MS5611). Si es NULL,
     * el driver crea su propio bus con sda_gpio/scl_gpio. */
    i2c_master_bus_handle_t bus;
    int sda_gpio;
    int scl_gpio;
    int int_gpio;            // -1 si INT no está cableado
    uint8_t addr;            // 0x68 (AD0 a GND) o 0x69
    uint32_t i2c_hz;         // 400000 para llegar a 1 kHz
    mpu_accel_fs_t accel_fs;
    mpu_gyro_fs_t gyro_fs;
    uint8_t dlpf;            // 1..6
    uint16_t rate_hz;        // 4..1000 (divisor de 1 kHz)
} mpu_config_t;

// Cableado verificado en esta placa: SDA=8, SCL=9, INT=10.
#define MPU_CONFIG_DEFAULT()                                                              \
    {                                                                                     \
        .sda_gpio = 8, .scl_gpio = 9, .int_gpio = 10, .addr = 0x68, .i2c_hz = 400000,     \
        .accel_fs = MPU_ACCEL_4G, .gyro_fs = MPU_GYRO_500, .dlpf = 1, .rate_hz = 1000,    \
    }

typedef struct {
    int16_t ax, ay, az, temp, gx, gy, gz;
} mpu_raw_t;

typedef struct {
    float ax, ay, az;   // g
    float gx, gy, gz;   // grados/s (ya restado el bias del giro)
    float temp_c;       // °C
} mpu_data_t;

typedef struct mpu_dev *mpu_handle_t;

esp_err_t mpu_init(const mpu_config_t *cfg, mpu_handle_t *out);
uint8_t mpu_who_am_i(mpu_handle_t dev);
const char *mpu_model(mpu_handle_t dev);

esp_err_t mpu_read_raw(mpu_handle_t dev, mpu_raw_t *raw);
void mpu_convert(mpu_handle_t dev, const mpu_raw_t *raw, mpu_data_t *out);
esp_err_t mpu_read(mpu_handle_t dev, mpu_data_t *out);

/* Interrupción DATA_RDY: cada muestra nueva notifica a `task`. */
esp_err_t mpu_int_attach(mpu_handle_t dev, TaskHandle_t task);
/* Espera la siguiente muestra. Devuelve cuántas llegaron desde la última llamada
 * (1 = normal, >1 = se perdieron n-1, 0 = timeout). */
uint32_t mpu_wait_sample(TickType_t timeout);

/* FIFO: accel + temperatura + giro (14 B por muestra, mismo orden que los registros). */
esp_err_t mpu_fifo_start(mpu_handle_t dev);
esp_err_t mpu_fifo_reset(mpu_handle_t dev);
/* Lee las muestras completas que haya (hasta max). Devuelve la cantidad o -1 si
 * hubo desbordamiento (el FIFO se reinicia y se pierden esas muestras). */
int mpu_fifo_read(mpu_handle_t dev, mpu_raw_t *out, int max);

/* Bias del giroscopio: promedia n muestras con el sensor QUIETO. */
esp_err_t mpu_calibrate_gyro(mpu_handle_t dev, int n);
void mpu_set_gyro_bias(mpu_handle_t dev, float bx, float by, float bz);

#ifdef __cplusplus
}
#endif
