/**
 * @file esp32cam.c
 * @brief Implementación de la librería de cámara para la AI-Thinker ESP32-CAM.
 */
#include "esp32cam.h"

#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "esp32cam";

// ---------------------------------------------------------------------------
// Patillaje de la AI-Thinker ESP32-CAM
// ---------------------------------------------------------------------------
#define PIN_PWDN   32
#define PIN_RESET  -1   // No conectado: se usa reset por software.
#define PIN_XCLK    0
#define PIN_SIOD   26
#define PIN_SIOC   27
#define PIN_D7     35
#define PIN_D6     34
#define PIN_D5     39
#define PIN_D4     36
#define PIN_D3     21
#define PIN_D2     19
#define PIN_D1     18
#define PIN_D0      5
#define PIN_VSYNC  25
#define PIN_HREF   23
#define PIN_PCLK   22
#define PIN_FLASH   4   // LED blanco de alta potencia (vía transistor).

// El driver de la cámara usa el timer/canal 0 de LEDC para el XCLK;
// el flash usa los siguientes para no pisarlo.
#define FLASH_TIMER    LEDC_TIMER_1
#define FLASH_CHANNEL  LEDC_CHANNEL_1

static SemaphoreHandle_t s_lock;
static bool s_ready;
// PLL personalizado: el driver reescribe el PLL en cada set_framesize, así que
// lo guardamos y lo volvemos a aplicar después de cada cambio de resolución.
static bool s_custom_pll;
static esp32cam_pll_t s_pll;

/** Nombres de las resoluciones, en el mismo orden que framesize_t. */
static const char *const FRAMESIZE_NAMES[FRAMESIZE_INVALID] = {
    [FRAMESIZE_96X96] = "96X96",   [FRAMESIZE_QQVGA] = "QQVGA",   [FRAMESIZE_128X128] = "128X128",
    [FRAMESIZE_QCIF] = "QCIF",     [FRAMESIZE_HQVGA] = "HQVGA",   [FRAMESIZE_240X240] = "240X240",
    [FRAMESIZE_QVGA] = "QVGA",     [FRAMESIZE_320X320] = "320X320", [FRAMESIZE_CIF] = "CIF",
    [FRAMESIZE_HVGA] = "HVGA",     [FRAMESIZE_VGA] = "VGA",       [FRAMESIZE_SVGA] = "SVGA",
    [FRAMESIZE_XGA] = "XGA",       [FRAMESIZE_HD] = "HD",         [FRAMESIZE_SXGA] = "SXGA",
    [FRAMESIZE_UXGA] = "UXGA",     [FRAMESIZE_FHD] = "FHD",       [FRAMESIZE_P_HD] = "P_HD",
    [FRAMESIZE_P_3MP] = "P_3MP",   [FRAMESIZE_QXGA] = "QXGA",     [FRAMESIZE_QHD] = "QHD",
    [FRAMESIZE_WQXGA] = "WQXGA",   [FRAMESIZE_P_FHD] = "P_FHD",   [FRAMESIZE_QSXGA] = "QSXGA",
    [FRAMESIZE_5MP] = "5MP",
};

static inline bool lock(void)
{
    return s_lock != NULL && xSemaphoreTake(s_lock, pdMS_TO_TICKS(3000)) == pdTRUE;
}

static inline void unlock(void)
{
    xSemaphoreGive(s_lock);
}

static esp_err_t flash_init(void)
{
    const ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = FLASH_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t c = {
        .gpio_num = PIN_FLASH,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = FLASH_CHANNEL,
        .timer_sel = FLASH_TIMER,
        .duty = 0,
    };
    esp_err_t err = ledc_timer_config(&t);
    return err == ESP_OK ? ledc_channel_config(&c) : err;
}

