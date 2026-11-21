/*
 * ============================================================================
 *  Dron_telemetria_ws — IMU 1 kHz + barómetro + GPS 5 Hz + LiDAR 100 Hz al panel
 *                       de control SAMATI por WebSocket
 * ============================================================================
 *
 *  Junta los ejemplos IMU_1khz, Barometro_HW291 y GPS_5hz en un solo
 *  firmware y manda la telemetría a la API (y de ahí al panel de control):
 *
 *      sensor         ritmo de lectura        tarea / núcleo
 *      ------         ----------------        --------------
 *      IMU            1000 Hz (INT + FIFO)    tarea_imu.c        / núcleo 1
 *      barómetro      ~89 Hz, OSR 4096        tarea_barometro.c  / núcleo 0
 *      GPS            5 Hz, 115200 baud       tarea_gps.c        / núcleo 0
 *      telemetría     10 msg/s (menuconfig)   enlace_api.c       / núcleo 0
 *
 *  El núcleo 1 queda SOLO para el lazo de 1 kHz: el WiFi, el TLS y los logs
 *  nunca lo retrasan. La telemetría manda la ÚLTIMA lectura de cada sensor;
 *  no tiene sentido mandar 1000 mensajes/s a una pantalla.
 *
 *  Conexiones (cableado real de la placa, comprobado con un diagnóstico
 *  que busca pull-ups, flancos y dispositivos I2C en todos los GPIO):
 *
 *      Módulo              Pin módulo   ESP32-S3
 *      ------              ----------   --------
 *      IMU MPU6050/6500    VCC / GND    3V3 / GND
 *                          SDA          GPIO12   (bus I2C compartido)
 *                          SCL          GPIO15   (bus I2C compartido)
 *                          INT          GPIO16   (opcional: sin él se usa el FIFO)
 *      Barómetro HW-291    VCC / GND    3V3 / GND
 *                          SDA          GPIO12   (mismo cable que el IMU)
 *                          SCL          GPIO15   (mismo cable que el IMU)
 *      GPS NEO-6M          VCC / GND    3V3 / GND
 *                          TX           GPIO18   (RX del ESP)  <- cruzados; si se
 *                          RX           GPIO17   (TX del ESP)     invierten, se detecta solo
 *      LiDAR TF Mini Plus  Rojo / Negro 5V (VBUS) / GND   (¡no 3V3!)
 *                          Verde (TX)   GPIO6    (RX del ESP, UART2)
 *                          Blanco (RX)  GPIO7    (TX del ESP; cruzados se detecta solo)
 *
 *  El IMU (0x68) y el barómetro (0x77) comparten bus: no chocan porque tienen
 *  direcciones distintas, y el driver I2C de ESP-IDF serializa las
 *  transacciones de las dos tareas. El barómetro solo ocupa el bus ~0.2 ms
 *  cada 10 ms (la conversión ocurre dentro del chip), así que la lectura del
 *  IMU (~450 us) cabe de sobra en su milisegundo.
 *
 *  Configurar WiFi, API y máquina:  copiar sdkconfig.defaults.local.ejemplo
 *  como sdkconfig.defaults.local (o idf.py menuconfig -> "Dron SAMATI").
 *  Compilar y flashear:   idf.py -p COM9 flash monitor   (salir: Ctrl+])
 * ============================================================================
 */

#include <stdio.h>
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "ota_dron.h"
#include "enlace_api.h"
#include "estado_dron.h"
#include "mpu6x00.h"
#include "ms5611.h"
#include "sensores.h"

static const char *TAG = "dron";

#define I2C_SDA_GPIO   12
#define I2C_SCL_GPIO   15
#define IMU_INT_GPIO   16

// Filtro paso bajo interno del IMU: 2 = 98 Hz / 2.8 ms, buen punto de partida para un dron.
#define DLPF_DRON      2

/* El driver I2C instala su interrupción en el núcleo que crea el bus. Si se
 * creara aquí (app_main, núcleo 0) cada transferencia del IMU esperaría a que
 * el WiFi/TLS suelte ese núcleo: medido, la lectura de 14 bytes pasaba de
 * ~450 us a ~1000 us, con atascos de 20 ms. Por eso se crea desde una tarea
 * breve fijada al núcleo 1, el mismo del lazo de vuelo. */
static i2c_master_bus_handle_t bus_creado;

static void tarea_crear_bus(void *aviso)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = 0,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,  // ayuda; los módulos traen los suyos
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &bus_creado));
    xTaskNotifyGive((TaskHandle_t)aviso);
    vTaskDelete(NULL);
}

static i2c_master_bus_handle_t crear_bus_i2c(void)
{
    xTaskCreatePinnedToCore(tarea_crear_bus, "crear_bus", 3072, xTaskGetCurrentTaskHandle(), 5, NULL,
                            NUCLEO_VUELO);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return bus_creado;
}

