#include "ms5611.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ms5611";

#define CMD_RESET 0x1E
#define CMD_ADC_READ 0x00
#define CMD_PROM_READ 0xA0
#define I2C_TIMEOUT_MS 50

struct ms5611_dev {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t i2c;
    bool own_bus;
    ms5611_variant_t variant;
    ms5611_osr_t osr_p, osr_t;
    uint16_t prom[8];
};

/* ------------------------------------------------------------------------ */
/* Cálculo puro                                                             */
/* ------------------------------------------------------------------------ */

uint8_t ms5611_crc4(const uint16_t prom[8])
{
    uint16_t n[8];
    memcpy(n, prom, sizeof(n));
    n[7] &= 0xFF00;  // los 4 bits bajos de C7 son el propio CRC: fuera del cálculo
    uint16_t rem = 0;
    for (int c = 0; c < 16; c++) {
        rem ^= (c & 1) ? (n[c >> 1] & 0x00FF) : (n[c >> 1] >> 8);
        for (int b = 0; b < 8; b++) rem = (rem & 0x8000) ? (rem << 1) ^ 0x3000 : (rem << 1);
    }
    return (rem >> 12) & 0x0F;
}

void ms5611_calc(const uint16_t c[8], ms5611_variant_t variant, uint32_t d1, uint32_t d2,
                 ms5611_data_t *out)
{
    // Todo en 64 bit: SENS y OFF pasan de 2^32 y D1*SENS llega a ~2^55.
    int64_t dT = (int64_t)d2 - ((int64_t)c[5] << 8);
    int64_t temp = 2000 + ((dT * c[6]) >> 23);
    int64_t off, sens;
    if (variant == MS5611_VARIANT_MS5607) {
        off = ((int64_t)c[2] << 17) + ((c[4] * dT) >> 6);
        sens = ((int64_t)c[1] << 16) + ((c[3] * dT) >> 7);
    } else {
        off = ((int64_t)c[2] << 16) + ((c[4] * dT) >> 7);
        sens = ((int64_t)c[1] << 15) + ((c[3] * dT) >> 8);
    }

    // Compensación de 2º orden: solo por debajo de 20 °C (el sensor se curva).
    if (temp < 2000) {
        int64_t t2 = (dT * dT) >> 31;
        int64_t d = temp - 2000, d2_ = d * d;
        int64_t off2, sens2;
        if (variant == MS5611_VARIANT_MS5607) {
            off2 = (61 * d2_) >> 4;
            sens2 = 2 * d2_;
        } else {
            off2 = (5 * d2_) >> 1;
            sens2 = (5 * d2_) >> 2;
        }
        if (temp < -1500) {
            int64_t e = temp + 1500, e2 = e * e;
            if (variant == MS5611_VARIANT_MS5607) {
                off2 += 15 * e2;
                sens2 += 8 * e2;
            } else {
                off2 += 7 * e2;
                sens2 += (11 * e2) >> 1;
            }
        }
        temp -= t2;
        off -= off2;
        sens -= sens2;
    }

    int64_t p = ((((int64_t)d1 * sens) >> 21) - off) >> 15;
    out->temp_c100 = (int32_t)temp;
    out->pressure_pa = (int32_t)p;
    out->temp_c = temp / 100.0f;
    out->pressure_hpa = p / 100.0f;
}

float ms5611_altitude_m(float pressure_pa, float ref_pa)
{
    // Fórmula barométrica ISA: h = 44330 · (1 − (p/p0)^(1/5.255))
    return 44330.0f * (1.0f - powf(pressure_pa / ref_pa, 0.190295f));
}

float ms5611_sea_level_pa(float pressure_pa, float altitude_m)
{
    return pressure_pa / powf(1.0f - altitude_m / 44330.0f, 5.255f);
}

uint32_t ms5611_conversion_time_us(ms5611_osr_t osr)
{
    static const uint16_t t_us[] = {600, 1170, 2280, 4540, 9040};
    return t_us[osr <= MS5611_OSR_4096 ? osr : MS5611_OSR_4096];
}

/* ------------------------------------------------------------------------ */
/* I2C                                                                      */
/* ------------------------------------------------------------------------ */

static esp_err_t send_cmd(ms5611_handle_t dev, uint8_t cmd)
{
    return i2c_master_transmit(dev->i2c, &cmd, 1, I2C_TIMEOUT_MS);
}

/* Espera al menos `us` microsegundos: duerme la tarea casi todo el tiempo
 * (deja la CPU libre) y remata con espera activa para no perder un tick entero. */
