# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

# Credenciales privadas (WiFi de casa, hotspot del teléfono y contraseña de la
# máquina) fuera de git: si existe sdkconfig.defaults.local se aplica DESPUÉS.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.local")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.local")
endif()

# Versión del firmware: viaja dentro del .bin (esp_app_desc_t). La API la lee al
# subir el archivo para OTA y la cámara la reporta en estado_dron. Súbela en cada
# OTA: ota_camara.c rechaza un .bin cuya versión no sea la que anunció la API.
set(PROJECT_VER "1.1.4")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(Camara_S3_ota_ws)
