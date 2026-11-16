#include "estado_dron.h"

#include "freertos/FreeRTOS.h"

static portMUX_TYPE candado = portMUX_INITIALIZER_UNLOCKED;
static estado_dron_t estado;
static volatile bool peticion_cero_altura;

void estado_leer(estado_dron_t *copia)
{
    taskENTER_CRITICAL(&candado);
    *copia = estado;
    taskEXIT_CRITICAL(&candado);
}

void estado_escribir_imu(const estado_imu_t *imu)
{
    taskENTER_CRITICAL(&candado);
    estado.imu = *imu;
    taskEXIT_CRITICAL(&candado);
}

void estado_escribir_baro(const estado_barometro_t *baro)
{
    taskENTER_CRITICAL(&candado);
    estado.baro = *baro;
    taskEXIT_CRITICAL(&candado);
}

void estado_escribir_gps(const estado_gps_t *gps)
{
    taskENTER_CRITICAL(&candado);
    estado.gps = *gps;
    taskEXIT_CRITICAL(&candado);
}

void estado_pedir_cero_altura(void)
{
    peticion_cero_altura = true;
}

bool estado_tomar_peticion_cero_altura(void)
{
    if (!peticion_cero_altura) return false;
    peticion_cero_altura = false;
    return true;
}
