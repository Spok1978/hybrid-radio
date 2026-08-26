# Корневой файл сборки IP-Radio.
# Собирается только под ESP32-P4: плата Waveshare ESP32-P4-WIFI6-Touch-LCD-5.

cmake_minimum_required(VERSION 3.16)

set(SUPPORTED_TARGETS esp32p4)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ip-radio)
