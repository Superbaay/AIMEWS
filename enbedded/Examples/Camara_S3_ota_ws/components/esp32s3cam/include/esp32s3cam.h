/**
 * @file esp32s3cam.h
 * @brief Cámara OV3660 en una ESP32-S3-WROOM-1 N16R8 (placa "ESP32-S3 CAM" tipo Freenove).
 *
 * Envoltorio sobre el driver oficial espressif/esp32-camera, hecho a partir de la
 * librería esp32cam de la AI-Thinker (Cloudcode/esp32cam-https-ws) y ampliado para
 * lo que permite el S3:
 *  - Patillaje de la placa ya puesto (menuconfig -> "esp32s3cam").
 *  - Ajustes por NOMBRE ("quality", "vflip"...): lo que llega por la red en un JSON.
 *  - MODOS AFINADOS: ventana del sensor medida en la placa para sacar más fps que
 *    los modos de serie, sin subir el PLL: VGA60 (640x480 a 60), XGA30, HD30 (36),
 *    HD45 (47) y FHD22 (de serie: 27,8 fps hasta XGA y 17,4 en HD/FHD).
 *  - fps NOMINALES leídos del propio sensor: SYSCLK / (HTS x VTS).
 *  - Comprobación rápida de cada JPEG (marcadores SOI/EOI) y recorte del relleno.
 *  - Mutex: varias tareas pueden capturar y ajustar a la vez.
 *
 * Uso mínimo:
 * @code
 *   esp32s3cam_config_t cfg = { .frame_size = FRAMESIZE_HD, .jpeg_quality = 12 };
 *   ESP_ERROR_CHECK(esp32s3cam_init(&cfg));
 *   esp32s3cam_set_mode("HD30");          // 1280x720 a 30 fps
 *   camera_fb_t *fb = esp32s3cam_capture();
 *   ... fb->buf / fb->len ...
 *   esp32s3cam_release(fb);
 * @endcode
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_camera.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Intentos de esp_camera_init() antes de rendirse (tras flashear, a veces el
 *  sensor no contesta por SCCB en el primer intento). */
#define ESP32S3CAM_INIT_ATTEMPTS 3

/** Configuración inicial. Los campos a 0 toman un valor por defecto. */
typedef struct {
    framesize_t frame_size;   /*!< Resolución inicial (0 = FRAMESIZE_96X96; usa p. ej. FRAMESIZE_HD). */
    int jpeg_quality;         /*!< 4-63, menor = mejor calidad y más bytes (0 = 12). */
    int fb_count;             /*!< Buffers de frame en PSRAM (0 = 3). Con 2+ captura y envío se solapan. */
    int xclk_hz;              /*!< Reloj del sensor (0 = CONFIG_ESP32S3CAM_XCLK_KHZ). */
    size_t jpeg_buffer_size;  /*!< Tamaño de cada buffer (0 = automático para la resolución máxima). */
} esp32s3cam_config_t;

/** Inicializa sensor y buffers. Solo una vez (o tras esp32s3cam_deinit). */
esp_err_t esp32s3cam_init(const esp32s3cam_config_t *cfg);

/** Libera el driver. */
esp_err_t esp32s3cam_deinit(void);

/**
 * @brief Captura un frame JPEG. Hay que devolverlo SIEMPRE con esp32s3cam_release().
 * @return El frame, o NULL si el sensor no entregó nada a tiempo.
 */
camera_fb_t *esp32s3cam_capture(void);

/** Devuelve el buffer al driver para que lo vuelva a llenar. */
void esp32s3cam_release(camera_fb_t *fb);

/** Nombre del sensor detectado ("OV3660"...) o "?" si no hay cámara. */
const char *esp32s3cam_sensor_name(void);

/**
 * @brief Cambia un ajuste por nombre.
 *
 * Nombres: framesize, quality, brightness, contrast, saturation, sharpness, denoise,
 * special_effect, wb_mode, awb, awb_gain, aec, aec2, ae_level, aec_value, agc,
 * agc_gain, gainceiling, bpc, wpc, raw_gma, lenc, hmirror, vflip, dcw, colorbar.
 *
 * Cambiar "framesize" deja el modo afinado (vuelve a la ventana y PLL de serie).
 *
 * @return ESP_OK, ESP_ERR_NOT_FOUND si el nombre no existe,
 *         ESP_FAIL si el sensor rechazó el valor.
 */
esp_err_t esp32s3cam_set(const char *name, int value);

/** Cambia la resolución por nombre ("QVGA", "VGA", "HD"...). */
esp_err_t esp32s3cam_set_framesize_name(const char *name);

/** Nombre de una resolución (FRAMESIZE_HD -> "HD"), o "?" si no existe. */
const char *esp32s3cam_framesize_name(framesize_t fs);

