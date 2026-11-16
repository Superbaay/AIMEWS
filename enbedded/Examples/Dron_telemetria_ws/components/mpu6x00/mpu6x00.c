#include "mpu6x00.h"

#include <stdlib.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "mpu6x00";

// Registros
#define R_SMPLRT_DIV    0x19
#define R_CONFIG        0x1A
#define R_GYRO_CONFIG   0x1B
#define R_ACCEL_CONFIG  0x1C
#define R_ACCEL_CONFIG2 0x1D  // solo MPU6500
#define R_FIFO_EN       0x23
#define R_INT_PIN_CFG   0x37
#define R_INT_ENABLE    0x38
#define R_INT_STATUS    0x3A
#define R_ACCEL_XOUT_H  0x3B
#define R_USER_CTRL     0x6A
#define R_PWR_MGMT_1    0x6B
#define R_FIFO_COUNT_H  0x72
#define R_FIFO_R_W      0x74
#define R_WHO_AM_I      0x75

// 14 B por muestra: accel (6) + TEMPERATURA (2) + giro (6), el mismo orden que
// los registros. La temperatura va como marca de sincronía: cambia despacio, así
// que si en su lugar aparece otra cosa, el FIFO está corrido (ver mpu_fifo_read).
#define FIFO_SAMPLE_BYTES 14
#define FIFO_MAX_BYTES    512  // 6500 = 512 B, 6050 = 1024 B: usamos el menor

struct mpu_dev {
    i2c_master_bus_handle_t bus;
    bool own_bus;     // true si el bus lo creó este driver
    i2c_master_dev_handle_t i2c;
    mpu_config_t cfg;
    uint8_t who;
    bool is_6050;
    float accel_lsb;  // LSB por g
    float gyro_lsb;   // LSB por grado/s
    float bias[3];    // grados/s
    TaskHandle_t notify_task;
};

// Timeout corto: si el bus (compartido) se cuelga, el driver lo reinicia a los
// 5 ms en vez de 20 y las muestras siguen esperando en el FIFO del sensor.
#define I2C_TIMEOUT_MS 5

static esp_err_t wr(mpu_handle_t d, uint8_t reg, uint8_t val) {
    uint8_t b[2] = {reg, val};
    return i2c_master_transmit(d->i2c, b, 2, I2C_TIMEOUT_MS);
}

static esp_err_t rd(mpu_handle_t d, uint8_t reg, uint8_t *dst, size_t n) {
    return i2c_master_transmit_receive(d->i2c, &reg, 1, dst, n, I2C_TIMEOUT_MS);
}

static inline int16_t be16(const uint8_t *p) { return (int16_t)((p[0] << 8) | p[1]); }

esp_err_t mpu_init(const mpu_config_t *cfg, mpu_handle_t *out) {
    ESP_RETURN_ON_FALSE(cfg->dlpf >= 1 && cfg->dlpf <= 6, ESP_ERR_INVALID_ARG, TAG, "dlpf 1..6");
    ESP_RETURN_ON_FALSE(cfg->rate_hz >= 4 && cfg->rate_hz <= 1000, ESP_ERR_INVALID_ARG, TAG,
                        "rate 4..1000");
    mpu_handle_t d = calloc(1, sizeof *d);
    ESP_RETURN_ON_FALSE(d, ESP_ERR_NO_MEM, TAG, "sin memoria");
    d->cfg = *cfg;

    // Se llama "ret" porque ESP_GOTO_ON_ERROR escribe el error en esa variable.
    esp_err_t ret = ESP_OK;
    if (cfg->bus) {
        d->bus = cfg->bus;  // bus compartido: no es nuestro, no se borra
    } else {
        i2c_master_bus_config_t bcfg = {
            .i2c_port = -1,  // el driver elige un puerto libre
            .sda_io_num = cfg->sda_gpio,
            .scl_io_num = cfg->scl_gpio,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,  // ayuda, pero el módulo trae los suyos
        };
        ret = i2c_new_master_bus(&bcfg, &d->bus);
        if (ret != ESP_OK) { free(d); return ret; }
        d->own_bus = true;
    }
    i2c_device_config_t dcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = cfg->addr,
        .scl_speed_hz = cfg->i2c_hz,
    };
    ESP_GOTO_ON_ERROR(i2c_master_bus_add_device(d->bus, &dcfg, &d->i2c), fail, TAG, "add_device");

    ESP_GOTO_ON_ERROR(rd(d, R_WHO_AM_I, &d->who, 1), fail, TAG, "sin respuesta en 0x%02X",
                      cfg->addr);
    d->is_6050 = (d->who == 0x68);

    ESP_GOTO_ON_ERROR(wr(d, R_PWR_MGMT_1, 0x80), fail, TAG, "reset");
    vTaskDelay(pdMS_TO_TICKS(100));
    wr(d, R_PWR_MGMT_1, 0x01);                       // despertar, reloj PLL del giro X
    wr(d, R_CONFIG, cfg->dlpf);                      // DLPF -> base 1 kHz
    wr(d, R_SMPLRT_DIV, (uint8_t)(1000 / cfg->rate_hz - 1));
    wr(d, R_GYRO_CONFIG, (uint8_t)(cfg->gyro_fs << 3));
    wr(d, R_ACCEL_CONFIG, (uint8_t)(cfg->accel_fs << 3));
    if (!d->is_6050) wr(d, R_ACCEL_CONFIG2, cfg->dlpf);  // filtro del accel del 6500
    wr(d, R_INT_ENABLE, 0x00);
    vTaskDelay(pdMS_TO_TICKS(20));                   // dejar que el PLL se estabilice

    static const float accel_lsb[] = {16384.f, 8192.f, 4096.f, 2048.f};
    static const float gyro_lsb[] = {131.f, 65.5f, 32.8f, 16.4f};
    d->accel_lsb = accel_lsb[cfg->accel_fs];
    d->gyro_lsb = gyro_lsb[cfg->gyro_fs];

    ESP_LOGI(TAG, "%s (WHO_AM_I=0x%02X) en 0x%02X, %u Hz, DLPF %u", mpu_model(d), d->who,
             cfg->addr, cfg->rate_hz, cfg->dlpf);
    *out = d;
    return ESP_OK;

