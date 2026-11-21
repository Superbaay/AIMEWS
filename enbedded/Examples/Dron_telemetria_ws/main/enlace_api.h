/*
 * enlace_api — WiFi + token de máquina + WebSocket con la API SAMATI.
 *
 * Protocolo (router API/Routers_BM/drone_ws_rt.py):
 *   1. POST /machine_rt/token_machine            -> {"token": "..."}
 *   2. ws(s)://<api>/ws/machine, primer mensaje   {"token": "..."}
 *   3. La API responde {"tipo": "auth_ok", ...}; el dron manda su estado_dron
 *      y desde entonces telemetría N veces por segundo.
 *   4. Comandos del panel (los reenvía main.py):
 *        {"tipo": "comando", "action": "iniciar_autoprueba"}  -> responde "autoprueba"
 *        {"tipo": "comando", "action": "cero_altura"}         -> el barómetro toma altura 0
 *        {"tipo": "comando", "action": "ota", "url", "version", "size"} -> ota_dron.c
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Arranca WiFi, pide token, abre el WebSocket y la tarea de telemetría.
 * No bloquea: si la red falla, reintenta sola en segundo plano. */
void enlace_api_iniciar(void);

/* true si la API ya aceptó el token (auth_ok). */
bool enlace_api_conectado(void);

/* Manda un texto por el WebSocket si está conectado (lo usa ota_dron.c). */
void enlace_api_enviar(const char *texto, int largo);

/* Token de máquina vigente (para la cabecera Authorization de la descarga OTA). */
const char *enlace_api_token(void);

/* "https://<host>:<puerto>" + ruta. Devuelve la longitud o -1 si no cabe. */
int enlace_api_url(char *salida, size_t tam, const char *ruta);
