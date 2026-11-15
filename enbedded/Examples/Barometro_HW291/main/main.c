/*
 * ============================================================================
 *  Barometro_HW291 — Altura relativa del dron SAMATI con el barómetro HW-291
 * ============================================================================
 *
 *  Qué hace:
 *    1. Inicializa el barómetro por I2C a 400 kHz y detecta si el chip es
 *       MS5611 o MS5607 (el HW-291 de la placa resultó ser MS5607).
 *    2. Lo deja calentar unos segundos y toma la presión del suelo como
 *       "altura 0" (el dron debe estar QUIETO al arrancar).
 *    3. Mide la presión a ~89 Hz con OSR 4096 (máxima resolución).
 *    4. Cada 100 ms imprime la altura relativa, presión, temperatura, Hz
 *       reales y errores I2C.
 *
 *  Por qué así (pensado para el dron):
 *    - La tarea va FIJADA AL NÚCLEO 0 con prioridad baja: el núcleo 1 queda
 *      para el lazo de vuelo a 1 kHz (ver ejemplo IMU_1khz). El barómetro es
 *      lento (9 ms por conversión) y no debe retrasar el control.
 *    - Se usa la API asíncrona: se lanza la conversión, la tarea DUERME
 *      mientras el ADC trabaja y luego recoge el resultado. La CPU queda
 *      libre esos 9 ms (con ms5611_read() también dormiría, pero así se ve
 *      dónde se podría intercalar otro trabajo).
 *    - La temperatura cambia despacio: se mide solo 1 de cada 20 veces. Así
 *      casi todo el tiempo del ADC va a la presión (53 Hz -> 89 Hz).
 *    - Cada línea impresa es la MEDIA de las ~9 muestras de ese bloque de
 *      100 ms: el ruido baja unas 3 veces (raíz de 9) sin añadir retardo
 *      más allá del propio bloque.
 *    - Un fallo de I2C no reinicia la placa: se cuenta y se sigue midiendo.
 *
 *  Conexiones (verificadas en la placa ESP32-S3 del proyecto):
 *
 *      Módulo HW-291   ESP32-S3
 *      -------------   --------
 *      VCC        ->   3V3
 *      GND        ->   GND
 *      SDA        ->   GPIO8
 *      SCL        ->   GPIO9
 *      (CSB a GND en el módulo -> dirección 0x77)
 *
 *  Para usarlo JUNTO al IMU (mismos pines 8/9): crear el bus una sola vez
 *  con i2c_new_master_bus() y pasarlo en cfg.bus a los dos drivers. Las
 *  direcciones no chocan (IMU 0x68, barómetro 0x77).
 *
 *  Resolución medida en la placa (OSR 4096, dron quieto):
 *      una muestra:           2.3 Pa ~ 20 cm de altura (desviación típica)
 *      media de 100 ms:       ~10 cm (en teoría 7 cm; el resto es deriva
 *                             lenta, ~12 cm en 30 s, que promediar no quita)
 *
 *  Compilar y flashear:   idf.py -p COM8 flash monitor   (salir: Ctrl+])
 * ============================================================================
 */

#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ms5611.h"  // driver en components/ms5611

static const char *TAG = "barometro";

/* ---------------------------------------------------------------------------
 *  Configuración
 * ------------------------------------------------------------------------- */

// Segundos de calentamiento antes de fijar la altura 0. Al convertir sin
// parar el chip sube 1-2 °C y la altura deriva ~1 m en el primer minuto;
// esperar un poco reduce esa deriva.
#define SEGUNDOS_CALENTAMIENTO 5

// Muestras promediadas para la referencia del suelo.
#define MUESTRAS_REFERENCIA 64

// Una medida de temperatura por cada N de presión.
#define CADA_N_TEMPERATURA 20

// Periodo de impresión (us).
#define PERIODO_PRINT_US 100000

// Núcleo y prioridad de la tarea (núcleo 1 reservado al lazo de vuelo).
#define NUCLEO_BAROMETRO 0
#define PRIORIDAD_BAROMETRO 3

/* ---------------------------------------------------------------------------
 *  Conversión asíncrona: lanzar -> dormir -> recoger
 * ------------------------------------------------------------------------- */
