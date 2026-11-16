# Librería de conexión WiFi con reconexión automática.
idf_component_register(
    SRCS "net_wifi.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event
    PRIV_REQUIRES esp_timer log
)
