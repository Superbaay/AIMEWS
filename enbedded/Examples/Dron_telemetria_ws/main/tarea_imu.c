/*
 * tarea_imu — acelerómetro y giroscopio a 1000 Hz (núcleo 1).
 *
 * Igual que el ejemplo IMU_1khz: el SENSOR marca el ritmo (pin INT en GPIO16
 * o, sin INT, su FIFO interno). La lectura I2C (~450 us) se
 * hace aquí. Además se estima la orientación con un filtro complementario.
 */
#include <math.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "estado_dron.h"
#include "mpu6x00.h"
#include "sensores.h"

static const char *TAG = "imu";

// Muestras para calibrar el bias del giroscopio (2000 = 2 s a 1 kHz, dron QUIETO).
#define MUESTRAS_CALIBRACION 2000

// Constante de tiempo del filtro complementario. Por debajo de ~0.5 s manda
// el giroscopio (rápido, sin ruido de vibración); por encima, el acelerómetro
// (lento, pero no deriva). alfa = tau / (tau + dt) ≈ 0.998 a 1 kHz.
#define TAU_FILTRO_S 0.5f

// El acelerómetro solo mide la gravedad si el dron no está acelerando: si el
// módulo del vector se aleja de 1 g se ignora y se integra solo el giro.
#define G_MIN 0.85f
#define G_MAX 1.15f

#define RAD_A_GRADOS (180.0f / (float)M_PI)

/* ---------------------------------------------------------------------------
 *  Filtro complementario
 * ------------------------------------------------------------------------- */
typedef struct {
    float alabeo, cabeceo, guinada;  // grados
    bool iniciado;
} orientacion_t;

static float envolver_180(float grados)
{
    while (grados > 180.0f) grados -= 360.0f;
    while (grados < -180.0f) grados += 360.0f;
    return grados;
}

static void filtro_complementario(orientacion_t *o, const mpu_data_t *m, float dt)
{
    // Ángulos que "ve" el acelerómetro (solo válidos con el dron sin acelerar).
    float alabeo_acc = atan2f(m->ay, m->az) * RAD_A_GRADOS;
    float cabeceo_acc = atan2f(-m->ax, sqrtf(m->ay * m->ay + m->az * m->az)) * RAD_A_GRADOS;

    if (!o->iniciado) {
        // Primera muestra: arrancar desde el acelerómetro, no desde 0.
        o->alabeo = alabeo_acc;
        o->cabeceo = cabeceo_acc;
        o->guinada = 0;
        o->iniciado = true;
        return;
    }

    // 1) Predicción con el giroscopio: ángulo += velocidad * dt.
    o->alabeo += m->gx * dt;
    o->cabeceo += m->gy * dt;
    o->guinada += m->gz * dt;

    // 2) Corrección lenta hacia el acelerómetro (si es fiable).
    float g = sqrtf(m->ax * m->ax + m->ay * m->ay + m->az * m->az);
    if (g > G_MIN && g < G_MAX) {
        float alfa = TAU_FILTRO_S / (TAU_FILTRO_S + dt);
        o->alabeo = alfa * o->alabeo + (1.0f - alfa) * alabeo_acc;
        o->cabeceo = alfa * o->cabeceo + (1.0f - alfa) * cabeceo_acc;
    }

    // La guiñada no tiene referencia absoluta sin magnetómetro: solo se
    // integra el giro (deriva poco a poco). Todo se mantiene en -180..180 para
    // que un giro completo (o un rato sin corrección) no deje ángulos de -1400°.
    o->alabeo = envolver_180(o->alabeo);
    o->cabeceo = envolver_180(o->cabeceo);
    o->guinada = envolver_180(o->guinada);
}

