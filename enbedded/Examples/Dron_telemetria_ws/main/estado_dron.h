/*
 * estado_dron — lo último que midió cada sensor, compartido entre tareas.
 *
 * Cada tarea de sensor escribe SOLO su parte y la tarea de telemetría copia
 * todo de una vez. Como corren en núcleos distintos (IMU en el 1, lo demás en
 * el 0) se protege con un spinlock: las secciones críticas solo copian
 * números, nunca hacen printf ni I2C dentro.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool listo;                 // init correcto y calibrado
    float cabeceo, alabeo, guinada;            // grados (pitch, roll, yaw)
    float vel_cabeceo, vel_alabeo, vel_guinada; // grados/s
    float ax, ay, az;           // g
    float temp_c;
    float hz;                   // muestras leídas en el último segundo
    uint32_t perdidas;          // interrupciones no atendidas (último segundo)
    uint32_t errores_i2c;       // acumulado
    uint32_t lectura_max_us;    // peor lectura I2C del último segundo
    uint32_t lectura_media_us;  // lectura I2C media del último segundo
} estado_imu_t;

typedef struct {
    bool listo;                 // init correcto y referencia del suelo tomada
    float presion_hpa;
    float temp_c;
    float altura_m;             // relativa al punto de arranque
    float hz;
    uint32_t errores_i2c;
} estado_barometro_t;

typedef struct {
    bool listo;                 // el GPS responde y llegan medidas
    bool fix;                   // posición válida
    uint8_t tipo_fix;           // 1 sin fix, 2 = 2D, 3 = 3D
    double latitud, longitud;
    float altitud_m;            // sobre el nivel del mar
    float velocidad_ms;
    float rumbo_deg;
    float hdop;
    uint8_t satelites_usados, satelites_vista;
    float hz;
    uint32_t frases_malas;
} estado_gps_t;

typedef struct {
    estado_imu_t imu;
    estado_barometro_t baro;
    estado_gps_t gps;
} estado_dron_t;

/* Copia atómica de todo el estado. */
void estado_leer(estado_dron_t *copia);

/* Escrituras de cada tarea (copian el bloque completo de su sensor). */
void estado_escribir_imu(const estado_imu_t *imu);
void estado_escribir_baro(const estado_barometro_t *baro);
void estado_escribir_gps(const estado_gps_t *gps);

/* Pide al barómetro que tome la presión actual como nueva altura 0
 * (comando "cero_altura" del panel). */
void estado_pedir_cero_altura(void);
bool estado_tomar_peticion_cero_altura(void);
