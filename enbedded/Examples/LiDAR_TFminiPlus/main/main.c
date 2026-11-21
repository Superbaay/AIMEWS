/*
 * ============================================================================
 *  LiDAR_TFminiPlus — Altura sobre el suelo del dron SAMATI con el TF Mini Plus
 * ============================================================================
 *
 *  Qué hace:
 *    1. Busca el LiDAR en el UART2 (prueba baudios y, si hace falta, los
 *       cables TX/RX al revés) y lo pone a 100 medidas por segundo.
 *    2. Con el sensor mirando HACIA ABAJO, cada medida es la distancia de la
 *       lente al suelo. Se le resta lo que sobresale el tren de aterrizaje
 *       por debajo de la lente (OFFSET_MONTAJE_CM).
 *    3. Filtra con la MEDIANA de las últimas 5 medidas válidas.
 *    4. Cada 100 ms imprime la altura filtrada, la última medida cruda, la
 *       fuerza de la señal, los Hz reales, cuántas medidas del bloque no
 *       fueron válidas y un aviso de "SUELO CERCA" para el aterrizaje.
 *
 *  Por qué así (pensado para el dron):
 *    - La tarea va FIJADA AL NÚCLEO 0: el núcleo 1 queda para el lazo de
 *      vuelo a 1 kHz (ver ejemplo IMU_1khz), igual que el barómetro y el GPS.
 *    - Mediana y no media: un reflejo raro (una hoja, un charco, el borde de
 *      una mesa) da UNA medida absurda. La media la arrastraría; la mediana
 *      de 5 la ignora y solo añade ~20 ms de retardo a 100 Hz.
 *    - Las medidas no válidas (señal débil, fuera de rango, más de 12 m) no
 *      entran al filtro: se cuentan. Muchas seguidas = el dron está muy alto
 *      o sobre algo que no refleja (agua, superficie muy oscura).
 *    - Si el sensor no responde al arrancar, se reintenta cada 2 s en lugar
 *      de reiniciar la placa (se puede conectar el sensor en caliente).
 *
 *  Conexiones (verificadas en la placa ESP32-S3 del proyecto):
 *
 *      TF Mini Plus         ESP32-S3
 *      ------------         --------
 *      Rojo   (+5 V)   ->   5V / VBUS   (¡NO a 3V3! necesita 4.5-5.5 V)
 *      Negro  (GND)    ->   GND
 *      Verde  (TX)     ->   GPIO4       (RX del ESP)
 *      Blanco (RX)     ->   GPIO5       (TX del ESP)
 *
 *  Las señales del sensor son de 3.3 V: van directo, sin convertidor.
 *  Usa el UART2 para no chocar con el GPS (UART1, GPIO17/18).
 *
 *  Medido en la placa (sensor quieto a ~2.1 m de una pared):
 *      firmware 2.1.5, 115200 baud, 98 Hz reales al pedir 100
 *      ruido: ±1 cm, fuerza de señal ~5200, 0 tramas corruptas
 *
 *  Si no lo encuentra, el log dice por qué. El caso típico: "GPIO5 y GPIO4
 *  están en 0 V aun con pull-up" = el sensor no recibe los 5 V.
 *
 *  Compilar y flashear:   idf.py -p COM8 flash monitor   (salir: Ctrl+])
 * ============================================================================
 */

#include <stdbool.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tfminiplus.h"  // driver en components/tfminiplus

static const char *TAG = "lidar";

/* ---------------------------------------------------------------------------
 *  Configuración
 * ------------------------------------------------------------------------- */

// Medidas por segundo que se le piden al sensor (1..1000; mejor divisores
// de 1000). 100 Hz basta para aterrizar; para seguir el terreno a ras de
// suelo y a velocidad se puede subir a 250.
#define FRECUENCIA_HZ 100

// Distancia (cm) entre la lente del LiDAR y la parte más baja del dron
// (patas del tren de aterrizaje). Con el dron en el suelo la altura debe
// salir ~0: mide la distancia con el dron apoyado y ponla aquí.
#define OFFSET_MONTAJE_CM 0

// Medidas válidas que entran a la mediana (impar).
#define VENTANA_MEDIANA 5

