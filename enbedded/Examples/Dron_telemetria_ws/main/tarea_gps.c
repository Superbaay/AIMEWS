/*
 * tarea_gps — posición 5 veces por segundo con el NEO-6M (núcleo 0).
 *
 * Igual que el ejemplo GPS_5hz (verificado con el módulo real): UART1,
 * 115200 baud y una medida cada 200 ms (5 Hz, el máximo del NEO-6M).
 * La configuración vive en la RAM del GPS, por eso el driver autodetecta los
 * baudios en cada arranque y la vuelve a aplicar.
 */
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "estado_dron.h"
#include "neo6m.h"
#include "sensores.h"

static const char *TAG = "gps";

#define BAUDIOS_GPS     115200
#define PERIODO_GPS_MS  200        // 5 Hz
#define REINTENTO_MS    30000      // si el GPS no responde, se reintenta (cada intento ocupa el UART ~10 s)

/* Los cables TX/RX del GPS se cruzan con facilidad: antes de configurar el
 * UART se escucha PASIVAMENTE (los dos pines como entradas) cuál tiene
 * actividad. Probar a ciegas configuraría como salida el pin donde transmite
 * el GPS: dos salidas peleando por el mismo cable y datos corruptos. */
#define GPIO_A          18
#define GPIO_B          17
#define ESCUCHA_MS      1000   // a 1 Hz el GPS habla en ráfagas: escuchar un segundo entero

typedef struct {
    int rx_gpio;  // ESP RX <- TX del GPS
    int tx_gpio;  // ESP TX -> RX del GPS
} pines_gps_t;

static uint32_t contar_flancos(int gpio_a, int gpio_b, uint32_t *flancos_b, uint32_t ms)
{
    // gpio_reset_pin desconecta el pin de cualquier periférico (borrar el driver
    // UART NO suelta el pin que usaba como TX), pero deja la ENTRADA apagada:
    // sin gpio_set_direction(INPUT), gpio_get_level() siempre lee 0.
    gpio_reset_pin(gpio_a);
    gpio_reset_pin(gpio_b);
    gpio_set_direction(gpio_a, GPIO_MODE_INPUT);
    gpio_set_direction(gpio_b, GPIO_MODE_INPUT);
    vTaskDelay(pdMS_TO_TICKS(5));
    uint32_t fa = 0, fb = 0;
    int la = gpio_get_level(gpio_a), lb = gpio_get_level(gpio_b);
    int64_t fin = esp_timer_get_time() + (int64_t)ms * 1000;
    uint32_t vueltas = 0;
    while (esp_timer_get_time() < fin) {
        int va = gpio_get_level(gpio_a), vb = gpio_get_level(gpio_b);
        if (va != la) { fa++; la = va; }
        if (vb != lb) { fb++; lb = vb; }
        // Ceder la CPU de vez en cuando (núcleo 0, junto al WiFi); se pierde
        // algún flanco, pero solo se compara cuál pin tiene más actividad.
        if (++vueltas % 4000 == 0) vTaskDelay(1);
    }
    *flancos_b = fb;
    return fa;
}

static pines_gps_t elegir_pines(void)
{
    uint32_t fb;
    uint32_t fa = contar_flancos(GPIO_A, GPIO_B, &fb, ESCUCHA_MS);
    ESP_LOGI(TAG, "actividad en %d ms: GPIO%d = %lu flancos, GPIO%d = %lu flancos", ESCUCHA_MS, GPIO_A,
             (unsigned long)fa, GPIO_B, (unsigned long)fb);
    if (fb > fa) return (pines_gps_t){.rx_gpio = GPIO_B, .tx_gpio = GPIO_A};
    return (pines_gps_t){.rx_gpio = GPIO_A, .tx_gpio = GPIO_B};  // por defecto TX del GPS -> 18
}

/* Intenta iniciar el GPS con unos pines. ESP_OK = 5 Hz configurado;
 * *solo_lectura = true si llega NMEA pero el GPS no acepta comandos. */
