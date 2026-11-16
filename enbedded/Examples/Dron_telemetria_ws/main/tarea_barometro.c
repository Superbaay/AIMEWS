/*
 * tarea_barometro — presión, temperatura y altura relativa (núcleo 0).
 *
 * Igual que el ejemplo Barometro_HW291 (verificado en la placa: ~89 Hz,
 * σ 10 cm, 0 errores I2C), pero compartiendo el bus I2C con el IMU.
 *
 * Por qué ~89 Hz es "la mayor frecuencia estable":
 *   - Con OSR 4096 cada conversión tarda hasta 9.04 ms (datasheet) y se
 *     espera 10-11 ms con el tick de 1 ms: unas 89-95 presiones por segundo.
 *     La temperatura solo se mide 1 de cada 20 veces.
 *   - Bajar el OSR da más Hz pero más ruido por muestra (OSR 256: ~1 kHz con
 *     6.5 Pa ≈ 55 cm); promediando para quitar ese ruido se acaba con la
 *     misma resolución por segundo y con 10 veces más tráfico en el bus que
 *     comparte con el IMU de 1 kHz. Cambia OSR_PRESION si quieres probarlo.
 */
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "estado_dron.h"
#include "ms5611.h"
#include "sensores.h"

static const char *TAG = "barometro";

#define OSR_PRESION             MS5611_OSR_4096
#define OSR_TEMPERATURA         MS5611_OSR_1024
#define CADA_N_TEMPERATURA      20
#define SEGUNDOS_CALENTAMIENTO  5     // el chip se calienta 1-2 °C al empezar
#define MUESTRAS_REFERENCIA     64
#define PERIODO_BLOQUE_US       100000 // se publica la media de cada 100 ms

// Lanzar la conversión -> dormir mientras el ADC trabaja -> recoger.
static esp_err_t convertir(ms5611_handle_t baro, ms5611_channel_t canal, ms5611_osr_t osr, uint32_t *adc)
{
    esp_err_t err = ms5611_start(baro, canal);
    if (err != ESP_OK) return err;
    // +1 ms de margen y +1 tick: vTaskDelay(n) puede durar entre n-1 y n ticks.
    vTaskDelay(pdMS_TO_TICKS(ms5611_conversion_time_us(osr) / 1000 + 1) + 1);
    return ms5611_fetch(baro, adc);
}

static bool tomar_referencia(ms5611_handle_t baro, float *presion_suelo_pa)
{
    float suma = 0;
    int validas = 0;
    ms5611_data_t dato;
    for (int i = 0; i < MUESTRAS_REFERENCIA; i++) {
        if (ms5611_read(baro, &dato) == ESP_OK) {
            suma += dato.pressure_pa;
            validas++;
        }
    }
    if (validas == 0) return false;
    *presion_suelo_pa = suma / validas;
    return true;
}

void tarea_barometro(void *arg)
{
    ms5611_handle_t baro = arg;
    estado_barometro_t publicado = {0};
    ms5611_data_t dato;

    ESP_LOGI(TAG, "calentando %d s... (dron quieto)", SEGUNDOS_CALENTAMIENTO);
    int64_t fin = esp_timer_get_time() + SEGUNDOS_CALENTAMIENTO * 1000000LL;
    while (esp_timer_get_time() < fin) ms5611_read(baro, &dato);

    float presion_suelo_pa;
    if (!tomar_referencia(baro, &presion_suelo_pa)) {
        ESP_LOGE(TAG, "sin lecturas válidas para la referencia");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "altura 0 = %.2f hPa", presion_suelo_pa / 100.0f);

    uint32_t d1 = 0, d2 = 0;
    convertir(baro, MS5611_D2_TEMPERATURE, OSR_TEMPERATURA, &d2);

    uint32_t n = 0;
    int n_bloque = 0;
    float suma_altura = 0, suma_presion = 0;
    int64_t t_bloque = esp_timer_get_time();

    for (;;) {
        if (estado_tomar_peticion_cero_altura() && tomar_referencia(baro, &presion_suelo_pa)) {
            ESP_LOGI(TAG, "nueva altura 0 = %.2f hPa (pedido por el panel)", presion_suelo_pa / 100.0f);
        }
        if (++n % CADA_N_TEMPERATURA == 0
            && convertir(baro, MS5611_D2_TEMPERATURE, OSR_TEMPERATURA, &d2) != ESP_OK) {
            publicado.errores_i2c++;
        }
        if (convertir(baro, MS5611_D1_PRESSURE, OSR_PRESION, &d1) != ESP_OK) {
            publicado.errores_i2c++;
            continue;  // se descarta esta muestra
        }
        ms5611_compensate(baro, d1, d2, &dato);
        suma_altura += ms5611_altitude_m(dato.pressure_pa, presion_suelo_pa);
        suma_presion += dato.pressure_pa;
        n_bloque++;

        // La media de ~9 muestras baja el ruido unas 3 veces (raíz de 9).
        int64_t ahora = esp_timer_get_time();
        if (ahora - t_bloque >= PERIODO_BLOQUE_US) {
            publicado.listo = true;
            publicado.hz = n_bloque * 1e6f / (ahora - t_bloque);
            publicado.altura_m = suma_altura / n_bloque;
            publicado.presion_hpa = suma_presion / n_bloque / 100.0f;
            publicado.temp_c = dato.temp_c;
            estado_escribir_baro(&publicado);
            suma_altura = suma_presion = 0;
            n_bloque = 0;
            t_bloque = ahora;
        }
    }
}
