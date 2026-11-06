#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define LED GPIO_NUM_2
static const char *TAG = "blink";

void app_main(void)
{
    gpio_reset_pin(LED);
    gpio_set_direction(LED, GPIO_MODE_OUTPUT);
    uint8_t estado = 0;
    uint32_t vueltas = 0;

    while (1) {
        estado = !estado;
        gpio_set_level(LED, estado);
        vueltas++;
        ESP_LOGI(TAG, "LED %s", estado ? "encendido" : "apagado");
        ESP_LOGD(TAG, "vuelta %lu", (unsigned long)vueltas);
        if (vueltas % 10 == 0) {
            ESP_LOGW(TAG, "ya van %lu cambios", (unsigned long)vueltas);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}