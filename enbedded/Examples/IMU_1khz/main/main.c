/*
 * ============================================================================
 *  IMU_1khz — Lectura del acelerómetro/giroscopio a 1000 Hz para el dron SAMATI
 * ============================================================================
 *
 *  Qué hace:
 *    1. Inicializa el MPU6050/MPU6500 por I2C a 400 kHz.
 *    2. Calibra el giroscopio al arrancar (el dron debe estar QUIETO).
 *    3. Lee una muestra cada 1 ms, sincronizado con el pin INT del sensor.
 *    4. Cada segundo imprime estadísticas: Hz reales, muestras perdidas,
 *       errores I2C y tiempo máximo de lectura.
 *
 *  Por qué así (pensado para un controlador de vuelo):
 *    - El SENSOR marca el ritmo con su pin INT. Si el ESP usara su propio
 *      temporizador, los dos relojes se desfasarían (±1-2 %) y cada pocos
 *      segundos leeríamos una muestra repetida o nos saltaríamos una.
 *    - La tarea de vuelo va FIJADA AL NÚCLEO 1 con prioridad alta. El núcleo 0
 *      queda para el WiFi, la telemetría y los logs, que así nunca retrasan
 *      el lazo de control.
 *    - La ISR solo "despierta" a la tarea (notificación). La lectura I2C
 *      (~450 us) se hace FUERA de la interrupción, en la tarea.
 *
 *  Conexiones (verificadas en la placa ESP32-S3 del proyecto):
 *
 *      Módulo IMU      ESP32-S3
 *      ----------      --------
 *      VCC        ->   3V3
 *      GND        ->   GND
 *      SDA        ->   GPIO8
 *      SCL        ->   GPIO9
 *      INT        ->   GPIO10
 *
 *  Presupuesto de tiempo por ciclo (1 ms = 1000 us):
 *      lectura I2C de 14 bytes  ~450 us  (medido)
 *      libre para control       ~550 us  (orientación + PID + motores)
 *
 *  Compilar y flashear:   idf.py -p COM8 flash monitor   (salir: Ctrl+])
 * ============================================================================
 */

#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mpu6x00.h"  // driver en components/mpu6x00

static const char *TAG = "imu_1khz";

/* ---------------------------------------------------------------------------
 *  Configuración
 * ------------------------------------------------------------------------- */

// Filtro paso bajo interno del sensor (DLPF). Compromiso ruido / retardo:
//   1 = 188 Hz, 1.9 ms   <- casi sin filtrar, mínimo retardo
//   2 =  98 Hz, 2.8 ms   <- buen punto de partida para un dron
//   3 =  42 Hz, 4.8 ms   <- más limpio, pero el PID reacciona más tarde
#define DLPF_DRON 2

// Muestras para calibrar el bias del giroscopio (2000 = 2 s a 1 kHz).
#define MUESTRAS_CALIBRACION 2000

// Núcleos y prioridades de las tareas.
#define NUCLEO_VUELO      1
#define NUCLEO_SISTEMA    0
#define PRIORIDAD_VUELO   (configMAX_PRIORITIES - 2)  // casi la máxima
#define PRIORIDAD_ESTADO  2                            // baja

/* ---------------------------------------------------------------------------
 *  Estado compartido entre la tarea de vuelo y la de estadísticas
 * ------------------------------------------------------------------------- */

static mpu_handle_t imu;

// La tarea de vuelo escribe y la de estadísticas lee: se protege con una
// sección crítica (spinlock), necesaria porque corren en núcleos distintos.
static portMUX_TYPE candado_estado = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    mpu_data_t ultima_muestra;      // última lectura válida (g, °/s, °C)
    uint32_t muestras;              // lecturas hechas en el último segundo
    uint32_t perdidas;              // interrupciones que no alcanzamos a leer
    uint32_t errores_i2c;           // lecturas fallidas
    uint32_t timeouts;              // veces que INT no llegó a tiempo
    int64_t lectura_max_us;         // peor tiempo de lectura I2C
} estado_imu_t;

static estado_imu_t estado;

/* ---------------------------------------------------------------------------
 *  Tarea de vuelo (núcleo 1): el corazón del controlador
 * ------------------------------------------------------------------------- */