static void wait_us(uint32_t us)
{
    int64_t t0 = esp_timer_get_time();
    const uint32_t tick_us = portTICK_PERIOD_MS * 1000;
    if (us > 2 * tick_us) vTaskDelay(us / tick_us - 1);
    int64_t rest = us - (esp_timer_get_time() - t0);
    if (rest > 0) esp_rom_delay_us((uint32_t)rest);
}

static esp_err_t read_prom(ms5611_handle_t dev)
{
    for (int i = 0; i < 8; i++) {
        uint8_t cmd = CMD_PROM_READ + 2 * i, b[2];
        ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(dev->i2c, &cmd, 1, b, 2, I2C_TIMEOUT_MS),
                            TAG, "lectura PROM[%d]", i);
        dev->prom[i] = (uint16_t)((b[0] << 8) | b[1]);
    }
    uint8_t crc = ms5611_crc4(dev->prom);
    if (crc != (dev->prom[7] & 0x0F)) {
        ESP_LOGE(TAG, "CRC PROM incorrecto: calculado %u, leído %u", crc, dev->prom[7] & 0x0F);
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}

esp_err_t ms5611_reset(ms5611_handle_t dev)
{
    ESP_RETURN_ON_FALSE(dev, ESP_ERR_INVALID_ARG, TAG, "handle nulo");
    ESP_RETURN_ON_ERROR(send_cmd(dev, CMD_RESET), TAG, "reset");
    vTaskDelay(pdMS_TO_TICKS(3) + 1);  // el datasheet pide 2.8 ms para recargar la PROM
    return read_prom(dev);
}

esp_err_t ms5611_init(const ms5611_config_t *cfg, ms5611_handle_t *out)
{
    ESP_RETURN_ON_FALSE(cfg && out, ESP_ERR_INVALID_ARG, TAG, "argumento nulo");
    ESP_RETURN_ON_FALSE(cfg->osr_pressure <= MS5611_OSR_4096 && cfg->osr_temperature <= MS5611_OSR_4096,
                        ESP_ERR_INVALID_ARG, TAG, "OSR fuera de rango");
    esp_err_t ret = ESP_OK;
    struct ms5611_dev *dev = calloc(1, sizeof(*dev));
    ESP_RETURN_ON_FALSE(dev, ESP_ERR_NO_MEM, TAG, "sin memoria");
    dev->variant = cfg->variant;
    dev->osr_p = cfg->osr_pressure;
    dev->osr_t = cfg->osr_temperature;

    if (cfg->bus) {
        dev->bus = cfg->bus;
    } else {
        i2c_master_bus_config_t bc = {
            .i2c_port = cfg->port,
            .sda_io_num = cfg->sda_gpio,
            .scl_io_num = cfg->scl_gpio,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,  // el módulo ya trae los suyos
        };
        ESP_GOTO_ON_ERROR(i2c_new_master_bus(&bc, &dev->bus), fail, TAG, "crear bus I2C");
        dev->own_bus = true;
    }

    /* Si el ESP32 se reinició a mitad de una lectura (p. ej. al flashear), el
     * MS5611 puede quedarse esperando relojes con SDA en bajo y el bus parece
     * vacío. i2c_master_bus_reset() da 9 pulsos de SCL + STOP y lo libera. */
    if (i2c_master_probe(dev->bus, cfg->addr, I2C_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "sin respuesta en 0x%02X: recuperando el bus", cfg->addr);
        i2c_master_bus_reset(dev->bus);
        ESP_GOTO_ON_FALSE(i2c_master_probe(dev->bus, cfg->addr, I2C_TIMEOUT_MS) == ESP_OK, ESP_ERR_NOT_FOUND,
                          fail, TAG, "nadie responde en 0x%02X (¿SDA/SCL invertidos o CSB?)", cfg->addr);
    }

    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = cfg->addr,
        .scl_speed_hz = cfg->i2c_hz,
    };
    ESP_GOTO_ON_ERROR(i2c_master_bus_add_device(dev->bus, &dc, &dev->i2c), fail, TAG, "añadir dispositivo");
    ESP_GOTO_ON_ERROR(ms5611_reset(dev), fail, TAG, "reset/PROM");

    if (dev->variant == MS5611_VARIANT_AUTO) {
        // Con la fórmula 5611, un MS5607 en superficie da P/2 (< 543 hPa).
        uint32_t d1, d2;
        ms5611_data_t d;
        ESP_GOTO_ON_ERROR(ms5611_read_raw(dev, &d1, &d2), fail, TAG, "lectura de autodetección");
        ms5611_calc(dev->prom, MS5611_VARIANT_MS5611, d1, d2, &d);
        dev->variant = d.pressure_pa < 54000 ? MS5611_VARIANT_MS5607 : MS5611_VARIANT_MS5611;
        ESP_LOGI(TAG, "autodetección: fórmula 5611 da %.2f hPa -> MS56%s", d.pressure_hpa,
                 dev->variant == MS5611_VARIANT_MS5607 ? "07" : "11");
    }

    ESP_LOGI(TAG, "MS56%s en 0x%02X, C1..C6 = %u %u %u %u %u %u",
             dev->variant == MS5611_VARIANT_MS5607 ? "07" : "11", cfg->addr, dev->prom[1], dev->prom[2],
             dev->prom[3], dev->prom[4], dev->prom[5], dev->prom[6]);
    *out = dev;
    return ESP_OK;

