/**
 * @file net_wifi.h
 * @brief Conexión WiFi en modo estación (STA) con reconexión automática.
 *
 * Idea principal: el WiFi se arranca UNA vez y la librería se encarga sola de
 * reconectar para siempre si el router se cae, se reinicia o la señal se pierde.
 * El resto del programa solo pregunta "¿hay red?" con net_wifi_is_connected()
 * o espera a que la haya con net_wifi_wait_connected().
 *
 * Estrategia de reconexión: espera exponencial (1 s, 2 s, 4 s ... hasta un
 * máximo) para no saturar al router, y nunca se rinde.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Configuración de la conexión WiFi. */
typedef struct {
    const char *ssid;          /*!< Nombre de la red (máx. 32 caracteres). */
    const char *password;      /*!< Contraseña (máx. 64). Cadena vacía = red abierta. */
    const char *ssid2;         /*!< Segunda red opcional (NULL o "" = solo una). Con dos, antes de
                                    cada intento se escanea y se usa la conocida con mejor señal. */
    const char *password2;     /*!< Contraseña de la segunda red. */
    const char *hostname;      /*!< Nombre del equipo en la red (opcional, NULL = por defecto). */
    uint32_t backoff_max_ms;   /*!< Espera máxima entre reintentos (0 = 30000 ms). */
} net_wifi_config_t;

/**
 * @brief Inicializa la pila de red y empieza a conectar (no bloquea).
 *
 * Requiere que NVS ya esté inicializado (nvs_flash_init) porque el driver WiFi
 * guarda calibración en él. Solo debe llamarse una vez.
 *
 * @return ESP_OK, ESP_ERR_INVALID_ARG si falta el SSID,
 *         ESP_ERR_INVALID_STATE si ya estaba iniciado, u otro error del driver.
 */
esp_err_t net_wifi_start(const net_wifi_config_t *config);

/** @brief true si hay conexión con IP asignada. Se puede llamar desde cualquier tarea. */
bool net_wifi_is_connected(void);

/**
 * @brief Bloquea hasta tener IP o hasta que pase el tiempo indicado.
 * @param timeout_ms Tiempo máximo de espera (UINT32_MAX = esperar para siempre).
 * @return true si hay conexión al salir.
 */
bool net_wifi_wait_connected(uint32_t timeout_ms);

/** @brief Intensidad de señal en dBm (p. ej. -55). Devuelve 0 si no hay conexión. */
int8_t net_wifi_get_rssi(void);

/**
 * @brief Milisegundos que lleva el WiFi sin conexión (0 si está conectado).
 * Útil para que la aplicación decida un reinicio de último recurso.
 */
uint32_t net_wifi_get_disconnected_ms(void);

#ifdef __cplusplus
}
#endif
