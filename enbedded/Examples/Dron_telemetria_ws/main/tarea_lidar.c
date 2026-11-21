/*
 * tarea_lidar — distancia al suelo con el TF Mini Plus a 100 Hz (núcleo 0).
 *
 * Igual que el ejemplo LiDAR_TFminiPlus (verificado con el sensor real):
 * UART2, TX=GPIO7 / RX=GPIO6, autodetección de baudios y de TX/RX cruzados,
 * mediana de las últimas 5 medidas válidas.
 *
 * Es el sensor que mide la PROFUNDIDAD de la inundación: la misión en seco
 * guarda la distancia al suelo en cada waypoint y la misión tras la lluvia
 * la vuelve a medir; la diferencia es el agua. Por eso:
 *   - Mediana y no media: un reflejo raro da UNA medida absurda que la
 *     media arrastraría; la mediana de 5 la ignora (~20 ms de retardo).
 *   - Las medidas no válidas (señal débil, fuera de rango) no entran al
 *     filtro: se cuentan. Muchas seguidas = muy alto o sobre algo que no
 *     refleja (agua quieta en ángulo, superficie muy oscura).
 *   - Si el sensor no responde al arrancar se reintenta cada 2 s (se puede
 *     conectar en caliente) en vez de bloquear al resto del dron.
 *
 * Conexiones: rojo 5V/VBUS (¡no 3V3!), negro GND, verde (TX) -> GPIO6,
 * blanco (RX) -> GPIO7. Señales de 3.3 V: directo, sin convertidor.
 */
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "estado_dron.h"
#include "sensores.h"
#include "tfminiplus.h"

static const char *TAG = "lidar";

// Pines en la placa (19-nov-2026). auto_swap detecta si verde/blanco van al revés.
#define LIDAR_TX_GPIO       7      // ESP TX -> blanco (RX del sensor)
#define LIDAR_RX_GPIO       6      // ESP RX <- verde (TX del sensor)
#define FRECUENCIA_LIDAR_HZ 100
// Distancia (cm) de la lente a la parte más baja del dron (patas). Con el
// dron apoyado en el suelo la distancia debe salir ~0: mídela y ponla aquí.
#define OFFSET_MONTAJE_CM   0
#define VENTANA_MEDIANA     5      // impar
// Sin una medida válida en este tiempo, el dato filtrado deja de ser cierto.
#define MEDIDA_VIEJA_US     300000

typedef struct {
    uint16_t muestras[VENTANA_MEDIANA];
    int n, siguiente;  // búfer circular
} mediana_t;

static void mediana_agregar(mediana_t *m, uint16_t valor)
{
    m->muestras[m->siguiente] = valor;
    m->siguiente = (m->siguiente + 1) % VENTANA_MEDIANA;
    if (m->n < VENTANA_MEDIANA) m->n++;
}

// Ordena una copia por inserción (son 5 números).
static uint16_t mediana_valor(const mediana_t *m)
{
    uint16_t orden[VENTANA_MEDIANA];
    for (int i = 0; i < m->n; i++) {
        uint16_t v = m->muestras[i];
        int j = i;
        while (j > 0 && orden[j - 1] > v) {
            orden[j] = orden[j - 1];
            j--;
        }
        orden[j] = v;
    }
    return orden[m->n / 2];
}

void tarea_lidar(void *arg)
{
    tfminiplus_config_t cfg = TFMINIPLUS_CONFIG_DEFAULT();  // UART2
    cfg.tx_gpio = LIDAR_TX_GPIO;
    cfg.rx_gpio = LIDAR_RX_GPIO;
    cfg.frame_rate_hz = FRECUENCIA_LIDAR_HZ;
    cfg.task_core = NUCLEO_SISTEMA;  // la tarea interna del driver, lejos del lazo de vuelo
    cfg.task_prio = PRIORIDAD_LIDAR + 1;

    tfminiplus_handle_t lidar;
    esp_err_t err;
    while ((err = tfminiplus_init(&cfg, &lidar)) != ESP_OK) {
        ESP_LOGE(TAG, "LiDAR no encontrado (%s): revisa 5 V, verde->GPIO6, blanco->GPIO7. Reintento en 2 s",
                 esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    char version[16] = "?";
    tfminiplus_get_version(lidar, version, sizeof(version));
    ESP_LOGI(TAG, "LiDAR listo: firmware %s, %lu baud%s, %u Hz", version, (unsigned long)tfminiplus_get_baud(lidar),
             tfminiplus_pins_swapped(lidar) ? " (TX/RX cruzados)" : "", FRECUENCIA_LIDAR_HZ);

    mediana_t filtro = {0};
    estado_lidar_t e = {.listo = true};
    uint32_t invalidas_seg = 0;
    int64_t ultima_valida = 0, fin_segundo = esp_timer_get_time() + 1000000;
    tfminiplus_data_t m;

    for (;;) {
        bool llego = tfminiplus_wait_update(lidar, &m, pdMS_TO_TICKS(200));
        int64_t ahora = esp_timer_get_time();
        if (!llego) {
            ESP_LOGW(TAG, "no llegan medidas del LiDAR (¿cable verde suelto?)");
            e.hz = 0;
        } else {
            if (m.valid) {
                mediana_agregar(&filtro, m.dist_cm);
                ultima_valida = ahora;
            } else {
                invalidas_seg++;
            }
            e.cruda_cm = m.dist_cm;
            e.fuerza = m.strength;
            e.temp_c = m.temp_c;
            e.hz = m.rate_hz;
            e.tramas_malas = m.bad_frames;
        }

        // Si hace rato que no hay medidas válidas, se olvida el filtro para no
        // publicar una distancia que ya no es cierta.
        if (ahora - ultima_valida > MEDIDA_VIEJA_US) filtro.n = 0;
        e.valido = filtro.n > 0;
        if (e.valido) {
            int cm = (int)mediana_valor(&filtro) - OFFSET_MONTAJE_CM;
            e.distancia_m = (cm < 0 ? 0 : cm) / 100.0f;
        }

        if (ahora >= fin_segundo) {
            e.invalidas = invalidas_seg;
            invalidas_seg = 0;
            fin_segundo += 1000000;
        }
        estado_escribir_lidar(&e);
    }
}
