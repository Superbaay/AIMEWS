/**
 * @file esp32s3cam.c
 * @brief Librería de cámara OV3660 para la ESP32-S3-WROOM-1 N16R8 CAM.
 */
#include "esp32s3cam.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp32s3cam_modes.h"

static const char *TAG = "esp32s3cam";

// ---------------------------------------------------------------------------
// Patillaje (menuconfig -> "esp32s3cam"). Por defecto el de la placa Freenove /
// ESP32-S3-EYE, VERIFICADO en la placa: OV3660 responde en 0x3C con SDA=4 y SCL=5.
// ---------------------------------------------------------------------------
#define PIN_PWDN   CONFIG_ESP32S3CAM_PIN_PWDN
#define PIN_RESET  CONFIG_ESP32S3CAM_PIN_RESET
#define PIN_XCLK   CONFIG_ESP32S3CAM_PIN_XCLK
#define PIN_SIOD   CONFIG_ESP32S3CAM_PIN_SIOD
#define PIN_SIOC   CONFIG_ESP32S3CAM_PIN_SIOC
#define PIN_D7     CONFIG_ESP32S3CAM_PIN_D7
#define PIN_D6     CONFIG_ESP32S3CAM_PIN_D6
#define PIN_D5     CONFIG_ESP32S3CAM_PIN_D5
#define PIN_D4     CONFIG_ESP32S3CAM_PIN_D4
#define PIN_D3     CONFIG_ESP32S3CAM_PIN_D3
#define PIN_D2     CONFIG_ESP32S3CAM_PIN_D2
#define PIN_D1     CONFIG_ESP32S3CAM_PIN_D1
#define PIN_D0     CONFIG_ESP32S3CAM_PIN_D0
#define PIN_VSYNC  CONFIG_ESP32S3CAM_PIN_VSYNC
#define PIN_HREF   CONFIG_ESP32S3CAM_PIN_HREF
#define PIN_PCLK   CONFIG_ESP32S3CAM_PIN_PCLK

// Registros del OV3660 que se leen para calcular los fps (ov3660_regs.h)
#define REG_PLL_CTRL0   0x303a   // bit 7: bypass
#define REG_PLL_CTRL1   0x303b   // multiplicador [4:0]
#define REG_PLL_CTRL2   0x303c   // divisor de sistema [3:0]
#define REG_PLL_CTRL3   0x303d   // root_2x bit 6, pre_div [5:4], seld5 [1:0]
#define REG_PCLK_RATIO  0x3824
#define REG_VFIFO_0C    0x460c   // bit 1: PCLK manual
#define REG_HTS_H       0x380c
#define REG_VTS_H       0x380e
#define REG_OUT_X_H     0x3808
#define REG_OUT_Y_H     0x380a

static SemaphoreHandle_t s_lock;
static bool s_ready;
// PLL propio: el driver reescribe el PLL en cada set_framesize, así que se
// guarda y se vuelve a aplicar tras cada cambio de resolución.
static bool s_custom_pll;
static esp32s3cam_pll_t s_pll;
static const esp32s3cam_mode_t *s_mode;   // modo afinado activo (NULL = de serie)

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

static int apply_pll_locked(sensor_t *s, const esp32s3cam_pll_t *p)
{
    return s->set_pll ? s->set_pll(s, p->bypass, p->multiplier, p->sys_div, p->root_2x, p->pre_div,
                                   p->seld5, p->pclk_manual, p->pclk_div)
                      : -1;
}