static esp_err_t convertir(ms5611_handle_t baro, ms5611_channel_t canal, ms5611_osr_t osr,
                           uint32_t *adc)
{
    esp_err_t err = ms5611_start(baro, canal);
    if (err != ESP_OK) return err;
    // +1 ms de margen y +1 tick: vTaskDelay(n) puede durar entre n-1 y n ticks.
    vTaskDelay(pdMS_TO_TICKS(ms5611_conversion_time_us(osr) / 1000 + 1) + 1);
    return ms5611_fetch(baro, adc);
}

/* ---------------------------------------------------------------------------
 *  Tarea del barómetro
 * ------------------------------------------------------------------------- */
static void tarea_barometro(void *arg)
{
    ms5611_config_t cfg = MS5611_CONFIG_DEFAULT();  // SDA=8, SCL=9, 0x77, AUTO
    cfg.osr_pressure = MS5611_OSR_4096;             // máxima resolución en presión
    cfg.osr_temperature = MS5611_OSR_1024;          // la temperatura no necesita tanta

    ms5611_handle_t baro;
    esp_err_t err = ms5611_init(&cfg, &baro);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no se pudo iniciar el barómetro: %s (revisa cableado)", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "chip: %s", ms5611_get_variant(baro) == MS5611_VARIANT_MS5607 ? "MS5607" : "MS5611");

    // 1) Calentamiento: medir y descartar.
    ESP_LOGI(TAG, "calentando %d s... (dron quieto)", SEGUNDOS_CALENTAMIENTO);
    int64_t fin = esp_timer_get_time() + SEGUNDOS_CALENTAMIENTO * 1000000LL;
    ms5611_data_t dato;
    while (esp_timer_get_time() < fin) ms5611_read(baro, &dato);

    // 2) Referencia del suelo = altura 0.
    float presion_suelo_pa = 0;
    int validas = 0;
    for (int i = 0; i < MUESTRAS_REFERENCIA; i++) {
        if (ms5611_read(baro, &dato) == ESP_OK) {
            presion_suelo_pa += dato.pressure_pa;
            validas++;
        }
    }
    if (validas == 0) {
        ESP_LOGE(TAG, "sin lecturas válidas para la referencia");
        vTaskDelete(NULL);
        return;
    }
    presion_suelo_pa /= validas;
    ESP_LOGI(TAG, "altura 0 = %.2f hPa (altitud ISA aprox. %.0f m)", presion_suelo_pa / 100.0f,
             ms5611_altitude_m(presion_suelo_pa, 101325.0f));

    // 3) Bucle de medida.
    uint32_t d1 = 0, d2 = 0;
    convertir(baro, MS5611_D2_TEMPERATURE, cfg.osr_temperature, &d2);

    uint32_t n_muestras = 0, errores_i2c = 0;
    float suma_altura = 0, suma_presion = 0;
    int n_bloque = 0;
    int64_t t_bloque = esp_timer_get_time();

    while (1) {
        if (++n_muestras % CADA_N_TEMPERATURA == 0) {
            if (convertir(baro, MS5611_D2_TEMPERATURE, cfg.osr_temperature, &d2) != ESP_OK) errores_i2c++;
        }
        if (convertir(baro, MS5611_D1_PRESSURE, cfg.osr_pressure, &d1) != ESP_OK) {
            errores_i2c++;
            continue;  // se descarta esta muestra, la siguiente vuelve a intentar
        }

        ms5611_compensate(baro, d1, d2, &dato);
        suma_altura += ms5611_altitude_m(dato.pressure_pa, presion_suelo_pa);
        suma_presion += dato.pressure_pa;
        n_bloque++;

        int64_t ahora = esp_timer_get_time();
        if (ahora - t_bloque >= PERIODO_PRINT_US) {
            float hz = n_bloque * 1e6f / (ahora - t_bloque);
            printf("altura %+7.2f m | P %8.2f hPa | T %5.2f C | %3.0f Hz | errores I2C %lu\n",
                   suma_altura / n_bloque, suma_presion / n_bloque / 100.0f, dato.temp_c, hz,
                   (unsigned long)errores_i2c);
            suma_altura = suma_presion = 0;
            n_bloque = 0;
            t_bloque = ahora;
        }
    }
}

void app_main(void)
{
    xTaskCreatePinnedToCore(tarea_barometro, "barometro", 4096, NULL, PRIORIDAD_BAROMETRO, NULL,
                            NUCLEO_BAROMETRO);
}