fail:
    if (d->i2c) i2c_master_bus_rm_device(d->i2c);
    // Diagnóstico: ver quién responde en el bus antes de rendirnos.
    ESP_LOGW(TAG, "Escaneando bus I2C (SDA=%d, SCL=%d)...", cfg->sda_gpio, cfg->scl_gpio);
    int encontrados = 0;
    for (uint8_t a = 0x08; a < 0x78; a++) {
        if (i2c_master_probe(d->bus, a, 20) == ESP_OK) {
            ESP_LOGW(TAG, "  dispositivo en 0x%02X%s", a,
                     (a == 0x68 || a == 0x69) ? "  <- parece la IMU" : "");
            encontrados++;
        }
    }
    if (encontrados == 0) {
        ESP_LOGW(TAG, "  nadie responde: revisa cables, alimentación o SDA/SCL invertidos");
    }
    if (d->own_bus) i2c_del_master_bus(d->bus);
    free(d);
    return ret;
}

uint8_t mpu_who_am_i(mpu_handle_t d) { return d->who; }

const char *mpu_model(mpu_handle_t d) {
    switch (d->who) {
        case 0x68: return "MPU6050";
        case 0x70: return "MPU6500";
        case 0x71: return "MPU9250";
        default:   return "clon desconocido";
    }
}

esp_err_t mpu_read_raw(mpu_handle_t d, mpu_raw_t *r) {
    uint8_t b[14];
    esp_err_t e = rd(d, R_ACCEL_XOUT_H, b, sizeof b);
    if (e != ESP_OK) return e;
    r->ax = be16(b + 0);  r->ay = be16(b + 2);  r->az = be16(b + 4);
    r->temp = be16(b + 6);
    r->gx = be16(b + 8);  r->gy = be16(b + 10); r->gz = be16(b + 12);
    return ESP_OK;
}

void mpu_convert(mpu_handle_t d, const mpu_raw_t *r, mpu_data_t *o) {
    o->ax = r->ax / d->accel_lsb;
    o->ay = r->ay / d->accel_lsb;
    o->az = r->az / d->accel_lsb;
    o->gx = r->gx / d->gyro_lsb - d->bias[0];
    o->gy = r->gy / d->gyro_lsb - d->bias[1];
    o->gz = r->gz / d->gyro_lsb - d->bias[2];
    // Cada familia usa una fórmula distinta (ver datasheets).
    o->temp_c = d->is_6050 ? r->temp / 340.f + 36.53f : r->temp / 333.87f + 21.f;
}

esp_err_t mpu_read(mpu_handle_t d, mpu_data_t *o) {
    mpu_raw_t r;
    esp_err_t e = mpu_read_raw(d, &r);
    if (e == ESP_OK) mpu_convert(d, &r, o);
    return e;
}

/* ---------------- Interrupción ---------------- */

static void IRAM_ATTR isr_data_ready(void *arg) {
    mpu_handle_t d = arg;
    BaseType_t hp = pdFALSE;
    vTaskNotifyGiveFromISR(d->notify_task, &hp);
    portYIELD_FROM_ISR(hp);
}