esp_err_t esp32cam_init(const esp32cam_config_t *cfg)
{
    if (s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
        esp_err_t ferr = flash_init();
        if (ferr != ESP_OK) {
            ESP_LOGW(TAG, "LED de flash no disponible: %s", esp_err_to_name(ferr));
        }
    }

    const esp32cam_config_t def = { 0 };
    if (cfg == NULL) {
        cfg = &def;
    }
    const bool psram = esp_psram_get_size() > 0;
    if (!psram) {
        ESP_LOGW(TAG, "Sin PSRAM: solo resoluciones pequeñas y 1 buffer");
    }

    camera_config_t cc = {
        .pin_pwdn = PIN_PWDN, .pin_reset = PIN_RESET, .pin_xclk = PIN_XCLK,
        .pin_sccb_sda = PIN_SIOD, .pin_sccb_scl = PIN_SIOC,
        .pin_d7 = PIN_D7, .pin_d6 = PIN_D6, .pin_d5 = PIN_D5, .pin_d4 = PIN_D4,
        .pin_d3 = PIN_D3, .pin_d2 = PIN_D2, .pin_d1 = PIN_D1, .pin_d0 = PIN_D0,
        .pin_vsync = PIN_VSYNC, .pin_href = PIN_HREF, .pin_pclk = PIN_PCLK,
        .xclk_freq_hz = cfg->xclk_hz > 0 ? cfg->xclk_hz : ESP32CAM_DEFAULT_XCLK_HZ,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        // Se arranca en la resolución MÁXIMA para que el driver reserve buffers
        // que sirvan para todas; luego se baja a la pedida.
        .frame_size = psram ? FRAMESIZE_QXGA : FRAMESIZE_QVGA,
        .jpeg_quality = cfg->jpeg_quality > 0 ? cfg->jpeg_quality : 12,
        .fb_count = psram ? (cfg->fb_count > 0 ? (size_t)cfg->fb_count : 2) : 1,
        .fb_location = psram ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM,
        // Con 2+ buffers: el más reciente siempre disponible (latencia mínima).
        .grab_mode = psram ? CAMERA_GRAB_LATEST : CAMERA_GRAB_WHEN_EMPTY,
    };

    // Justo después de flashear (reset por RTS) o con la alimentación aún
    // estabilizándose, el sensor a veces no contesta por SCCB ("probe failed").
    // El driver ya limpia tras un fallo y en cada intento hace el ciclo de PWDN
    // (apaga y enciende el sensor), así que basta con esperar y repetir.
    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= ESP32CAM_INIT_ATTEMPTS; attempt++) {
        err = esp_camera_init(&cc);
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "esp_camera_init falló (intento %d/%d): %s", attempt,
                 ESP32CAM_INIT_ATTEMPTS, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(200 * attempt));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cámara no detectada tras %d intentos", ESP32CAM_INIT_ATTEMPTS);
        return err;
    }
    sensor_t *s = esp_camera_sensor_get();
    if (s == NULL) {
        esp_camera_deinit();
        return ESP_FAIL;
    }
    // El OV3660 de la AI-Thinker va montado al revés respecto al OV2640.
    if (s->id.PID == OV3660_PID) {
        s->set_vflip(s, 1);
    }
    framesize_t fs = cfg->frame_size;
    if (fs >= FRAMESIZE_INVALID || fs > esp32cam_max_framesize()) {
        fs = FRAMESIZE_VGA;
    }
    s->set_framesize(s, fs);
    s_ready = true;
    ESP_LOGI(TAG, "Cámara %s lista (%s, xclk %d kHz, %u buffers)", esp32cam_sensor_name(),
             esp32cam_framesize_name(fs), cc.xclk_freq_hz / 1000, (unsigned)cc.fb_count);
    return ESP_OK;
}

esp_err_t esp32cam_deinit(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!lock()) {
        return ESP_ERR_TIMEOUT;
    }
    s_ready = false;
    s_custom_pll = false;
    esp_err_t err = esp_camera_deinit();
    unlock();
    return err;
}

camera_fb_t *esp32cam_capture(void)
{
    // Sin mutex a propósito: esp_camera_fb_get ya es seguro entre tareas y así
    // un cambio de ajustes lento no retrasa el streaming.
    return s_ready ? esp_camera_fb_get() : NULL;
}

void esp32cam_release(camera_fb_t *fb)
{
    if (fb != NULL) {
        esp_camera_fb_return(fb);
    }
}

const char *esp32cam_sensor_name(void)
{
    sensor_t *s = esp_camera_sensor_get();
    if (s == NULL) {
        return "?";
    }
    camera_sensor_info_t *info = esp_camera_sensor_get_info(&s->id);
    return info != NULL ? info->name : "?";
}