static void tarea_vuelo(void *arg)
{
    // 1) Calibración: con el dron quieto, el giro debería marcar 0 °/s.
    //    Lo que marque es el bias; el driver lo restará de cada lectura.
    ESP_LOGI(TAG, "Calibrando giroscopio, no muevas el dron...");
    ESP_ERROR_CHECK(mpu_calibrate_gyro(imu, MUESTRAS_CALIBRACION));

    // 2) Conectar el pin INT: a partir de aquí, cada muestra nueva del
    //    sensor despierta a ESTA tarea.
    ESP_ERROR_CHECK(mpu_int_attach(imu, xTaskGetCurrentTaskHandle()));
    ESP_LOGI(TAG, "Lazo a 1 kHz en marcha (núcleo %d)", xPortGetCoreID());

    int64_t tiempo_anterior_us = esp_timer_get_time();

    while (1) {
        // 3) Dormir hasta la siguiente muestra. Devuelve cuántas interrupciones
        //    se acumularon: 1 = normal, >1 = llegamos tarde y se perdieron n-1,
        //    0 = pasaron 5 ms sin INT (cable suelto o sensor colgado).
        uint32_t pendientes = mpu_wait_sample(pdMS_TO_TICKS(5));
        if (pendientes == 0) {
            taskENTER_CRITICAL(&candado_estado);
            estado.timeouts++;
            taskEXIT_CRITICAL(&candado_estado);
            continue;
        }

        // 4) dt REAL entre muestras (≈ 0.001 s). Mejor medirlo que suponerlo:
        //    el reloj del sensor no es exactamente 1000 Hz.
        int64_t ahora_us = esp_timer_get_time();
        float dt = (ahora_us - tiempo_anterior_us) * 1e-6f;
        tiempo_anterior_us = ahora_us;

        // 5) Leer acelerómetro + temperatura + giroscopio (14 bytes).
        mpu_data_t muestra;
        esp_err_t resultado = mpu_read(imu, &muestra);
        int64_t lectura_us = esp_timer_get_time() - ahora_us;

        // 6) ---- Aquí va el control de vuelo ----
        //    a) Estimar la orientación (roll/pitch/yaw) con `muestra` y `dt`:
        //       filtro complementario o de Kalman.
        //    b) PID: error = consigna - orientación -> corrección por eje.
        //    c) Mezclador: corrección -> velocidad de cada motor (PWM/DShot).
        //    Todo eso debe caber en los ~550 us libres de cada ciclo.
        (void)dt;

        // 7) Publicar estadísticas para la otra tarea (sección crítica corta:
        //    solo copiar números, nunca hacer printf aquí dentro).
        taskENTER_CRITICAL(&candado_estado);
        estado.muestras++;
        estado.perdidas += pendientes - 1;
        if (resultado == ESP_OK) {
            estado.ultima_muestra = muestra;
        } else {
            estado.errores_i2c++;
        }
        if (lectura_us > estado.lectura_max_us) {
            estado.lectura_max_us = lectura_us;
        }
        taskEXIT_CRITICAL(&candado_estado);
    }
}

/* ---------------------------------------------------------------------------
 *  Tarea de estado (núcleo 0): imprime una vez por segundo
 * ------------------------------------------------------------------------- */

static void tarea_estado(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        // Copiar y reiniciar los contadores dentro de la sección crítica;
        // el printf (lento) se hace fuera, con la copia.
        taskENTER_CRITICAL(&candado_estado);
        estado_imu_t copia = estado;
        estado.muestras = 0;
        estado.perdidas = 0;
        estado.errores_i2c = 0;
        estado.timeouts = 0;
        estado.lectura_max_us = 0;
        taskEXIT_CRITICAL(&candado_estado);

        // Sin muestras ni timeouts = la tarea de vuelo aún está calibrando.
        if (copia.muestras == 0 && copia.timeouts == 0) {
            continue;
        }

        const mpu_data_t *m = &copia.ultima_muestra;
        ESP_LOGI(TAG, "%4lu Hz | perdidas %lu | err %lu | timeouts %lu | lectura max %lld us",
                 (unsigned long)copia.muestras, (unsigned long)copia.perdidas,
                 (unsigned long)copia.errores_i2c, (unsigned long)copia.timeouts,
                 copia.lectura_max_us);
        ESP_LOGI(TAG, "     accel[g] %+6.3f %+6.3f %+6.3f | giro[dps] %+7.2f %+7.2f %+7.2f | %4.1f C",
                 m->ax, m->ay, m->az, m->gx, m->gy, m->gz, m->temp_c);

        // Avisos útiles durante las pruebas.
        if (copia.perdidas > 0) {
            ESP_LOGW(TAG, "Se perdieron muestras: la tarea de vuelo tarda demasiado");
        }
        if (copia.timeouts > 0) {
            ESP_LOGW(TAG, "INT no llega: revisa el cable INT -> GPIO10");
        }
    }
}

/* ---------------------------------------------------------------------------
 *  Arranque
 * ------------------------------------------------------------------------- */

void app_main(void)
{
    // Pines 8/9/10, 400 kHz, 1000 Hz, ±4 g, ±500 °/s (ver mpu6x00.h).
    mpu_config_t configuracion = MPU_CONFIG_DEFAULT();
    configuracion.dlpf = DLPF_DRON;

    esp_err_t resultado = mpu_init(&configuracion, &imu);
    if (resultado != ESP_OK) {
        ESP_LOGE(TAG, "No responde la IMU (%s): revisa VCC, GND, SDA=8, SCL=9",
                 esp_err_to_name(resultado));
        return;
    }
    ESP_LOGI(TAG, "IMU detectada: %s (WHO_AM_I = 0x%02X)", mpu_model(imu), mpu_who_am_i(imu));

    // Tarea de vuelo en el núcleo 1, prioridad alta.
    xTaskCreatePinnedToCore(tarea_vuelo, "tarea_vuelo", 4096, NULL,
                            PRIORIDAD_VUELO, NULL, NUCLEO_VUELO);
    // Estadísticas en el núcleo 0, prioridad baja.
    xTaskCreatePinnedToCore(tarea_estado, "tarea_estado", 4096, NULL,
                            PRIORIDAD_ESTADO, NULL, NUCLEO_SISTEMA);
}