esp_err_t esp32s3cam_init(const esp32s3cam_config_t *cfg)
{
    if (s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    const esp32s3cam_config_t def = { 0 };
    if (cfg == NULL) {
        cfg = &def;
    }
    if (esp_psram_get_size() == 0) {
        // La N16R8 trae 8 MB de PSRAM octal: si no aparece, falta CONFIG_SPIRAM_MODE_OCT.
        ESP_LOGE(TAG, "Sin PSRAM: activa CONFIG_SPIRAM y CONFIG_SPIRAM_MODE_OCT");
        return ESP_ERR_NO_MEM;
    }

    camera_config_t cc = {
        .pin_pwdn = PIN_PWDN, .pin_reset = PIN_RESET, .pin_xclk = PIN_XCLK,
        .pin_sccb_sda = PIN_SIOD, .pin_sccb_scl = PIN_SIOC,
        .pin_d7 = PIN_D7, .pin_d6 = PIN_D6, .pin_d5 = PIN_D5, .pin_d4 = PIN_D4,
        .pin_d3 = PIN_D3, .pin_d2 = PIN_D2, .pin_d1 = PIN_D1, .pin_d0 = PIN_D0,
        .pin_vsync = PIN_VSYNC, .pin_href = PIN_HREF, .pin_pclk = PIN_PCLK,
        .xclk_freq_hz = cfg->xclk_hz > 0 ? cfg->xclk_hz : CONFIG_ESP32S3CAM_XCLK_KHZ * 1000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        // Se arranca en la resolución MÁXIMA para que el driver reserve buffers que
        // sirvan para todas (2048x1536/5 = 629 KB cada uno); luego se baja a la pedida.
        .frame_size = FRAMESIZE_QXGA,
        .jpeg_quality = cfg->jpeg_quality > 0 ? cfg->jpeg_quality : 12,
        .fb_count = cfg->fb_count > 0 ? (size_t)cfg->fb_count : 3,
        .fb_location = CAMERA_FB_IN_PSRAM,
        // Con 2+ buffers: siempre el más reciente (latencia mínima, se saltan los viejos).
        .grab_mode = CAMERA_GRAB_LATEST,
        .jpeg_buffer_size = cfg->jpeg_buffer_size,
    };

    // Justo después de flashear (reset por RTS) el sensor a veces no contesta por
    // SCCB ("probe failed"). El driver limpia tras un fallo: basta con esperar y repetir.
    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= ESP32S3CAM_INIT_ATTEMPTS; attempt++) {
        err = esp_camera_init(&cc);
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "esp_camera_init falló (intento %d/%d): %s", attempt,
                 ESP32S3CAM_INIT_ATTEMPTS, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(200 * attempt));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cámara no detectada tras %d intentos", ESP32S3CAM_INIT_ATTEMPTS);
        return err;
    }
    sensor_t *s = esp_camera_sensor_get();
    if (s == NULL) {
        esp_camera_deinit();
        return ESP_FAIL;
    }
#if CONFIG_ESP32S3CAM_VFLIP
    s->set_vflip(s, 1);
#endif
#if CONFIG_ESP32S3CAM_HMIRROR
    s->set_hmirror(s, 1);
#endif
    framesize_t fs = cfg->frame_size;
    if (fs >= FRAMESIZE_INVALID || fs > esp32s3cam_max_framesize()) {
        fs = FRAMESIZE_VGA;
    }
    s->set_framesize(s, fs);
    s_ready = true;
    ESP_LOGI(TAG, "Cámara %s lista (%s, xclk %d kHz, %u buffers)", esp32s3cam_sensor_name(),
             esp32s3cam_framesize_name(fs), cc.xclk_freq_hz / 1000, (unsigned)cc.fb_count);
    return ESP_OK;
}

esp_err_t esp32s3cam_deinit(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!lock()) {
        return ESP_ERR_TIMEOUT;
    }
    s_ready = false;
    s_custom_pll = false;
    s_mode = NULL;
    esp_err_t err = esp_camera_deinit();
    unlock();
    return err;
}

camera_fb_t *esp32s3cam_capture(void)
{
    // Sin mutex a propósito: esp_camera_fb_get ya es seguro entre tareas y así un
    // cambio de ajustes lento no frena el streaming.
    return s_ready ? esp_camera_fb_get() : NULL;
}

void esp32s3cam_release(camera_fb_t *fb)
{
    if (fb != NULL) {
        esp_camera_fb_return(fb);
    }
}

const char *esp32s3cam_sensor_name(void)
{
    sensor_t *s = esp_camera_sensor_get();
    if (s == NULL) {
        return "?";
    }
    camera_sensor_info_t *info = esp_camera_sensor_get_info(&s->id);
    return info != NULL ? info->name : "?";
}

framesize_t esp32s3cam_max_framesize(void)
{
    sensor_t *s = esp_camera_sensor_get();
    camera_sensor_info_t *info = s ? esp_camera_sensor_get_info(&s->id) : NULL;
    return info != NULL ? info->max_size : FRAMESIZE_UXGA;
}

const char *esp32s3cam_framesize_name(framesize_t fs)
{
    return (fs < FRAMESIZE_INVALID && FRAMESIZE_NAMES[fs]) ? FRAMESIZE_NAMES[fs] : "?";
}

