/*
 * app_main.c — проверочная сборка IP-Radio.
 *
 * Это ещё не прошивка приёмника. Задача этого файла — доказать, что
 * инструментарий собран правильно, и заодно снять с платы те сведения,
 * которые нельзя получить по документации:
 *
 *   1. ревизию кристалла ESP32-P4 — от неё зависят настройки SDK
 *      (docs/26-firmware-spec.md, §11, пункт 8);
 *   2. фактический объём PSRAM — в неё лягут буфер потока и рабочая
 *      память декодера;
 *   3. кто реально отвечает на шине I²C — проверка адресов тюнера
 *      и расширителя портов, когда они будут подключены.
 *
 * ВНИМАНИЕ: на момент написания (2026-08-27) инструментарий ещё
 * скачивался, и этот файл ни разу не компилировался. Считать его
 * черновиком до первой успешной сборки.
 */

#include <stdio.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_system.h"

#include "driver/i2c_master.h"

#include "board_pins.h"
#include "ipradio_state.h"
#include "ipradio_input.h"
#include "ipradio_storage.h"
#include "ipradio_tuner.h"
#include "ipradio_audio.h"
#include "ipradio_net.h"
#include "ipradio_ui.h"
#include "ipradio_netradio.h"

static const char *TAG = "ip-radio";

/* Общая шина платы: её открывает опрос и дальше пользуются все. */
static i2c_master_bus_handle_t s_i2c_bus;

/* Кто должен отзываться на шине: штатные узлы платы плюс наши. */
static const struct {
    uint8_t addr;
    const char *name;
} known_devices[] = {
    { 0x14, "GT911 (тач, вариант адреса)" },
    { 0x18, "ES8311 (кодек платы)" },
    { I2C_ADDR_TUNER_SEQ,    "RDA5807M, последовательный доступ" },
    { I2C_ADDR_TUNER_RANDOM, "RDA5807M, доступ к регистру" },
    { I2C_ADDR_EXPANDER,     "MCP23017 (расширитель портов)" },
    { 0x40, "ES7210 (микрофонный кодек платы)" },
    { 0x5D, "GT911 (тач, основной адрес)" },
};

static void log_chip(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);

    /* В ESP-IDF 5.x поле revision кодируется как major * 100 + minor. */
    ESP_LOGI(TAG, "кристалл: %d ядер, ревизия v%d.%d",
             info.cores, info.revision / 100, info.revision % 100);
    ESP_LOGI(TAG, "  ревизия важна: свежие ESP-IDF по умолчанию поддерживают");
    ESP_LOGI(TAG, "  только 3.1 и выше; для 1.x нужны особые настройки SDK");

    ESP_LOGI(TAG, "свободная куча: %" PRIu32 " байт", esp_get_free_heap_size());

#if CONFIG_SPIRAM
    if (esp_psram_is_initialized()) {
        ESP_LOGI(TAG, "PSRAM: %u байт", (unsigned) esp_psram_get_size());
    } else {
        ESP_LOGW(TAG, "PSRAM не инициализирована — проверить настройки");
    }
#else
    ESP_LOGW(TAG, "PSRAM выключена в конфигурации; для потока она нужна");
#endif
}

static void scan_i2c(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,                     /* любой свободный порт */
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        /* Подтяжки 2,2 кОм к 3,3 В уже стоят на плате (R49, R50),
         * внутренние включать не нужно. */
        .flags.enable_internal_pullup = false,
    };

    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "шина I2C не поднялась: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "опрос шины I2C (SDA=GPIO%d, SCL=GPIO%d):",
             PIN_I2C_SDA, PIN_I2C_SCL);

    int found = 0;
    for (int addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(bus, addr, 50) != ESP_OK) {
            continue;
        }
        found++;

        const char *name = "неизвестное устройство";
        for (size_t i = 0; i < sizeof(known_devices) / sizeof(known_devices[0]); i++) {
            if (known_devices[i].addr == addr) {
                name = known_devices[i].name;
                break;
            }
        }
        ESP_LOGI(TAG, "  0x%02X — %s", addr, name);
    }

    if (found == 0) {
        ESP_LOGW(TAG, "  никто не ответил — проверить питание и провода");
    }

    /* Шину НЕ закрываем: её делят тюнер, тач и кодеки платы.
     * Отдаём наружу, чтобы тюнер сел на неё же. */
    s_i2c_bus = bus;
}

/* Мост между автоматом и железом. Вызывается автоматом после каждого
 * применённого события. */
static void on_state_changed(const ipradio_snapshot_t *snap, void *ctx)
{
    (void) ctx;
    ipradio_audio_apply(snap);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== IP-Radio, проверочная сборка ===");

    log_chip();
    scan_i2c();

    /* Конечный автомат — единственный владелец состояния (§3).
     * Поднимается раньше всех модулей: они будут слать ему события. */
    /* Хранилище поднимается ПЕРЕД автоматом: автомат должен стартовать
     * с последнего сохранённого состояния, а не с умолчаний.
     * Отсутствие карты не фатально — прибор поднимется без неё. */
    ipradio_storage_init();

    ESP_ERROR_CHECK(ipradio_state_init());

    /* Органы управления. Поднимаются после автомата: сразу начнут
     * слать ему события от человека. */
    ESP_ERROR_CHECK(ipradio_input_init());

    /* Эфирный тюнер садится на ту же шину I²C, что и узлы платы.
     * Его отсутствие не фатально: прибор останется интернет-радио. */
    ipradio_tuner_init(s_i2c_bus);

    /* Звуковой тракт: I²S на свой ЦАП, линия приглушения усилителя.
     * Штатный усилитель платы при этом удерживается выключенным. */
    ESP_ERROR_CHECK(ipradio_audio_init());

    /* Замыкаем круг: любое изменение состояния сразу применяется
     * к железу. Автомат зовёт это из своей задачи, поэтому
     * обработчик обязан быть коротким — он и есть короткий. */
    ipradio_subscribe(on_state_changed, NULL);

    /* Сеть поднимается ПОСЛЕДНЕЙ и не блокирует: эфир не должен ждать
     * сеть (§3.2). Тюнер к этому моменту уже играет. */
    /* Интерфейс. Панель и тач поднимает BSP платы; здесь только
     * экраны. Подписку на автомат модуль делает сам. */
    ipradio_ui_init();

    ipradio_net_init();
    ipradio_net_start_sntp("MSK-3");

    /* Конвейер интернет-радио. Поднимается один раз и остаётся
     * поднятым: возврат из эфира не должен стоить лишних секунд. */
    ipradio_netradio_init();

    {   /* Применить то, что уже прочитано из хранилища. */
        ipradio_snapshot_t snap;
        ipradio_get(&snap);
        ipradio_audio_apply(&snap);
    }

    ESP_LOGI(TAG, "готово; дальше — проверка связки ADF с сетью через C6");

    /* Раз в секунду будим автомат: по этому событию интерфейс
     * перерисовывает часы и отсчитывает вход в ждущий режим. */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        ipradio_post_simple(IPRADIO_EV_TICK, 0);
    }
}
