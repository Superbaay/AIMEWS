# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

# Credenciales privadas (WiFi y contraseña de la máquina) fuera de git:
# si existe sdkconfig.defaults.local se aplica DESPUÉS de sdkconfig.defaults.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.local")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.local")
endif()

# Versión del firmware: viaja dentro del .bin (esp_app_desc_t). La API la lee al
# subir el archivo para OTA y el dron la reporta en estado_dron. Súbela en cada
# OTA (la API rechaza nada, pero así sabes qué corre cada dron).
set(PROJECT_VER "0.3.3")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(Dron_telemetria_ws)
