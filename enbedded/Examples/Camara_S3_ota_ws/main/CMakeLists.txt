idf_component_register(SRCS "main.c" "enlace_api.c" "ota_camara.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp32s3cam net_wifi esp_wifi esp_http_client esp_https_ota
                                     app_update esp_app_format nvs_flash esp_timer mbedtls log)