bool esp32s3cam_framesize_dims(framesize_t fs, int *w, int *h)
{
    if (fs >= FRAMESIZE_INVALID) {
        return false;
    }
    if (w) *w = resolution[fs].width;
    if (h) *h = resolution[fs].height;
    return true;
}

esp_err_t esp32s3cam_set_framesize_name(const char *name)
{
    for (int i = 0; name != NULL && i < FRAMESIZE_INVALID; i++) {
        if (FRAMESIZE_NAMES[i] && strcasecmp(name, FRAMESIZE_NAMES[i]) == 0) {
            return esp32s3cam_set("framesize", i);
        }
    }
    return ESP_ERR_NOT_FOUND;
}

// ---------------------------------------------------------------------------
// Ajustes por nombre: tabla nombre -> función del sensor
// ---------------------------------------------------------------------------
typedef int (*setter_t)(sensor_t *, int);

/** Cada sensor rellena su propia tabla de punteros: se resuelve en ejecución. */
static setter_t find_setter(sensor_t *s, const char *name)
{
    const struct {
        const char *name;
        setter_t fn;
    } table[] = {
        {"quality", s->set_quality},       {"brightness", s->set_brightness},
        {"contrast", s->set_contrast},     {"saturation", s->set_saturation},
        {"sharpness", s->set_sharpness},   {"denoise", s->set_denoise},
        {"special_effect", s->set_special_effect}, {"wb_mode", s->set_wb_mode},
        {"awb", s->set_whitebal},          {"awb_gain", s->set_awb_gain},
        {"aec", s->set_exposure_ctrl},     {"aec2", s->set_aec2},
        {"ae_level", s->set_ae_level},     {"aec_value", s->set_aec_value},
        {"agc", s->set_gain_ctrl},         {"agc_gain", s->set_agc_gain},
        {"bpc", s->set_bpc},               {"wpc", s->set_wpc},
        {"raw_gma", s->set_raw_gma},       {"lenc", s->set_lenc},
        {"hmirror", s->set_hmirror},       {"vflip", s->set_vflip},
        {"dcw", s->set_dcw},               {"colorbar", s->set_colorbar},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strcmp(name, table[i].name) == 0) {
            return table[i].fn;
        }
    }
    return NULL;
}