// Por debajo de esta altura (cm) se avisa "SUELO CERCA" (fase final del
// aterrizaje: bajar despacio).
#define ALTURA_ATERRIZAJE_CM 30

// Periodo de impresión (us).
#define PERIODO_PRINT_US 100000

// Núcleo y prioridad de la tarea (núcleo 1 reservado al lazo de vuelo).
#define NUCLEO_LIDAR 0
#define PRIORIDAD_LIDAR 5

/* ---------------------------------------------------------------------------
 *  Filtro de mediana
 * ------------------------------------------------------------------------- */

typedef struct {
    uint16_t muestras[VENTANA_MEDIANA];
    int n;          // muestras guardadas (hasta VENTANA_MEDIANA)
    int siguiente;  // posición donde va la próxima (búfer circular)
} mediana_t;

static void mediana_agregar(mediana_t *m, uint16_t valor)
{
    m->muestras[m->siguiente] = valor;
    m->siguiente = (m->siguiente + 1) % VENTANA_MEDIANA;
    if (m->n < VENTANA_MEDIANA) m->n++;
}

// Ordena una copia (son 5 números: inserción es lo más simple y rápido).
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

/* ---------------------------------------------------------------------------
 *  Tarea del LiDAR
 * ------------------------------------------------------------------------- */

static void tarea_lidar(void *arg)
{
    tfminiplus_config_t cfg = TFMINIPLUS_CONFIG_DEFAULT();  // UART2, TX=GPIO5, RX=GPIO4
    cfg.frame_rate_hz = FRECUENCIA_HZ;
    cfg.task_core = NUCLEO_LIDAR;  // la tarea interna del driver, en el mismo núcleo

    tfminiplus_handle_t lidar;
    esp_err_t err;
    while ((err = tfminiplus_init(&cfg, &lidar)) != ESP_OK) {
        ESP_LOGE(TAG, "LiDAR no encontrado (%s), reintento en 2 s", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    char version[16] = "?";
    tfminiplus_get_version(lidar, version, sizeof(version));
    ESP_LOGI(TAG, "LiDAR listo: firmware %s, %u Hz, offset de montaje %d cm", version, FRECUENCIA_HZ,
             OFFSET_MONTAJE_CM);

    mediana_t filtro = {0};
    uint32_t validas = 0, invalidas = 0;  // en el bloque de impresión actual
    int64_t proximo_print = esp_timer_get_time() + PERIODO_PRINT_US;
    tfminiplus_data_t m = {0};

    for (;;) {
        // Espera la siguiente medida (llega cada ~10 ms a 100 Hz).
        if (!tfminiplus_wait_update(lidar, &m, pdMS_TO_TICKS(200))) {
            ESP_LOGW(TAG, "no llegan medidas del LiDAR (¿cable verde suelto?)");
            continue;
        }
        if (m.valid) {
            mediana_agregar(&filtro, m.dist_cm);
            validas++;
        } else {
            invalidas++;
        }

        int64_t ahora = esp_timer_get_time();
        if (ahora < proximo_print) continue;
        proximo_print += PERIODO_PRINT_US;

        if (filtro.n == 0) {
            printf("altura  ----  | sin medidas válidas todavía | señal %5u | %5.1f Hz\n", m.strength, m.rate_hz);
        } else {
            int altura = (int)mediana_valor(&filtro) - OFFSET_MONTAJE_CM;
            if (altura < 0) altura = 0;
            printf("altura %4d cm | cruda %4u | señal %5u | %5.1f Hz | no válidas %2lu/%-2lu %s\n", altura,
                   m.dist_cm, m.strength, m.rate_hz, (unsigned long)invalidas,
                   (unsigned long)(validas + invalidas), altura < ALTURA_ATERRIZAJE_CM ? "<< SUELO CERCA" : "");
        }
        // Todo el bloque inválido: el filtro guarda alturas viejas, mejor
        // olvidarlas para no mostrar un dato que ya no es cierto.
        if (validas == 0) filtro.n = 0;
        validas = invalidas = 0;
    }
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(1500));  // dar tiempo a que el monitor serie se conecte
    xTaskCreatePinnedToCore(tarea_lidar, "lidar", 4096, NULL, PRIORIDAD_LIDAR, NULL, NUCLEO_LIDAR);
}
