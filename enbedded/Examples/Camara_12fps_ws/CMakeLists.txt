# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

# Reutiliza la librería de cámara del ejemplo Camara_12fps sin copiarla:
# un arreglo en components/esp32cam sirve para los dos proyectos.
#
# El WiFi usa net_wifi de la librería esp32-https-ws (probada en esta misma
# placa): reconexión con backoff, WPA2/WPA3 y registra el motivo de cada caída.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../Camara_12fps/components"
    "${CMAKE_CURRENT_LIST_DIR}/../../../../esp32-https-ws/firmware/components/net_wifi")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(Camara_12fps_ws)
