/*
 * ============================================================================
 *  GPS_basico — Lectura del GPS NEO-6M (GY-GPS6MV2) para el dron SAMATI
 * ============================================================================
 *
 *  Qué hace:
 *    1. Inicializa el GPS por UART1 detectando solo los baudios.
 *    2. Pide al módulo su versión (comprueba que el cable TX del ESP llega).
 *    3. En cada medida (fija 1 por segundo con un comando UBX) imprime
 *       posición, altitud, satélites, precisión, velocidad y hora UTC.
 *    4. Sin fix muestra cada satélite visible con su señal (dB): sirve para
 *       saber si la antena "ve el cielo".
 *
 *  Conceptos:
 *    - El GPS manda texto NMEA ($GPRMC, $GPGGA...) sin parar. El driver
 *      verifica el checksum de cada frase y junta todas las de una misma
 *      medida antes de entregarla, para no mezclar datos de dos segundos.
 *    - "fix" = el GPS ya calculó su posición. Necesita ver al menos 4
 *      satélites; el primero tras encender tarda de 30 s a varios minutos y
 *      en interiores casi nunca llega.
 *    - HDOP = cuánto empeora la precisión por la geometría de los satélites:
 *      < 2 bueno, 2-5 aceptable, > 5 malo.
 *
 *  Conexiones (verificadas en la placa ESP32-S3 del proyecto):
 *
 *      GY-GPS6MV2      ESP32-S3
 *      ----------      --------
 *      VCC        ->   3V3
 *      GND        ->   GND
 *      TX         ->   GPIO18   (RX del ESP)   <- cruzados
 *      RX         ->   GPIO17   (TX del ESP)
 *
 *  Módulo verificado: firmware u-blox 7.03 (45969), hardware 00040007.
 *  El LED del módulo solo parpadea cuando YA hay fix (apagado no significa
 *  que esté sin corriente).
 *
 *  Compilar y flashear:   idf.py -p COM8 flash monitor   (salir: Ctrl+])
 * ============================================================================
 */

#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "neo6m.h"  // driver en components/neo6m

static const char *TAG = "gps_basico";

void app_main(void)
{
    neo6m_config_t cfg = NEO6M_CONFIG_DEFAULT();  // UART1, TX=17, RX=18, baudios automáticos
    // El GPS guarda su configuración en RAM mientras tenga corriente: si antes
    // corrió GPS_5hz seguiría a 5 Hz. Se fija 1 Hz para que este ejemplo
    // haga siempre lo mismo.
    cfg.rate_ms = 1000;
    neo6m_handle_t gps;
    esp_err_t err = neo6m_init(&cfg, &gps);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no se pudo iniciar el GPS: %s (revisa cableado y alimentación)", esp_err_to_name(err));
        return;
    }

    char sw[31], hw[11];
    if (neo6m_get_version(gps, sw, sizeof(sw), hw, sizeof(hw)) == ESP_OK)
        ESP_LOGI(TAG, "u-blox software %s, hardware %s", sw, hw);
    else
        ESP_LOGW(TAG, "el GPS no responde a comandos: revisa el cable GPS RX -> GPIO17");

    neo6m_data_t d;
    while (1) {
        if (!neo6m_wait_update(gps, &d, pdMS_TO_TICKS(3000))) {
            ESP_LOGW(TAG, "sin datos del GPS en 3 s (¿cable TX suelto?)");
            continue;
        }
        if (d.time_valid) printf("%02u:%02u:%02u UTC | ", d.hour, d.minute, d.second);
        else printf("--:--:-- UTC | ");  // sin satélites el GPS aún no sabe la hora

        if (d.valid) {
            printf("fix %uD | %.6f, %.6f | alt %.1f m | %u sat | HDOP %.1f | %.1f km/h\n", d.fix_type, d.lat,
                   d.lon, d.alt_m, d.sats_used, d.hdop, d.speed_kmh);
        } else {
            printf("sin fix | %u satélites a la vista:", d.sats_view);
            for (int i = 0; i < d.n_sats; i++) printf(" %u(%udB)", d.sats[i].prn, d.sats[i].snr);
            printf("\n");
        }
    }
}