fail:
    ms5611_deinit(dev);
    return ret;
}

esp_err_t ms5611_deinit(ms5611_handle_t dev)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    if (dev->i2c) i2c_master_bus_rm_device(dev->i2c);
    if (dev->own_bus && dev->bus) i2c_del_master_bus(dev->bus);
    free(dev);
    return ESP_OK;
}

void ms5611_get_prom(ms5611_handle_t dev, uint16_t prom[8])
{
    memcpy(prom, dev->prom, sizeof(dev->prom));
}

ms5611_variant_t ms5611_get_variant(ms5611_handle_t dev)
{
    return dev->variant;
}

esp_err_t ms5611_set_osr(ms5611_handle_t dev, ms5611_osr_t pressure, ms5611_osr_t temperature)
{
    ESP_RETURN_ON_FALSE(dev && pressure <= MS5611_OSR_4096 && temperature <= MS5611_OSR_4096,
                        ESP_ERR_INVALID_ARG, TAG, "argumento inválido");
    dev->osr_p = pressure;
    dev->osr_t = temperature;
    return ESP_OK;
}

esp_err_t ms5611_start(ms5611_handle_t dev, ms5611_channel_t ch)
{
    ESP_RETURN_ON_FALSE(dev, ESP_ERR_INVALID_ARG, TAG, "handle nulo");
    ms5611_osr_t osr = (ch == MS5611_D1_PRESSURE) ? dev->osr_p : dev->osr_t;
    return send_cmd(dev, (uint8_t)ch + 2 * osr);
}

esp_err_t ms5611_fetch(ms5611_handle_t dev, uint32_t *adc)
{
    ESP_RETURN_ON_FALSE(dev && adc, ESP_ERR_INVALID_ARG, TAG, "argumento nulo");
    uint8_t cmd = CMD_ADC_READ, b[3];
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(dev->i2c, &cmd, 1, b, 3, I2C_TIMEOUT_MS), TAG,
                        "leer ADC");
    *adc = ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | b[2];
    // 0 = conversión no terminada (o comando interrumpido): el chip no da otra pista.
    return *adc ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t convert(ms5611_handle_t dev, ms5611_channel_t ch, uint32_t *adc)
{
    ESP_RETURN_ON_ERROR(ms5611_start(dev, ch), TAG, "iniciar conversión");
    wait_us(ms5611_conversion_time_us(ch == MS5611_D1_PRESSURE ? dev->osr_p : dev->osr_t));
    return ms5611_fetch(dev, adc);
}

esp_err_t ms5611_read_raw(ms5611_handle_t dev, uint32_t *d1, uint32_t *d2)
{
    ESP_RETURN_ON_FALSE(dev && d1 && d2, ESP_ERR_INVALID_ARG, TAG, "argumento nulo");
    ESP_RETURN_ON_ERROR(convert(dev, MS5611_D1_PRESSURE, d1), TAG, "D1");
    return convert(dev, MS5611_D2_TEMPERATURE, d2);
}

void ms5611_compensate(ms5611_handle_t dev, uint32_t d1, uint32_t d2, ms5611_data_t *out)
{
    ms5611_calc(dev->prom, dev->variant, d1, d2, out);
}

esp_err_t ms5611_read(ms5611_handle_t dev, ms5611_data_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "argumento nulo");
    uint32_t d1, d2;
    ESP_RETURN_ON_ERROR(ms5611_read_raw(dev, &d1, &d2), TAG, "lectura");
    ms5611_compensate(dev, d1, d2, out);
    return ESP_OK;
}
