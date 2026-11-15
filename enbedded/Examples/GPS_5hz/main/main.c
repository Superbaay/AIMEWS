/*
 * ============================================================================
 *  GPS_5hz — Posición del dron SAMATI 5 veces por segundo con el NEO-6M
 * ============================================================================
 *
 *  Qué hace:
 *    1. Inicializa el GPS por UART1 (detecta solo los baudios).
 *    2. Lo reconfigura con comandos UBX: 115200 baud y una medida cada 200 ms
 *       (5 Hz, el máximo del NEO-6M).
 *    3. Una tarea fijada al núcleo 0 recibe cada medida y cada segundo
 *       imprime la última posición, la frecuencia real y las frases dañadas.
 *
 *  Por qué así (pensado para el dron):
 *    - A 1 Hz y 10 m/s el dron avanza 10 m entre medidas. A 5 Hz, 2 m: la
 *      navegación por puntos y el "regreso a casa" reaccionan 5 veces antes.
 *    - Los baudios suben porque a 9600 solo caben ~2 medidas/s con todas las
 *      frases (cada medida son ~500 bytes de texto). A 115200 sobra espacio
 *      y se conservan todas, incluida la lista de satélites (GSV).
 *    - La tarea va en el NÚCLEO 0 con prioridad baja: el núcleo 1 queda para
 *      el lazo de vuelo a 1 kHz (ver ejemplo IMU_1khz).
 *    - La configuración vive en la RAM del GPS: al quitarle la corriente
 *      vuelve a 9600 / 1 Hz, pero al reiniciar SOLO el ESP sigue a 115200.
 *      Por eso el driver autodetecta los baudios en cada arranque.
 *
 *  Conexiones (verificadas en la placa ESP32-S3 del proyecto):
 *
 *      GY-GPS6MV2      ESP32-S3
 *      ----------      --------
 *      VCC        ->   3V3
 *      GND        ->   GND
 *      TX         ->   GPIO18   (RX del ESP)   <- cruzados
 *      RX         ->   GPIO17   (TX del ESP)   <- necesario para configurarlo
 *
 *  Compilar y flashear:   idf.py -p COM8 flash monitor   (salir: Ctrl+])
 * ============================================================================
 */

#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "neo6m.h"  // driver en components/neo6m

static const char *TAG = "gps_5hz";

/* ---------------------------------------------------------------------------
 *  Configuración
 * ------------------------------------------------------------------------- */

#define BAUDIOS_GPS 115200
#define PERIODO_GPS_MS 200       // 200 ms = 5 Hz (mínimo del NEO-6M)
#define PERIODO_PRINT_US 1000000 // un resumen por segundo

// Núcleo y prioridad (núcleo 1 reservado al lazo de vuelo).
#define NUCLEO_GPS 0
#define PRIORIDAD_GPS 4

/* ---------------------------------------------------------------------------
 *  Tarea del GPS
 * ------------------------------------------------------------------------- */
static void tarea_gps(void *arg)
{
    neo6m_config_t cfg = NEO6M_CONFIG_DEFAULT();  // UART1, TX=17, RX=18
    cfg.baud_target = BAUDIOS_GPS;
    cfg.rate_ms = PERIODO_GPS_MS;
    cfg.task_core = NUCLEO_GPS;  // la tarea lectora del driver, también en el núcleo 0

    neo6m_handle_t gps;
    esp_err_t err = neo6m_init(&cfg, &gps);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no se pudo iniciar el GPS: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "ESP_ERR_NOT_FOUND = no llega nada por GPIO18; ESP_ERR_TIMEOUT = no acepta comandos (GPIO17)");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "GPS a %lu baud, una medida cada %d ms", (unsigned long)neo6m_get_baud(gps), PERIODO_GPS_MS);

    neo6m_data_t d;
    uint32_t epocas_ultimo = 0;
    int64_t t_ultimo = esp_timer_get_time();

    while (1) {
        if (!neo6m_wait_update(gps, &d, pdMS_TO_TICKS(1000))) {
            ESP_LOGW(TAG, "sin medidas en 1 s");
            continue;
        }
        // Aquí iría el uso real de cada medida (navegación, telemetría...).

        int64_t ahora = esp_timer_get_time();
        if (ahora - t_ultimo < PERIODO_PRINT_US) continue;
        float hz = (d.epochs - epocas_ultimo) * 1e6f / (ahora - t_ultimo);
        epocas_ultimo = d.epochs;
        t_ultimo = ahora;

        if (d.valid) {
            printf("fix %uD | %.6f, %.6f | alt %6.1f m | %2u sat | HDOP %4.1f | %5.1f km/h %3.0f° | t=%lld | "
                   "%.1f Hz | malas %lu\n",
                   d.fix_type, d.lat, d.lon, d.alt_m, d.sats_used, d.hdop, d.speed_kmh, d.course_deg,
                   (long long)neo6m_unix_time(&d), hz, (unsigned long)d.nmea_bad);
        } else {
            int mejor_snr = 0;
            for (int i = 0; i < d.n_sats; i++)
                if (d.sats[i].snr > mejor_snr) mejor_snr = d.sats[i].snr;
            printf("sin fix | %u sat a la vista, mejor señal %d dB | %.1f Hz | malas %lu\n", d.sats_view,
                   mejor_snr, hz, (unsigned long)d.nmea_bad);
        }
    }
}

void app_main(void)
{
    xTaskCreatePinnedToCore(tarea_gps, "gps", 4096, NULL, PRIORIDAD_GPS, NULL, NUCLEO_GPS);
}
