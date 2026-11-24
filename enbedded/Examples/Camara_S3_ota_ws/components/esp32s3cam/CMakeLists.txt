# Librería de cámara OV3660 para la ESP32-S3-WROOM-1 N16R8 CAM.
idf_component_register(
    SRCS "esp32s3cam.c" "esp32s3cam_modes.c"
    INCLUDE_DIRS "include"
    REQUIRES espressif__esp32-camera
    PRIV_REQUIRES log freertos esp_psram
)
