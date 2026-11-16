/*
 * sensores — tareas de cada sensor. app_main crea los drivers (comparten el
 * bus I2C) y luego arranca estas tareas pasando el handle como argumento.
 */
#pragma once

#include "freertos/FreeRTOS.h"

// Núcleos: el 1 queda solo para el lazo de 1 kHz; WiFi, TLS, GPS y barómetro en el 0.
#define NUCLEO_VUELO        1
#define NUCLEO_SISTEMA      0

#define PRIORIDAD_IMU       (configMAX_PRIORITIES - 2)  // casi la máxima
#define PRIORIDAD_GPS       4
// El barómetro comparte el candado del bus I2C con el IMU. Si una tarea de red
// (lwIP = 18) lo dejara sin CPU con el candado tomado, el IMU esperaría el bus
// y su FIFO se desbordaría (medido durante los handshakes TLS). Con 19 nadie de
// la red lo frena; casi no usa CPU (~0.3 ms de cada 10, el resto duerme).
#define PRIORIDAD_BAROMETRO 19
#define PRIORIDAD_ENLACE    2                            // telemetría: la más baja

void tarea_imu(void *mpu_handle);         // arg: mpu_handle_t
void tarea_barometro(void *ms5611_handle); // arg: ms5611_handle_t
void tarea_gps(void *arg);                // arg: NULL (el GPS se inicia dentro)
