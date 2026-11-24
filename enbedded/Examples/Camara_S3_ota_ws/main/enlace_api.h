/*
 * enlace_api — WiFi (casa u hotspot) + token de máquina + DOS WebSockets con la API.
 *
 *   Vídeo   ws(s)://<api>/camera_rt/ws/machine   (API/Routers_BM/camera_rt.py)
 *     1er mensaje {"token": ...} -> {"type": "auth_ok", "fps": 12, "max_frame_bytes": ...}
 *     La cámara manda cada JPEG como mensaje BINARIO y su estado como texto.
 *     La API le reenvía del panel: {"type":"set","name":"quality","value":20}.
 *
 *   Control ws(s)://<api>/ws/machine             (API/Routers_BM/drone_ws_rt.py)
 *     1er mensaje {"token": ...} -> {"tipo": "auth_ok"}
 *     La cámara manda estado_dron con "acciones": ["ota"] y su versión. Por aquí
 *     llega {"tipo":"comando","action":"ota",...} (POST /ota/deploy y
 *     enbedded/tools/ota_subir.py) y salen los avisos {"tipo":"ota",...}.
 *
 * Por qué dos: la OTA de la API (ota_rt.py) solo sabe hablar con las máquinas
 * conectadas al router del dron. Así la cámara usa la OTA tal cual, sin cambiar
 * la API. En el panel aparece en la lista de drones, solo con la acción "ota".
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define NUCLEO_SISTEMA   0   // WiFi, TLS, WebSockets, OTA
#define PRIORIDAD_ENLACE 5

/* Arranca WiFi, pide token y abre los dos WebSockets. No bloquea: si la red o la
 * API fallan, reintenta sola en segundo plano. */
void enlace_api_iniciar(void);

/* true si camera_rt aceptó el token: ya se puede mandar vídeo. */
bool enlace_api_video_listo(void);

/* true si el router del dron aceptó el token (canal de control / OTA). */
bool enlace_api_control_listo(void);

/* Manda un JPEG por el WebSocket de vídeo. ESP_ERR_INVALID_STATE si no está listo. */
esp_err_t enlace_api_enviar_frame(const uint8_t *jpeg, size_t largo);

/* Manda el estado de la cámara (framesize, calidad...) a los visores del panel. */
void enlace_api_enviar_estado(void);

/* true si alguien cambió framesize o calidad desde el panel: main.c deja de
 * adaptar la imagen solo (manda el ajuste manual). */
bool enlace_api_ajuste_manual(void);

/* Manda un texto por el WebSocket de control (avisos de OTA). */
void enlace_api_enviar_control(const char *texto, int largo);

/* Token de máquina vigente (cabecera Authorization de la descarga OTA). */
const char *enlace_api_token(void);

/* "https://<host>:<puerto>" + ruta. Devuelve la longitud o -1 si no cabe. */
int enlace_api_url(char *salida, size_t tam, const char *ruta);
