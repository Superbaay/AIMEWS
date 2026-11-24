/*
 * ota_camara — actualización del firmware por WiFi (OTA) desde el panel SAMATI.
 * Misma lógica que ota_dron.c de Dron_telemetria_ws (probada allí).
 *
 * La API (API/Routers_BM/ota_rt.py) manda por el WebSocket de control (/ws/machine):
 *   {"tipo":"comando","action":"ota","url":"/ota/firmware/3","version":"0.3.0","size":1070384}
 * y la cámara (por el WebSocket de control):
 *   1. Descarga https://<api>/ota/firmware/3 con su token de máquina.
 *   2. Lo escribe en la partición OTA que NO está corriendo (ota_0 <-> ota_1).
 *   3. Avisa el avance al panel: {"tipo":"ota","estado":"descargando","porcentaje":40,...}
 *   4. Si todo cuadra (imagen válida, misma versión que anunció la API), reinicia.
 *
 * Seguridad (rollback): la versión nueva arranca "pendiente de verificar". Si en
 * OTA_TIEMPO_VERIFICAR_S no logra conectarse a la API (auth_ok), la cámara reinicia
 * y el bootloader vuelve SOLO a la versión anterior. Un firmware roto nunca
 * deja a la cámara sin poder recibir otra OTA.
 */
#pragma once

#include <stdbool.h>

/* Llamar al arrancar: si esta versión acaba de llegar por OTA, arma el
 * temporizador que hace rollback si no se confirma a tiempo. */
void ota_revisar_arranque(void);

/* Llamar cuando la API acepta el token (auth_ok): confirma la versión nueva
 * y cancela el rollback. No hace nada si no venía de una OTA. */
void ota_confirmar(void);

/* Empieza la descarga en una tarea propia (no bloquea). false si ya hay una
 * OTA en curso o los datos no son válidos. */
bool ota_iniciar(const char *ruta, const char *version, int tam);

/* true mientras descarga/escribe. */
bool ota_en_curso(void);

/* Versión de este firmware (PROJECT_VER del CMakeLists, viaja dentro del .bin). */
const char *ota_version_actual(void);
