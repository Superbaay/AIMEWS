idf_component_register(SRCS "tfminiplus.c" "tfminiplus_proto.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_uart esp_driver_gpio esp_timer)