/* Una línea por segundo en el monitor serie (la tarea de menor prioridad). */
static void imprimir_estado(void)
{
    estado_dron_t e;
    estado_leer(&e);
    char lidar[48];
    if (!e.lidar.listo) {
        snprintf(lidar, sizeof(lidar), "sin sensor");
    } else if (!e.lidar.valido) {
        snprintf(lidar, sizeof(lidar), "%3.0f Hz sin medida (señal %u)", e.lidar.hz, e.lidar.fuerza);
    } else {
        snprintf(lidar, sizeof(lidar), "%3.0f Hz %5.2f m (señal %u)", e.lidar.hz, e.lidar.distancia_m,
                 e.lidar.fuerza);
    }
    printf("IMU %4.0f Hz (perd %lu, media %lu max %lu us) R%+6.1f P%+6.1f Y%+6.1f | "
           "BARO %4.1f Hz %7.2f hPa %+6.2f m | GPS %3.1f Hz %s %u sat | LIDAR %s | API %s\n",
           e.imu.hz, (unsigned long)e.imu.perdidas, (unsigned long)e.imu.lectura_media_us,
           (unsigned long)e.imu.lectura_max_us, e.imu.alabeo,
           e.imu.cabeceo, e.imu.guinada, e.baro.hz, e.baro.presion_hpa, e.baro.altura_m, e.gps.hz,
           e.gps.fix ? "fix" : (e.gps.listo ? "sin fix" : "sin GPS"), e.gps.satelites_usados, lidar,
           enlace_api_conectado() ? "conectada" : "desconectada");
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();  // el WiFi guarda su calibración en NVS
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // Si esta versión acaba de llegar por OTA, tiene 2 min para conectarse a la
    // API o el bootloader vuelve a la anterior (ota_dron.c).
    ota_revisar_arranque();

    // 1) Bus I2C compartido. El barómetro se inicia PRIMERO: si el bus quedó
    //    trabado tras flashear, su init lo recupera (9 pulsos de SCL) antes
    //    de que el IMU empiece a leer a 1 kHz.
    i2c_master_bus_handle_t bus = crear_bus_i2c();

    ms5611_config_t cfg_baro = MS5611_CONFIG_DEFAULT();  // 0x77, AUTO (el HW-291 es MS5607)
    cfg_baro.bus = bus;
    cfg_baro.osr_pressure = MS5611_OSR_4096;
    cfg_baro.osr_temperature = MS5611_OSR_1024;
    ms5611_handle_t baro = NULL;
    // Medido el 19-nov: tras flashear, a veces el bus arranca con ruido (el
    // escaneo veía direcciones fantasma 0x16, 0x41...) y el barómetro no
    // contesta al primer intento. Igual que con el IMU: liberar y reintentar.
    for (int intento = 1; intento <= 5; intento++) {
        err = ms5611_init(&cfg_baro, &baro);
        if (err == ESP_OK) break;
        ESP_LOGW(TAG, "barómetro no responde (intento %d/5): recuperando el bus", intento);
        i2c_master_bus_reset(bus);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "barómetro %s en 0x77",
                 ms5611_get_variant(baro) == MS5611_VARIANT_MS5607 ? "MS5607" : "MS5611");
        // Núcleo 0, pero con prioridad por encima de la pila de red (ver sensores.h).
        xTaskCreatePinnedToCore(tarea_barometro, "barometro", 4096, baro, PRIORIDAD_BAROMETRO, NULL,
                                NUCLEO_SISTEMA);
    } else {
        ESP_LOGE(TAG, "barómetro no responde (%s): revisa SDA=GPIO12, SCL=GPIO15", esp_err_to_name(err));
    }

    mpu_config_t cfg_imu = MPU_CONFIG_DEFAULT();  // 0x68, 400 kHz, 1000 Hz, ±4 g
    cfg_imu.bus = bus;
    // ±2000 °/s como los controladores de vuelo: con ±500 un giro brusco satura
    // el sensor (se vio 498 °/s al mover la placa) y el ángulo integrado se pierde.
    cfg_imu.gyro_fs = MPU_GYRO_2000;
    cfg_imu.int_gpio = IMU_INT_GPIO;
    cfg_imu.dlpf = DLPF_DRON;
    mpu_handle_t imu = NULL;
    // Justo después de flashear (reinicio a mitad de una lectura) el IMU a veces
    // no responde al primer intento: se libera el bus (9 pulsos de SCL) y se
    // reintenta, igual que hace el driver del barómetro.
    for (int intento = 1; intento <= 5; intento++) {
        err = mpu_init(&cfg_imu, &imu);
        if (err == ESP_OK) break;
        ESP_LOGW(TAG, "IMU no responde (intento %d/5): recuperando el bus", intento);
        i2c_master_bus_reset(bus);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "IMU %s (WHO_AM_I 0x%02X)", mpu_model(imu), mpu_who_am_i(imu));
        xTaskCreatePinnedToCore(tarea_imu, "imu", 4096, imu, PRIORIDAD_IMU, NULL, NUCLEO_VUELO);
    } else {
        ESP_LOGE(TAG, "IMU no responde (%s): revisa SDA=GPIO12, SCL=GPIO15", esp_err_to_name(err));
    }

    // 2) GPS (se inicia dentro de su tarea: autodetectar baudios tarda ~1-2 s).
    xTaskCreatePinnedToCore(tarea_gps, "gps", 4096, NULL, PRIORIDAD_GPS, NULL, NUCLEO_SISTEMA);

    // LiDAR TF Mini Plus en UART2 (GPIO7/6). Si no está conectado reintenta
    // cada 2 s sin frenar lo demás.
    xTaskCreatePinnedToCore(tarea_lidar, "lidar", 4096, NULL, PRIORIDAD_LIDAR, NULL, NUCLEO_SISTEMA);

    // 3) WiFi + API. Aunque falte algún sensor, se conecta y manda lo que haya.
    enlace_api_iniciar();

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        imprimir_estado();
    }
}
