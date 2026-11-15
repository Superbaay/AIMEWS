idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES esp32cam net_wifi esp_timer esp_wifi esp_netif esp_event nvs_flash
                                  esp_http_client mbedtls espressif__esp_websocket_client)