framesize_t esp32cam_max_framesize(void)
{
    sensor_t *s = esp_camera_sensor_get();
    camera_sensor_info_t *info = s ? esp_camera_sensor_get_info(&s->id) : NULL;
    return info != NULL ? info->max_size : FRAMESIZE_UXGA;
}

const char *esp32cam_framesize_name(framesize_t fs)
{
    return (fs < FRAMESIZE_INVALID && FRAMESIZE_NAMES[fs]) ? FRAMESIZE_NAMES[fs] : "?";
}

bool esp32cam_framesize_dims(framesize_t fs, int *w, int *h)
{
    if (fs >= FRAMESIZE_INVALID) {
        return false;
    }
    if (w) *w = resolution[fs].width;
    if (h) *h = resolution[fs].height;
    return true;
}

esp_err_t esp32cam_set_framesize_name(const char *name)
{
    for (int i = 0; name != NULL && i < FRAMESIZE_INVALID; i++) {
        if (FRAMESIZE_NAMES[i] && strcasecmp(name, FRAMESIZE_NAMES[i]) == 0) {
            return esp32cam_set("framesize", i);
        }
    }
    return ESP_ERR_NOT_FOUND;
}

// ---------------------------------------------------------------------------
// Ajustes por nombre: tabla nombre -> función del sensor
// ---------------------------------------------------------------------------
typedef int (*setter_t)(sensor_t *, int);

/** Busca el setter de un ajuste. Se resuelve en tiempo de ejecución porque
 *  cada sensor rellena su propia tabla de punteros. */
static setter_t find_setter(sensor_t *s, const char *name)
{
    const struct {
        const char *name;
        setter_t fn;
    } table[] = {
        {"quality", s->set_quality},
        {"brightness", s->set_brightness},
        {"contrast", s->set_contrast},
        {"saturation", s->set_saturation},
        {"sharpness", s->set_sharpness},
        {"denoise", s->set_denoise},
        {"special_effect", s->set_special_effect},
        {"wb_mode", s->set_wb_mode},
        {"awb", s->set_whitebal},
        {"awb_gain", s->set_awb_gain},
        {"aec", s->set_exposure_ctrl},
        {"aec2", s->set_aec2},
        {"ae_level", s->set_ae_level},
        {"aec_value", s->set_aec_value},
        {"agc", s->set_gain_ctrl},
        {"agc_gain", s->set_agc_gain},
        {"bpc", s->set_bpc},
        {"wpc", s->set_wpc},
        {"raw_gma", s->set_raw_gma},
        {"lenc", s->set_lenc},
        {"hmirror", s->set_hmirror},
        {"vflip", s->set_vflip},
        {"dcw", s->set_dcw},
        {"colorbar", s->set_colorbar},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strcmp(name, table[i].name) == 0) {
            return table[i].fn;
        }
    }
    return NULL;
}

