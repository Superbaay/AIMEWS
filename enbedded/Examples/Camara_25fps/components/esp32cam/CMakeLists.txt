# Librería de cámara para la AI-Thinker ESP32-CAM.
idf_component_register(
    SRCS "esp32cam.c"
    INCLUDE_DIRS "include"
    REQUIRES espressif__esp32-camera
    PRIV_REQUIRES esp_driver_ledc esp_driver_gpio log freertos esp_psram
)