esp_err_t esp32s3cam_set(const char *name, int value)
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
        if (value < 0 || value > (int)esp32s3cam_max_framesize()) {
            err = ESP_ERR_INVALID_ARG;
        } else {
            rc = s->set_framesize(s, (framesize_t)value);
            s_mode = NULL;   // el driver acaba de reescribir la ventana de serie
            if (rc == 0 && s_custom_pll) {
                rc = apply_pll_locked(s, &s_pll);
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

// ---------------------------------------------------------------------------
// Modos afinados
// ---------------------------------------------------------------------------
const esp32s3cam_mode_t *esp32s3cam_modes(size_t *count)
{
    if (count) {
        *count = esp32s3cam_mode_count;
    }
    return esp32s3cam_mode_table;
}

const char *esp32s3cam_current_mode(void)
{
    return s_mode ? s_mode->name : NULL;
}

/** Fracción del campo de visión horizontal del sensor que cubre un modo (0..1).
 *  Con binning se lee el doble de la salida; en recorte 1:1, la salida tal cual. */
static float mode_fov(const esp32s3cam_mode_t *m)
{
    return (float)(m->window.end_x - m->window.start_x + 1) / 2080.0f;
}

const esp32s3cam_mode_t *esp32s3cam_pick_mode(int min_fps, int min_width)
{
    // Datos de cada modo m = &esp32s3cam_mode_table[i] (i < esp32s3cam_mode_count):
    //   m->fps (medidos), m->window.output_x / output_y (píxeles), mode_fov(m) (campo).
    // Pendiente en la librería original (Cloudcode/esp32s3-cam-ov3660): elegir el modo
    // que cumple fps >= min_fps y ancho >= min_width. Esta copia aún devuelve NULL.
    (void)min_fps;
    (void)min_width;
    (void)mode_fov;
    return NULL;
}

static int apply_window_locked(sensor_t *s, const esp32s3cam_window_t *w)
{
    return s->set_res_raw ? s->set_res_raw(s, w->start_x, w->start_y, w->end_x, w->end_y,
                                           w->offset_x, w->offset_y, w->total_x, w->total_y,
                                           w->output_x, w->output_y, w->scale, w->binning)
                          : -1;
}

esp_err_t esp32s3cam_set_mode(const char *name)
{
    const esp32s3cam_mode_t *m = NULL;
    for (size_t i = 0; name != NULL && i < esp32s3cam_mode_count; i++) {
        if (strcasecmp(name, esp32s3cam_mode_table[i].name) == 0) {
            m = &esp32s3cam_mode_table[i];
        }
    }
    if (m == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!s_ready || !lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    sensor_t *s = esp_camera_sensor_get();
    // Orden: framesize de serie (prepara ISP y escala) -> ventana -> PLL.
    // El driver escribe su PLL en set_framesize: el del modo tiene que ir DESPUÉS.
    int rc = s->set_framesize(s, m->frame_size);
    if (rc == 0) {
        rc = apply_window_locked(s, &m->window);
    }
    if (rc == 0 && m->has_pll) {
        rc = apply_pll_locked(s, &m->pll);
    } else if (rc == 0 && s_custom_pll) {
        rc = apply_pll_locked(s, &s_pll);
    }
    s_mode = rc == 0 ? m : NULL;
    unlock();
    if (rc != 0) {
        ESP_LOGW(TAG, "No se pudo aplicar el modo %s", m->name);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Modo %s: %s", m->name, m->description);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Ajustes avanzados
// ---------------------------------------------------------------------------
esp_err_t esp32s3cam_set_reg(int reg, int mask, int value)
{
    if (!s_ready || !lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    sensor_t *s = esp_camera_sensor_get();
    int rc = s->set_reg ? s->set_reg(s, reg, mask, value) : -1;
    unlock();
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

int esp32s3cam_get_reg(int reg, int mask)
{
    if (!s_ready || !lock()) {
        return -1;
    }
    sensor_t *s = esp_camera_sensor_get();
    int v = s->get_reg ? s->get_reg(s, reg, mask) : -1;
    unlock();
    return v;
}

esp_err_t esp32s3cam_set_res_raw(const esp32s3cam_window_t *w)
{
    if (w == NULL || !s_ready || !lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    int rc = apply_window_locked(esp_camera_sensor_get(), w);
    s_mode = NULL;
    unlock();
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t esp32s3cam_set_pll(const esp32s3cam_pll_t *p)
{
    if (!s_ready || !lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    sensor_t *s = esp_camera_sensor_get();
    int rc;
    if (p == NULL) {
        // Volver al PLL del driver: basta con re-aplicar la resolución actual.
        s_custom_pll = false;
        s_mode = NULL;
        rc = s->set_framesize(s, s->status.framesize);
    } else {
        rc = apply_pll_locked(s, p);
        if (rc == 0) {
            s_pll = *p;
            s_custom_pll = true;
        }
    }
    unlock();
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

/** Lee un par de registros de 8 bits como un número (alto, bajo). */
static int get_reg16(sensor_t *s, int reg_h, int mask_h)
{
    int h = s->get_reg(s, reg_h, mask_h);
    int l = s->get_reg(s, reg_h + 1, 0xff);
    return (h < 0 || l < 0) ? -1 : (h << 8) | l;
}

esp_err_t esp32s3cam_get_timing(esp32s3cam_timing_t *t)
{
    if (t == NULL || !s_ready || !lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    sensor_t *s = esp_camera_sensor_get();
    memset(t, 0, sizeof(*t));
    if (s->get_reg == NULL) {
        unlock();
        return ESP_ERR_NOT_SUPPORTED;
    }
    t->hts = get_reg16(s, REG_HTS_H, 0x0f);
    t->vts = get_reg16(s, REG_VTS_H, 0xff);
    t->output_x = get_reg16(s, REG_OUT_X_H, 0x0f);
    t->output_y = get_reg16(s, REG_OUT_Y_H, 0x07);
    int c0 = s->get_reg(s, REG_PLL_CTRL0, 0xff);
    int mult = s->get_reg(s, REG_PLL_CTRL1, 0x1f);
    int sys_div = s->get_reg(s, REG_PLL_CTRL2, 0x0f);
    int c3 = s->get_reg(s, REG_PLL_CTRL3, 0xff);
    int pclk_ratio = s->get_reg(s, REG_PCLK_RATIO, 0x1f);
    int vfifo = s->get_reg(s, REG_VFIFO_0C, 0xff);
    unlock();
    if (t->hts <= 0 || t->vts <= 0 || c0 < 0 || mult < 0 || sys_div < 0 || c3 < 0) {
        return ESP_FAIL;
    }
    // Misma fórmula que calc_sysclk() del driver (ov3660.c), en Hz y sin redondeos raros:
    //   VCO = XCLK * mult * root / pre_div    PLLCLK = VCO / sys_div / seld5
    //   SYSCLK = PLLCLK / 4                   PCLK = PLLCLK / 2 / pclk_div
    static const int pre_div2x[] = { 2, 3, 4, 6 };   // x2 para evitar decimales
    static const int seld5_2x[] = { 2, 2, 4, 5 };
    uint64_t xclk = (uint64_t)s->xclk_freq_hz;
    uint64_t pllclk;
    if (c0 & 0x80) {
        pllclk = xclk;   // bypass
    } else {
        int root = (c3 & 0x40) ? 2 : 1;
        uint64_t vco = xclk * (uint64_t)mult * root * 2 / pre_div2x[(c3 >> 4) & 3];
        pllclk = vco * 2 / (sys_div ? sys_div : 1) / seld5_2x[c3 & 3];
    }
    t->sysclk_hz = (uint32_t)(pllclk / 4);
    bool pclk_manual = (vfifo & 0x02) != 0;
    t->pclk_hz = (uint32_t)(pllclk / 2 / ((pclk_manual && pclk_ratio) ? pclk_ratio : 1));
    t->fps = (float)t->sysclk_hz / ((float)t->hts * (float)t->vts);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Utilidades
// ---------------------------------------------------------------------------
bool esp32s3cam_jpeg_ok(camera_fb_t *fb)
{
    if (fb == NULL || fb->len < 4 || fb->buf[0] != 0xFF || fb->buf[1] != 0xD8) {
        return false;
    }
    // El driver ya corta en el EOI, pero puede quedar relleno del último bloque DMA:
    // se busca hacia atrás en los últimos 64 bytes.
    size_t from = fb->len > 64 ? fb->len - 64 : 2;
    for (size_t i = fb->len - 1; i > from; i--) {
        if (fb->buf[i - 1] == 0xFF && fb->buf[i] == 0xD9) {
            fb->len = i + 1;
            return true;
        }
    }
    return false;
}

int esp32s3cam_status_json(char *buf, size_t size)
{
    sensor_t *s = esp_camera_sensor_get();
    if (s == NULL || !s_ready) {
        int n = snprintf(buf, size, "{\"sensor\":\"?\",\"ready\":false}");
        return (n < 0 || (size_t)n >= size) ? -1 : n;
    }
    const camera_status_t *st = &s->status;
    esp32s3cam_timing_t t = { 0 };
    esp32s3cam_get_timing(&t);
    int w = t.output_x, h = t.output_y;
    if (w <= 0 || h <= 0) {
        esp32s3cam_framesize_dims(st->framesize, &w, &h);
    }
    const char *mode = esp32s3cam_current_mode();
    int n = snprintf(buf, size,
                     "{\"sensor\":\"%s\",\"ready\":true,\"framesize\":\"%s\",\"mode\":\"%s\","
                     "\"width\":%d,\"height\":%d,\"quality\":%u,\"brightness\":%d,\"contrast\":%d,"
                     "\"saturation\":%d,\"sharpness\":%d,\"special_effect\":%u,\"wb_mode\":%u,"
                     "\"awb\":%u,\"aec\":%u,\"ae_level\":%d,\"agc\":%u,\"gainceiling\":%u,"
                     "\"hmirror\":%u,\"vflip\":%u,\"xclk_khz\":%d,\"hts\":%d,\"vts\":%d,"
                     "\"sysclk_khz\":%u,\"pclk_khz\":%u,\"fps_nominal\":%.1f,\"max_framesize\":\"%s\"}",
                     esp32s3cam_sensor_name(), esp32s3cam_framesize_name(st->framesize),
                     mode ? mode : "", w, h, st->quality, st->brightness, st->contrast,
                     st->saturation, st->sharpness, st->special_effect, st->wb_mode, st->awb,
                     st->aec, st->ae_level, st->agc, st->gainceiling, st->hmirror, st->vflip,
                     s->xclk_freq_hz / 1000, t.hts, t.vts, (unsigned)(t.sysclk_hz / 1000),
                     (unsigned)(t.pclk_hz / 1000), t.fps,
                     esp32s3cam_framesize_name(esp32s3cam_max_framesize()));
    return (n < 0 || (size_t)n >= size) ? -1 : n;
}
