idf_component_register(SRCS "mpu6x00.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_i2c esp_driver_gpio)