/** Resolución en píxeles de una framesize (false si no existe). */
bool esp32s3cam_framesize_dims(framesize_t fs, int *w, int *h);

/** Mayor resolución que admite el sensor detectado. */
framesize_t esp32s3cam_max_framesize(void);

// ---------------------------------------------------------------------------
// Modos afinados (ventana + PLL medidos en la placa)
// ---------------------------------------------------------------------------

/** Ventana del sensor (ver set_res_raw del driver). La duración de un frame es
 *  proporcional a total_x * total_y: una ventana más pequeña = más fps. */
typedef struct {
    int start_x, start_y, end_x, end_y;   /*!< Zona del sensor que se lee. */
    int offset_x, offset_y;               /*!< Márgenes internos del ISP. */
    int total_x, total_y;                 /*!< HTS / VTS: tamaño total con blanking. */
    int output_x, output_y;               /*!< Tamaño de la imagen entregada. */
    bool scale, binning;
} esp32s3cam_window_t;

/** Parámetros del PLL del OV3660 (ver set_pll del driver). */
typedef struct {
    int bypass, multiplier, sys_div, root_2x, pre_div, seld5, pclk_manual, pclk_div;
} esp32s3cam_pll_t;

/** Un modo afinado: resolución de partida + ventana + PLL. */
typedef struct {
    const char *name;            /*!< "HD30", "SVGA60"... */
    const char *description;     /*!< Texto corto para humanos. */
    framesize_t frame_size;      /*!< Se aplica primero (buffers, ISP, nombre en el estado). */
    esp32s3cam_window_t window;  /*!< Ventana que se escribe encima. */
    bool has_pll;                /*!< false = PLL de serie del driver. */
    esp32s3cam_pll_t pll;
    int fps;                     /*!< fps esperados (nominales) con xclk por defecto. */
} esp32s3cam_mode_t;

/** Tabla de modos disponibles (count = número de entradas). */
const esp32s3cam_mode_t *esp32s3cam_modes(size_t *count);

/** Aplica un modo afinado por nombre (sin distinguir mayúsculas). */
esp_err_t esp32s3cam_set_mode(const char *name);

/** Nombre del modo afinado activo, o NULL si se usa un framesize de serie. */
const char *esp32s3cam_current_mode(void);

/**
 * @brief Elige el modo afinado que mejor cumple unos requisitos mínimos.
 * @param min_fps    fps mínimos que necesita la aplicación (0 = sin requisito).
 * @param min_width  ancho mínimo de la imagen en píxeles (0 = sin requisito).
 * @return El modo elegido, o NULL si ninguno cumple ambos requisitos.
 */
const esp32s3cam_mode_t *esp32s3cam_pick_mode(int min_fps, int min_width);

// ---------------------------------------------------------------------------
// Ajustes avanzados del sensor
// ---------------------------------------------------------------------------

esp_err_t esp32s3cam_set_res_raw(const esp32s3cam_window_t *w);
/** Fija un PLL propio que se mantiene al cambiar de resolución. NULL = volver al del driver. */
esp_err_t esp32s3cam_set_pll(const esp32s3cam_pll_t *p);
/** Escribe bits de un registro: reg = (reg & ~mask) | (value & mask). */
esp_err_t esp32s3cam_set_reg(int reg, int mask, int value);
/** Lee un registro del sensor (-1 si falla). */
int esp32s3cam_get_reg(int reg, int mask);

/** Temporización que el sensor tiene programada AHORA (leída por SCCB). */
typedef struct {
    int hts, vts;              /*!< Tamaño total de la ventana (con blanking). */
    int output_x, output_y;    /*!< Tamaño de salida. */
    uint32_t sysclk_hz;        /*!< Reloj interno calculado a partir del PLL y el XCLK. */
    uint32_t pclk_hz;          /*!< Reloj de píxel del bus DVP. */
    float fps;                 /*!< fps nominales = sysclk / (hts * vts). */
} esp32s3cam_timing_t;

esp_err_t esp32s3cam_get_timing(esp32s3cam_timing_t *t);

// ---------------------------------------------------------------------------
// Utilidades
// ---------------------------------------------------------------------------

/**
 * @brief Comprueba que el frame es un JPEG completo y recorta el relleno final.
 *
 * Mira SOI (FFD8) al principio y busca el EOI (FFD9) en los últimos bytes; si lo
 * encuentra, deja fb->len justo después del EOI. No decodifica: un JPEG con bandas
 * desplazadas puede pasar esta prueba (ver tools/verificar.py para eso).
 */
bool esp32s3cam_jpeg_ok(camera_fb_t *fb);

/**
 * @brief Escribe el estado actual como objeto JSON en `buf`.
 * @return Longitud escrita (sin el '\0'), o -1 si no cabe.
 */
int esp32s3cam_status_json(char *buf, size_t size);

#ifdef __cplusplus
}
#endif