esp_err_t esp32cam_set(const char *name, int value)
{
    if (name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready || !lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    sensor_t *s = esp_camera_sensor_get();
    int rc = -1;
    esp_err_t err = ESP_OK;
    if (strcmp(name, "framesize") == 0) {
        if (value < 0 || value > (int)esp32cam_max_framesize()) {
            err = ESP_ERR_INVALID_ARG;
        } else {
            rc = s->set_framesize(s, (framesize_t)value);
            if (rc == 0 && s_custom_pll && s->set_pll) {
                rc = s->set_pll(s, s_pll.bypass, s_pll.multiplier, s_pll.sys_div, s_pll.root_2x,
                                s_pll.pre_div, s_pll.seld5, s_pll.pclk_manual, s_pll.pclk_div);
            }
        }
    } else if (strcmp(name, "gainceiling") == 0) {
        rc = s->set_gainceiling(s, (gainceiling_t)value);
    } else {
        setter_t fn = find_setter(s, name);
        if (fn == NULL) {
            err = ESP_ERR_NOT_FOUND;
        } else {
            rc = fn(s, value);
        }
    }
    unlock();
    if (err == ESP_OK && rc != 0) {
        err = ESP_FAIL;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set %s=%d: %s", name, value, esp_err_to_name(err));
    }
    return err;
}

esp_err_t esp32cam_set_reg(int reg, int mask, int value)
{
    if (!s_ready || !lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    sensor_t *s = esp_camera_sensor_get();
    int rc = s->set_reg ? s->set_reg(s, reg, mask, value) : -1;
    unlock();
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

int esp32cam_get_reg(int reg, int mask)
{
    if (!s_ready || !lock()) {
        return -1;
    }
    sensor_t *s = esp_camera_sensor_get();
    int v = s->get_reg ? s->get_reg(s, reg, mask) : -1;
    unlock();
    return v;
}

esp_err_t esp32cam_set_res_raw(const esp32cam_window_t *w)
{
    if (w == NULL || !s_ready || !lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    sensor_t *s = esp_camera_sensor_get();
    int rc = s->set_res_raw
                 ? s->set_res_raw(s, w->start_x, w->start_y, w->end_x, w->end_y, w->offset_x,
                                  w->offset_y, w->total_x, w->total_y, w->output_x, w->output_y,
                                  w->scale, w->binning)
                 : -1;
    unlock();
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t esp32cam_set_pll(const esp32cam_pll_t *p)
{
    if (!s_ready || !lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    sensor_t *s = esp_camera_sensor_get();
    int rc;
    if (p == NULL) {
        // Volver al PLL del driver: basta con re-aplicar la resolución actual.
        s_custom_pll = false;
        rc = s->set_framesize(s, s->status.framesize);
    } else {
        rc = s->set_pll ? s->set_pll(s, p->bypass, p->multiplier, p->sys_div, p->root_2x,
                                     p->pre_div, p->seld5, p->pclk_manual, p->pclk_div)
                        : -1;
        if (rc == 0) {
            s_pll = *p;
            s_custom_pll = true;
        }
    }
    unlock();
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t esp32cam_set_xclk_drive(int cap)
{
    if (cap < 0 || cap > 3) {
        return ESP_ERR_INVALID_ARG;
    }
    // 0 = ~5 mA (flancos lentos, menos armónicos en 2,4 GHz) ... 3 = ~40 mA.
    return gpio_set_drive_capability((gpio_num_t)PIN_XCLK, (gpio_drive_cap_t)cap);
}

int esp32cam_get_xclk_drive(void)
{
    gpio_drive_cap_t cap;
    return gpio_get_drive_capability((gpio_num_t)PIN_XCLK, &cap) == ESP_OK ? (int)cap : -1;
}

esp_err_t esp32cam_flash(uint8_t level)
{
    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, FLASH_CHANNEL, level);
    return err == ESP_OK ? ledc_update_duty(LEDC_LOW_SPEED_MODE, FLASH_CHANNEL) : err;
}

int esp32cam_status_json(char *buf, size_t size)
{
    sensor_t *s = esp_camera_sensor_get();
    if (s == NULL || !s_ready) {
        int n = snprintf(buf, size, "{\"sensor\":\"?\",\"ready\":false}");
        return (n < 0 || (size_t)n >= size) ? -1 : n;
    }
    const camera_status_t *st = &s->status;
    int w = 0, h = 0;
    esp32cam_framesize_dims(st->framesize, &w, &h);
    int n = snprintf(buf, size,
                     "{\"sensor\":\"%s\",\"ready\":true,\"framesize\":\"%s\",\"width\":%d,"
                     "\"height\":%d,\"quality\":%u,\"brightness\":%d,\"contrast\":%d,"
                     "\"saturation\":%d,\"sharpness\":%d,\"special_effect\":%u,\"wb_mode\":%u,"
                     "\"awb\":%u,\"aec\":%u,\"ae_level\":%d,\"agc\":%u,\"gainceiling\":%u,"
                     "\"hmirror\":%u,\"vflip\":%u,\"xclk_khz\":%d,\"max_framesize\":\"%s\"}",
                     esp32cam_sensor_name(), esp32cam_framesize_name(st->framesize), w, h,
                     st->quality, st->brightness, st->contrast, st->saturation, st->sharpness,
                     st->special_effect, st->wb_mode, st->awb, st->aec, st->ae_level, st->agc,
                     st->gainceiling, st->hmirror, st->vflip, s->xclk_freq_hz / 1000,
                     esp32cam_framesize_name(esp32cam_max_framesize()));
    return (n < 0 || (size_t)n >= size) ? -1 : n;
}
