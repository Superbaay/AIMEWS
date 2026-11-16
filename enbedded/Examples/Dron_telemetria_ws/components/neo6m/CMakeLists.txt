idf_component_register(SRCS "neo6m.c" "neo6m_nmea.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_uart esp_driver_gpio esp_timer)