/* ---------------------------------------------------------------------------
 *  Tarea
 *
 *  Las muestras SIEMPRE se leen del FIFO interno del sensor (1000 muestras/s
 *  exactas, marcadas por su reloj). Lo que cambia es quién despierta a la tarea:
 *    - INT (GPIO16): cada 2 muestras nuevas (ver MUESTRAS_POR_LECTURA).
 *    - Sin INT (cable suelto): un periodo fijo de 2 ms -> ráfagas de 2 muestras.
 *  Con el FIFO, si una lectura se retrasa porque el barómetro ocupa el bus
 *  compartido, las muestras esperan en el sensor (caben 42 ms) en vez de
 *  perderse. Leer directo los registros (como IMU_1khz) perdía 1-4 por segundo.
 * ------------------------------------------------------------------------- */

#define PERIODO_FIFO_MS   2
// Muestras por lectura del FIFO con INT. Con 1, el IMU ocupaba el bus I2C
// compartido el 65-90 % del tiempo (lectura del contador + 12 bytes cada ms) y
// el barómetro fallaba por timeout. Con 2: una sola ráfaga de 24 bytes cada
// 2 ms, la mitad de lecturas del contador y ~2 ms de latencia máxima.
#define MUESTRAS_POR_LECTURA 2

// Salto máximo de la temperatura cruda entre muestras para darla por buena:
// 300 LSB ≈ 0.9 °C (MPU6500: 333.87 LSB/°C). Entre dos muestras (1 ms) la
// temperatura real cambia mucho menos; un trozo de otro eje casi nunca cae ahí.
#define SALTO_TEMP_MAX       300
#define MAX_RAFAGA        32   // 32 muestras x 14 B = 448 B < 512 B del FIFO

typedef struct {
    orientacion_t orientacion;
    estado_imu_t publicado;
    uint32_t muestras, perdidas, lectura_max_us;
    uint64_t lectura_suma_us;
    uint32_t lecturas;
    int temp_ref;           // temperatura cruda de referencia (marca de sincronía)
    int64_t t_segundo;
} ciclo_imu_t;

static void procesar_muestra(ciclo_imu_t *ci, const mpu_data_t *m, float dt)
{
    filtro_complementario(&ci->orientacion, m, dt);
    estado_imu_t *p = &ci->publicado;
    p->listo = true;
    p->alabeo = ci->orientacion.alabeo;
    p->cabeceo = ci->orientacion.cabeceo;
    p->guinada = ci->orientacion.guinada;
    p->vel_alabeo = m->gx;
    p->vel_cabeceo = m->gy;
    p->vel_guinada = m->gz;
    p->ax = m->ax;
    p->ay = m->ay;
    p->az = m->az;
    ci->muestras++;
    // ---- Aquí iría el control de vuelo (PID + mezclador de motores) ----
}

/* Cada segundo: frecuencia real, pérdidas y peor lectura; luego publica. */
static void cerrar_ciclo(ciclo_imu_t *ci, int64_t ahora)
{
    if (ahora - ci->t_segundo >= 1000000) {
        ci->publicado.hz = ci->muestras * 1e6f / (ahora - ci->t_segundo);
        ci->publicado.perdidas = ci->perdidas;
        ci->publicado.lectura_max_us = ci->lectura_max_us;
        ci->publicado.lectura_media_us = ci->lecturas ? (uint32_t)(ci->lectura_suma_us / ci->lecturas) : 0;
        ci->muestras = ci->perdidas = ci->lectura_max_us = ci->lecturas = 0;
        ci->lectura_suma_us = 0;
        ci->t_segundo = ahora;
    }
    estado_escribir_imu(&ci->publicado);
}

