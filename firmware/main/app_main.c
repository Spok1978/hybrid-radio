/*
 * app_main.c — точка входа IP-Radio.
 *
 * Порядок подъёма задан в docs/26-firmware-spec.md, §3.2, и он не
 * произвольный:
 *
 *   1. хранилище — раньше автомата: автомат должен стартовать
 *      с последнего сохранённого состояния, а не с умолчаний;
 *   2. автомат — единственный владелец состояния (§3);
 *   3. органы управления — сразу начнут слать ему события;
 *   4. тюнер и звуковой тракт;
 *   5. интерфейс;
 *   6. сеть — последней и не блокируя: эфир не должен ждать сеть;
 *   7. мост — когда всё железо уже поднято.
 *
 * Отсутствие любой из внешних частей не фатально. Нет карты — прибор
 * поднимется с умолчаниями. Нет тюнера — останется интернет-радио.
 * Нет панели — радио играет вслепую, органы работают. Приёмник,
 * который не включается из-за одной неисправной части, сломан целиком;
 * приёмник, который включается и говорит, чего лишился, — нет.
 *
 * Заодно при старте в журнал уходит то, что нельзя узнать
 * по документации: ревизия кристалла, фактический объём PSRAM
 * и список ответивших на шине I²C.
 */

#include <stdio.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_chip_info.h"
#include <stdlib.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_system.h"

#include "driver/i2c_master.h"

#include "board_pins.h"
#include "ipradio_state.h"
#include "ipradio_input.h"
#include "ipradio_tuner.h"
#include "ipradio_audio.h"
#include "ipradio_net.h"
#include "ipradio_ui.h"
#include "bsp/esp-bsp.h"

#include "ipradio_board_codec.h"
#include "ipradio_netradio.h"
#include "ipradio_bridge.h"
#include "ipradio_watchdog.h"
#include "ipradio_storage.h"

#include "bsp/display.h"

/* Показ экрана при старте — для проверок на столе.
 *
 * 0 — обычный запуск, 1 — через две секунды само открывается меню
 * настроек. Заведено, чтобы посмотреть экраны на живой панели, пока
 * не подключены регуляторы: без них до меню не добраться никак,
 * оно открывается только долгим нажатием регулятора 1.
 *
 * Открывается ОБЫЧНЫМ событием, тем же, что шлёт настоящее нажатие:
 * экран настоящий и путь настоящий, ничего не нарисовано отдельно
 * ради показа.
 *
 * В рабочей сборке ноль. */
#define IPRADIO_DEMO_SCREEN  0

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
    /* Шину НЕ создаём свою, а берём у BSP платы.
     *
     * Раньше здесь стоял i2c_new_master_bus с портом -1, то есть
     * «любой свободный». Он занимал порт 0, а BSP для касания
     * поднимал свою шину на порту 1 (CONFIG_BSP_I2C_NUM=1) - и обе
     * висели на одних и тех же выводах 7 и 8. Два контроллера,
     * дёргающие одни ноги, шину ломают. В журнале это выглядело так:
     *
     *     W i2c.common: GPIO 7 is not usable, maybe conflict with others
     *     E i2c.master: clear bus failed
     *     E i2c.master: reset hardware failed
     *
     * Записи кое-как проходили, а чтение возвращало нули: дамп
     * регистров кодека ES8311 выдал 00, 00, 00 и оборвался на
     * третьем регистре. Отсюда и отсутствие звука - кодек не был
     * настроен на самом деле, хотя вызовы возвращали успех.
     *
     * Правильно - одна шина на всех: касание, тюнер RDA5807M,
     * кодек ES8311 и расширитель выводов сидят на одних выводах,
     * значит и контроллер у них должен быть один. */
    esp_err_t err = bsp_i2c_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "шина I2C платы не поднялась: %s", esp_err_to_name(err));
        return;
    }

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        ESP_LOGE(TAG, "BSP не отдал ручку шины I2C");
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

void app_main(void)
{
    ESP_LOGI(TAG, "=== IP-Radio, проверочная сборка ===");

    ESP_ERROR_CHECK(ipradio_watchdog_init());

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
    /* Без ESP_ERROR_CHECK намеренно: отказ линии приглушения или
     * штатного усилителя не повод не включаться. Это тот же принцип,
     * что объявлен в шапке файла, - раньше он здесь нарушался. */
    if (ipradio_audio_init() != ESP_OK) {
        ESP_LOGE(TAG, "звуковой тракт не поднялся, звука не будет");
    }

    /* Интерфейс. Панель и тач поднимает BSP платы; здесь только
     * экраны. Подписку на автомат модуль делает сам. */
    ipradio_ui_init();

    /* Сеть поднимается ПОСЛЕДНЕЙ и не блокирует: эфир не должен ждать
     * сеть (§3.2). Тюнер к этому моменту уже играет. */
    ipradio_net_init();

    {
        /* Яркость и часовой пояс лежали в файле настроек и никем
         * не применялись: прибор всегда включался с яркостью по
         * умолчанию и московским временем, что бы человек ни выставил.
         *
         * Яркость применяем ПОСЛЕ подъёма интерфейса: до него ШИМ
         * подсветки ещё не настроен, и запись ушла бы в никуда. */
        ipradio_store_t st;
        ipradio_storage_get(&st);

        if (st.settings.brightness > 0) {
            bsp_display_brightness_set(st.settings.brightness);
        }

        ipradio_net_start_sntp(st.settings.tz[0] ? st.settings.tz : "MSK-3");
    }

    /* Конвейер интернет-радио. Поднимается один раз и остаётся
     * поднятым: возврат из эфира не должен стоить лишних секунд. */
#if IPRADIO_USE_BOARD_CODEC
    /* ВРЕМЕННО: до кодека платы, потому что конвейер сразу за ним
     * поднимает I²S, а микросхеме надо успеть настроиться. */
    ipradio_board_codec_init(s_i2c_bus);
#endif

    ipradio_netradio_init();

    /* Мост — последним: он начнёт переносить состояние на железо
     * сразу, поэтому всё железо к этому моменту должно быть поднято.
     * Он же применит то, что прочитано из хранилища, и прибор
     * зазвучит, не дожидаясь первого нажатия. */
    ESP_ERROR_CHECK(ipradio_bridge_init());

    ESP_LOGI(TAG, "прибор поднят");

#if IPRADIO_DEMO_SCREEN
    /* Открываем меню тем же событием, каким его открывает долгое
     * нажатие регулятора 1: экран настоящий, путь настоящий. */
    vTaskDelay(pdMS_TO_TICKS(2000));
    ipradio_post_simple(IPRADIO_EV_MENU, 0);
    ESP_LOGW(TAG, "ПОКАЗ: открыто меню настроек");
#endif

    /* Раз в секунду будим автомат: по этому событию интерфейс
     * перерисовывает часы и отсчитывает вход в ждущий режим. */
    bool boot_done = false;

    unsigned beat = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        ipradio_post_simple(IPRADIO_EV_TICK, 0);

        /* ВРЕМЕННО: пульс главной задачи. Нужен, чтобы отличить
         * «встала вся система» от «встал только интерфейс».
         * УБРАТЬ после разбора зависания. */
        if (++beat % 3 == 0) {
            ESP_LOGW(TAG, "ПУЛЬС main: %u с, внутренней памяти %u КБ, "
                          "наибольший кусок %u КБ", beat,
                     (unsigned) (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned) (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
        }

        /* Решение о том, ждать ли сеть после включения (§5.2,
         * правило 4). Принимается один раз, дальше не мешаем. */
        if (!boot_done) {
            boot_done = ipradio_bridge_boot_check();
        }
    }
}
