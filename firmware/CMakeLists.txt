# Корневой файл сборки IP-Radio.
# Собирается только под ESP32-P4: плата Waveshare ESP32-P4-WIFI6-Touch-LCD-5.

cmake_minimum_required(VERSION 3.16)

set(SUPPORTED_TARGETS esp32p4)

# Компоненты ESP-ADF: конвейер потока, кодеки, потоковые элементы.
# Сам ADF в сборку целиком не тянется - берём только его components.
# Требование esp_hosted ^2.6.4 живёт в манифестах ПРИМЕРОВ ADF
# и на нас не распространяется, см. docs/26-firmware-spec.md, §2.4.
if(DEFINED ENV{ADF_PATH})
    set(EXTRA_COMPONENT_DIRS $ENV{ADF_PATH}/components)
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ip-radio)