esp_err_t mpu_int_attach(mpu_handle_t d, TaskHandle_t task) {
    ESP_RETURN_ON_FALSE(d->cfg.int_gpio >= 0, ESP_ERR_INVALID_STATE, TAG, "INT no cableado");
    d->notify_task = task;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << d->cfg.int_gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio_config");
    esp_err_t e = gpio_install_isr_service(0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;  // ya instalado = OK
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(d->cfg.int_gpio, isr_data_ready, d), TAG, "isr");
    // Pulso de 50 us, activo alto, push-pull: no necesita leer INT_STATUS.
    ESP_RETURN_ON_ERROR(wr(d, R_INT_PIN_CFG, 0x10), TAG, "int_pin_cfg");
    return wr(d, R_INT_ENABLE, 0x01);  // DATA_RDY
}

uint32_t mpu_wait_sample(TickType_t timeout) { return ulTaskNotifyTake(pdTRUE, timeout); }

/* ---------------- FIFO ---------------- */

esp_err_t mpu_fifo_reset(mpu_handle_t d) {
    ESP_RETURN_ON_ERROR(wr(d, R_USER_CTRL, 0x04), TAG, "fifo_reset");  // FIFO_RESET
    vTaskDelay(pdMS_TO_TICKS(1));
    return wr(d, R_USER_CTRL, 0x40);                                   // FIFO_EN
}

esp_err_t mpu_fifo_start(mpu_handle_t d) {
    ESP_RETURN_ON_ERROR(wr(d, R_FIFO_EN, 0x00), TAG, "fifo_en");
    ESP_RETURN_ON_ERROR(mpu_fifo_reset(d), TAG, "reset");
    return wr(d, R_FIFO_EN, 0xF8);  // temperatura + giro XYZ + accel XYZ
}

int mpu_fifo_read(mpu_handle_t d, mpu_raw_t *out, int max) {
    uint8_t c[2];
    // Sin leer INT_STATUS (una transacción menos por muestra en el bus
    // compartido): un FIFO lleno ya indica desbordamiento.
    if (rd(d, R_FIFO_COUNT_H, c, 2) != ESP_OK) return 0;
    int bytes = ((c[0] & 0x1F) << 8) | c[1];
    if (bytes >= FIFO_MAX_BYTES) {  // lleno: pudo desbordar y los datos estarían desalineados
        mpu_fifo_reset(d);
        return -1;
    }
    int n = bytes / FIFO_SAMPLE_BYTES;
    if (n > max) n = max;
    if (n == 0) return 0;
    uint8_t buf[FIFO_MAX_BYTES];
    if (rd(d, R_FIFO_R_W, buf, n * FIFO_SAMPLE_BYTES) != ESP_OK) {
        // Una lectura cortada a la mitad ya sacó parte de los bytes del FIFO: desde
        // ahí cada muestra quedaría corrida (acelerómetro leído como giroscopio,
        // medido: ~480 °/s con la placa quieta). Se reinicia para realinear.
        mpu_fifo_reset(d);
        return -1;
    }
    for (int i = 0; i < n; i++) {
        const uint8_t *p = buf + i * FIFO_SAMPLE_BYTES;
        out[i] = (mpu_raw_t){.ax = be16(p), .ay = be16(p + 2), .az = be16(p + 4), .temp = be16(p + 6),
                             .gx = be16(p + 8), .gy = be16(p + 10), .gz = be16(p + 12)};
    }
    return n;
}

/* ---------------- Calibración ---------------- */

esp_err_t mpu_calibrate_gyro(mpu_handle_t d, int n) {
    double s[3] = {0};
    int validas = 0, fallos = 0;
    // Una lectura fallida suelta (bus compartido, cable largo) no arruina la
    // calibración: se descarta. Solo se rinde si falla más del 10 %.
    while (validas < n) {
        mpu_raw_t r;
        if (mpu_read_raw(d, &r) == ESP_OK) {
            s[0] += r.gx; s[1] += r.gy; s[2] += r.gz;
            validas++;
        } else if (++fallos > n / 10) {
            ESP_LOGE(TAG, "calibración: %d lecturas fallidas", fallos);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);  // con CONFIG_FREERTOS_HZ=1000: ~1 muestra nueva por lectura
    }
    if (fallos) ESP_LOGW(TAG, "calibración: %d lecturas fallidas descartadas", fallos);
    for (int k = 0; k < 3; k++) d->bias[k] = (float)(s[k] / n / d->gyro_lsb);
    ESP_LOGI(TAG, "bias giro = %+.3f %+.3f %+.3f dps", d->bias[0], d->bias[1], d->bias[2]);
    return ESP_OK;
}

void mpu_set_gyro_bias(mpu_handle_t d, float bx, float by, float bz) {
    d->bias[0] = bx; d->bias[1] = by; d->bias[2] = bz;
}