static esp_err_t iniciar_con_pines(const pines_gps_t *pines, neo6m_handle_t *gps, bool *solo_lectura)
{
    neo6m_config_t cfg = NEO6M_CONFIG_DEFAULT();  // UART1
    cfg.rx_gpio = pines->rx_gpio;
    cfg.tx_gpio = pines->tx_gpio;
    cfg.baud_target = BAUDIOS_GPS;
    cfg.rate_ms = PERIODO_GPS_MS;
    cfg.task_core = NUCLEO_SISTEMA;
    *solo_lectura = false;

    esp_err_t err = neo6m_init(&cfg, gps);
    if (err == ESP_ERR_NOT_FOUND) {
        // La escucha pasiva vio actividad en rx_gpio, pero con nuestro TX asignado
        // no llega NMEA. Medido en la placa: al poner 3.3 V en la línea hacia el RX
        // del GPS, el módulo se reiniciaba en bucle (mal contacto de GND o del
        // cable RX). Se escucha SIN asignar TX: el GPS funciona igual, solo que no
        // se le pueden mandar comandos.
        gpio_reset_pin(pines->tx_gpio);
        cfg.tx_gpio = UART_PIN_NO_CHANGE;
        cfg.baud_target = 0;
        cfg.rate_ms = 0;
        err = neo6m_init(&cfg, gps);
        *solo_lectura = (err == ESP_OK);
        if (err == ESP_OK) {
            ESP_LOGW(TAG, "con TX en GPIO%d el GPS no responde (¿se reinicia?): escuchando sin TX. "
                          "Revisa el cable RX del GPS y su GND", pines->tx_gpio);
        }
        return err;
    }
    if (err != ESP_ERR_TIMEOUT) return err;

    // Llega NMEA (el cable TX del GPS está bien) pero el GPS no acepta comandos:
    // falla el cable hacia su RX. Mejor algo que nada: solo escuchar con su
    // configuración actual. Ojo: este NEO-6M guarda la configuración con su pila
    // de respaldo; si quedó a 5 Hz pero a 9600 baud, las frases no caben y salen
    // casi sin pausas (las "épocas" de 30 ms de silencio se cortan a ~6/s). Los
    // datos sirven igual; con el cable RX bien se reconfigura a 115200 y queda limpio.
    cfg.baud_target = 0;
    cfg.rate_ms = 0;
    err = neo6m_init(&cfg, gps);
    *solo_lectura = (err == ESP_OK);
    return err;
}

void tarea_gps(void *arg)
{
    neo6m_handle_t gps;
    // Un GPS desconectado no debe impedir que el resto del dron funcione.
    for (;;) {
        bool solo_lectura = false;
        const pines_gps_t elegidos = elegir_pines();
        const pines_gps_t *pines = &elegidos;
        esp_err_t err = iniciar_con_pines(pines, &gps, &solo_lectura);
        if (err == ESP_OK && !solo_lectura) {
            ESP_LOGI(TAG, "GPS a %lu baud y %d ms por medida (TX del GPS -> GPIO%d, RX del GPS <- GPIO%d)",
                     (unsigned long)neo6m_get_baud(gps), PERIODO_GPS_MS, pines->rx_gpio, pines->tx_gpio);
            break;
        }
        if (err == ESP_OK) {
            ESP_LOGW(TAG, "GPS en modo SOLO LECTURA a %lu baud (TX del GPS -> GPIO%d): no acepta comandos, "
                          "para 5 Hz revisa el cable RX del GPS <- GPIO%d",
                     (unsigned long)neo6m_get_baud(gps), pines->rx_gpio, pines->tx_gpio);
            break;
        }
        ESP_LOGE(TAG, "no llega NMEA por GPIO%d (%s): revisa TX, GND y VCC del GPS. Reintento en %d s",
                 pines->rx_gpio, esp_err_to_name(err), REINTENTO_MS / 1000);
        gpio_reset_pin(GPIO_A);  // no dejar ningún pin como salida entre intentos
        gpio_reset_pin(GPIO_B);
        vTaskDelay(pdMS_TO_TICKS(REINTENTO_MS));
    }

    estado_gps_t publicado = {0};
    neo6m_data_t d;
    uint32_t epocas_antes = 0;
    int64_t t_antes = esp_timer_get_time();

    for (;;) {
        bool hay_medida = neo6m_wait_update(gps, &d, pdMS_TO_TICKS(1000));
        int64_t ahora = esp_timer_get_time();

        // Frecuencia real, medida cada segundo.
        if (ahora - t_antes >= 1000000) {
            neo6m_get(gps, &d);
            publicado.hz = (d.epochs - epocas_antes) * 1e6f / (ahora - t_antes);
            epocas_antes = d.epochs;
            t_antes = ahora;
        }
        if (!hay_medida) {
            ESP_LOGW(TAG, "sin medidas en 1 s");
            publicado.listo = false;
            estado_escribir_gps(&publicado);
            continue;
        }

        publicado.listo = true;
        publicado.fix = d.valid;
        publicado.tipo_fix = d.fix_type;
        publicado.latitud = d.lat;
        publicado.longitud = d.lon;
        publicado.altitud_m = d.alt_m;
        publicado.velocidad_ms = d.speed_kmh / 3.6f;  // la API usa m/s
        publicado.rumbo_deg = d.course_deg;
        publicado.hdop = d.hdop;
        publicado.satelites_usados = d.sats_used;
        publicado.satelites_vista = d.sats_view;
        publicado.frases_malas = d.nmea_bad;
        estado_escribir_gps(&publicado);
    }
}
