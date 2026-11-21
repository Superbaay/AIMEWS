idf_component_register(SRCS "main.c" "estado_dron.c" "tarea_imu.c" "tarea_barometro.c" "tarea_gps.c"
                            "enlace_api.c" "tarea_lidar.c" "ota_dron.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES mpu6x00 ms5611 neo6m tfminiplus net_wifi esp_driver_i2c esp_driver_gpio esp_driver_uart esp_timer esp_wifi
                                  esp_netif esp_event nvs_flash esp_http_client mbedtls
                                  espressif__esp_websocket_client esp_https_ota app_update esp_app_format)
