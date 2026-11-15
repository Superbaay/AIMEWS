/**
 * @file esp32cam.h
 * @brief Cámara de la placa AI-Thinker ESP32-CAM (sensor OV3660 / OV2640 / OV5640).
 *
 * Envoltorio fino sobre el driver oficial espressif/esp32-camera que:
 *  - Ya trae el patillaje de la AI-Thinker (no hay que pasar 16 pines).
 *  - Permite cambiar cualquier ajuste por NOMBRE ("framesize", "quality",
 *    "vflip"...), que es lo que llega por la red en un JSON.
 *  - Controla el LED de flash (GPIO 4) con PWM para regular la intensidad.
 *  - Lleva un mutex: varias tareas pueden capturar y ajustar a la vez.
 *
 * Los frames se guardan en PSRAM (la AI-Thinker trae 4/8 MB); sin PSRAM solo
 * funcionarían resoluciones muy pequeñas.
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

/**
 * XCLK por defecto: 19 MHz y no los 20 habituales. Medido en esta placa: a
 * 20 MHz (reloj exacto 80/4) el WiFi pierde 30-40 % de los paquetes mientras la
 * cámara funciona; a 19 MHz (divisor fraccionario del LEDC) pierde 0 %.
 */
#define ESP32CAM_DEFAULT_XCLK_HZ 19000000

/** Intentos de esp_camera_init() antes de rendirse (el sensor a veces no
 *  contesta justo después de un reset por el puerto serie). */
#define ESP32CAM_INIT_ATTEMPTS 3

/** Configuración inicial. Los campos a 0 toman un valor por defecto. */
typedef struct {
    framesize_t frame_size;   /*!< Resolución inicial (0 = FRAMESIZE_96X96; usa p. ej. FRAMESIZE_VGA). */
    int jpeg_quality;         /*!< 0-63, menor = mejor calidad y más bytes (0 = 12). */
    int fb_count;             /*!< Buffers de frame en PSRAM (0 = 2). Con 2+ la captura
                                   y el envío se solapan. */
    int xclk_hz;              /*!< Reloj del sensor (0 = ESP32CAM_DEFAULT_XCLK_HZ). */
} esp32cam_config_t;

/** Inicializa sensor, buffers y LED de flash. Solo una vez (o tras esp32cam_deinit). */
esp_err_t esp32cam_init(const esp32cam_config_t *cfg);

/** Libera el driver (p. ej. para reiniciarlo con otro xclk). */
esp_err_t esp32cam_deinit(void);

/**
 * @brief Captura un frame JPEG. Hay que devolverlo SIEMPRE con esp32cam_release().
 * @return El frame, o NULL si el sensor no entregó nada a tiempo.
 */
camera_fb_t *esp32cam_capture(void);

/** Devuelve el buffer al driver para que lo vuelva a llenar. */
void esp32cam_release(camera_fb_t *fb);

/** Nombre del sensor detectado ("OV3660", "OV2640"...) o "?" si no hay cámara. */
const char *esp32cam_sensor_name(void);

/**
 * @brief Cambia un ajuste por nombre.
 *
 * Nombres admitidos: framesize (número o se usa esp32cam_set_framesize_name),
 * quality, brightness, contrast, saturation, sharpness, special_effect,
 * wb_mode, awb, awb_gain, aec, aec2, ae_level, aec_value, agc, agc_gain,
 * gainceiling, bpc, wpc, raw_gma, lenc, hmirror, vflip, dcw, colorbar.
 *
 * @return ESP_OK, ESP_ERR_NOT_FOUND si el nombre no existe,
 *         ESP_FAIL si el sensor rechazó el valor.
 */
esp_err_t esp32cam_set(const char *name, int value);

/** Cambia la resolución por nombre ("QVGA", "VGA", "UXGA"...). */
esp_err_t esp32cam_set_framesize_name(const char *name);

/** Nombre de una resolución (FRAMESIZE_VGA -> "VGA"), o "?" si no existe. */
const char *esp32cam_framesize_name(framesize_t fs);

/** Resolución en píxeles de una framesize (false si no existe). */
bool esp32cam_framesize_dims(framesize_t fs, int *w, int *h);

/** Mayor resolución que admite el sensor detectado. */
framesize_t esp32cam_max_framesize(void);

// ---------------------------------------------------------------------------
// Ajustes avanzados del sensor (para exprimir fps). El driver vuelve a
// escribir ventana y PLL en cada cambio de "framesize": aplícalos DESPUÉS.
// ---------------------------------------------------------------------------

/** Ventana del sensor (ver set_res_raw del driver). La duración de un frame es
 *  proporcional a total_x * total_y: una ventana más pequeña = más fps. */
typedef struct {
    int start_x, start_y, end_x, end_y;   /*!< Zona del sensor que se lee. */
    int offset_x, offset_y;               /*!< Márgenes internos del ISP. */
    int total_x, total_y;                 /*!< HTS / VTS: tamaño total con blanking. */
    int output_x, output_y;               /*!< Tamaño de la imagen entregada. */
    bool scale, binning;
} esp32cam_window_t;

/** Parámetros del PLL del sensor (ver set_pll del driver OV3660). */
typedef struct {
    int bypass, multiplier, sys_div, root_2x, pre_div, seld5, pclk_manual, pclk_div;
} esp32cam_pll_t;

esp_err_t esp32cam_set_res_raw(const esp32cam_window_t *w);
/** Fija un PLL propio que se mantiene al cambiar de resolución. NULL = volver al del driver. */
esp_err_t esp32cam_set_pll(const esp32cam_pll_t *p);
/** Escribe bits de un registro del sensor: reg = (reg & ~mask) | (value & mask). */
esp_err_t esp32cam_set_reg(int reg, int mask, int value);
/** Lee un registro del sensor (-1 si falla). */
int esp32cam_get_reg(int reg, int mask);

/**
 * Fuerza de salida del pin XCLK (GPIO0): 0 (~5 mA) .. 3 (~40 mA). Por defecto
 * el ESP32 usa 2. Menos fuerza = flancos más suaves = menos interferencia con
 * el WiFi, mientras el sensor siga recibiendo un reloj limpio.
 */
esp_err_t esp32cam_set_xclk_drive(int cap);
int esp32cam_get_xclk_drive(void);

/** LED de flash: 0 = apagado, 255 = máximo (calienta: no lo dejes al máximo mucho rato). */
esp_err_t esp32cam_flash(uint8_t level);

/**
 * @brief Escribe el estado actual como objeto JSON en `buf`.
 * @return Longitud escrita (sin el '\0'), o -1 si no cabe.
 */
int esp32cam_status_json(char *buf, size_t size);

#ifdef __cplusplus
}
#endif
