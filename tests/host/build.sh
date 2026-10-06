#!/bin/sh
# Собрать и прогнать проверки на машине разработчика.
#
# Компилятор — w64devkit (gcc для Windows) в C:\Users\telen\tools.
# Кросс-компилятор из ESP-IDF не годится: он собирает для платы.
#
# Флаги те же, что у прошивки: -std=gnu17 -Wall -Wextra. Смысл в том,
# чтобы код вёл себя одинаково в тесте и в прошивке.
#
# -static: иначе exe требует библиотек из каталога w64devkit
# и не запускается из другого места — оболочка отвечает кодом 127.
#
# IPRADIO_HOST_TEST открывает в автомате ipradio_state_pump():
# разбор очереди вручную, без задачи и планировщика.
set -e

GCC=${GCC:-/c/Users/telen/tools/w64devkit/bin/gcc.exe}
IDF=${IDF_PATH:-/c/Users/telen/esp/esp-adf/esp-idf}
MAIN=../../firmware/main
OUT=selftest.exe

"$GCC" -std=gnu17 -Wall -Wextra -Wno-unused-parameter -O1 -g -static \
    -DIPRADIO_HOST_TEST=1 \
    -I stubs \
    -I "$MAIN" \
    -I "$IDF/components/json/cJSON" \
    main.c \
    test_state.c \
    test_json_hooks.c \
    test_preset_edit.c \
    fakes/fake_storage.c \
    fakes/fake_deps.c \
    "$MAIN/ipradio_selftest.c" \
    "$MAIN/ipradio_preset_edit.c" \
    "$MAIN/ipradio_store_format.c" \
    "$MAIN/ipradio_state.c" \
    "$IDF/components/json/cJSON/cJSON.c" \
    -o "$OUT"

./"$OUT"