static void lazo_imu(mpu_handle_t imu, ciclo_imu_t *ci, bool con_int)
{
    static mpu_raw_t rafaga[MAX_RAFAGA];
    const float dt = 0.001f;  // el reloj del sensor marca 1 ms entre muestras
    uint32_t sin_int_seguidos = 0;
    TickType_t siguiente = xTaskGetTickCount();
    ESP_ERROR_CHECK(mpu_fifo_start(imu));

    for (;;) {
        if (con_int) {
            // Se lee cada MUESTRAS_POR_LECTURA interrupciones. Si INT falla, a los
            // 5 ms se lee igual: el FIFO no pierde nada.
            uint32_t llegadas = 0;
            while (llegadas < MUESTRAS_POR_LECTURA) {
                uint32_t n_int = mpu_wait_sample(pdMS_TO_TICKS(5));
                if (n_int == 0) break;
                llegadas += n_int;
            }
            if (llegadas == 0 && ++sin_int_seguidos == 200) {
                ESP_LOGW(TAG, "INT dejó de llegar: se sigue leyendo el FIFO cada 5 ms");
            } else if (llegadas > 0) {
                sin_int_seguidos = 0;
            }
        } else {
            vTaskDelayUntil(&siguiente, pdMS_TO_TICKS(PERIODO_FIFO_MS));
        }

        int64_t inicio = esp_timer_get_time();
        int n = mpu_fifo_read(imu, rafaga, MAX_RAFAGA);
        uint32_t lectura_us = (uint32_t)(esp_timer_get_time() - inicio);
        if (lectura_us > ci->lectura_max_us) ci->lectura_max_us = lectura_us;
        ci->lectura_suma_us += lectura_us;
        ci->lecturas++;
        if (n < 0) {
            // Desbordamiento (más de 42 ms sin leer): el FIFO se reinicia.
            ci->perdidas += 512 / 14;
            ci->publicado.errores_i2c++;
        }
        for (int i = 0; i < n; i++) {
            mpu_data_t m;
            mpu_convert(imu, &rafaga[i], &m);
            // Vigilante de alineación: la temperatura viaja en el FIFO entre el
            // accel y el giro y cambia muy despacio. Si el FIFO se corre aunque
            // sea 2 bytes, en su lugar aparece un trozo de otro eje y el salto
            // delata el desfase (el |a| no bastaba: con 2 bytes de desfase seguía
            // dando ~1 g y el giro recibía basura, medido: ángulos girando solos).
            int salto = (int)rafaga[i].temp - ci->temp_ref;
            if (salto > SALTO_TEMP_MAX || salto < -SALTO_TEMP_MAX) {
                ESP_LOGW(TAG, "FIFO desalineado (temperatura %d, esperada ~%d): reiniciando",
                         rafaga[i].temp, ci->temp_ref);
                mpu_fifo_reset(imu);
                ci->publicado.errores_i2c++;
                break;  // el resto de la ráfaga también está corrido
            }
            // La referencia sigue a la temperatura real (sube al calentarse el chip).
            ci->temp_ref += salto / 16;
            ci->publicado.temp_c = m.temp_c;
            procesar_muestra(ci, &m, dt);
        }
        cerrar_ciclo(ci, esp_timer_get_time());
    }
}

void tarea_imu(void *arg)
{
    mpu_handle_t imu = arg;
    static ciclo_imu_t ci;

    ESP_LOGI(TAG, "Calibrando giroscopio, no muevas el dron...");
    esp_err_t err = mpu_calibrate_gyro(imu, MUESTRAS_CALIBRACION);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "falló la calibración: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    // Temperatura inicial: referencia para comprobar la alineación del FIFO.
    mpu_raw_t crudo;
    if (mpu_read_raw(imu, &crudo) == ESP_OK) ci.temp_ref = crudo.temp;
    ci.t_segundo = esp_timer_get_time();

    // ¿Llega INT? Se esperan 10 muestras: con INT a 1 kHz tardan ~10 ms.
    bool int_ok = mpu_int_attach(imu, xTaskGetCurrentTaskHandle()) == ESP_OK;
    for (int i = 0; int_ok && i < 10; i++) {
        int_ok = mpu_wait_sample(pdMS_TO_TICKS(20)) > 0;
    }
    if (int_ok) {
        ESP_LOGI(TAG, "lazo a 1 kHz: INT + FIFO (núcleo %d)", xPortGetCoreID());
    } else {
        ESP_LOGW(TAG, "INT no llega (¿cable INT -> GPIO16?): lazo a 1 kHz leyendo el FIFO cada %d ms",
                 PERIODO_FIFO_MS);
    }
    lazo_imu(imu, &ci, int_ok);
}
