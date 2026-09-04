/*
 * ipradio_bridge.c — перенос состояния на железо.
 *
 * Замысел — в ipradio_bridge.h.
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"

#include "bsp/display.h"

#include "board_pins.h"
#include "ipradio_audio.h"
#include "ipradio_bridge.h"
#include "ipradio_netradio.h"
#include "ipradio_state.h"
#include "ipradio_storage.h"
#include "ipradio_tuner.h"

static const char *TAG = "bridge";

/* Что уже применено к железу. Начальные значения намеренно
 * невозможные: первое же сравнение должно дать расхождение
 * и всё выставить. */
static ipradio_mode_t  s_applied_mode  = (ipradio_mode_t) -1;
static ipradio_band_t  s_applied_band  = (ipradio_band_t) -1;
static uint32_t        s_applied_freq;
static int8_t          s_applied_preset = -2;

/* Ячейка, которую конвейер уже объявил не отвечающей. Пока в ней
 * стоим, повторных запусков не делаем. */
static int8_t          s_dead_preset = -2;
static bool            s_powering_off;

/* Правило 4 из §5.2: если последняя станция была интернетная, а сеть
 * за пятнадцать секунд не поднялась - играем последнюю эфирную
 * и говорим об этом.
 *
 * Радио, молчащее после включения, выглядит неисправным. Поэтому
 * решение принимается САМО, но однократно: самовольно переключаться
 * обратно, когда сеть появится, нельзя - решение уже показано
 * человеку, и менять его за него мы не вправе. */
#define NET_WAIT_AT_BOOT_MS  15000

static int64_t s_boot_us;
static bool    s_boot_decided;
static bool    s_wanted_net_at_boot;

/* ------------------------------------------------------------------ *
 *  Эфир
 * ------------------------------------------------------------------ */

static void apply_tuner(const ipradio_snapshot_t *s)
{
    if (!ipradio_tuner_present()) {
        return;
    }

    /* Диапазон переключаем первым: смена диапазона перепрограммирует
     * чип целиком, и выставленная до неё частота потерялась бы. */
    if (s->band != s_applied_band) {
        ipradio_tuner_set_band(s->band);
        s_applied_band = s->band;
        s_applied_freq = 0;          /* частоту придётся выставить заново */
    }

    if (s->freq_khz != s_applied_freq) {
        ipradio_tuner_set_freq(s->freq_khz);
        s_applied_freq = s->freq_khz;
    }
}

/* ------------------------------------------------------------------ *
 *  Интернет-поток
 * ------------------------------------------------------------------ */

/* Адрес потока живёт в ячейке пресета, а не в снимке состояния.
 * Класть его в снимок не стали: снимок копируется на каждое событие,
 * и таскать за собой 192 байта адреса ради редкого случая незачем. */
static bool preset_url(int8_t cell, char *url, size_t url_len,
                       char *name, size_t name_len)
{
    if (cell < 1 || cell > IPRADIO_PRESET_MAX) {
        return false;
    }

    /* Одна ячейка вместо всего банка: этот код исполняется в задаче
     * автомата, а её стек полной копии не выдерживает - на этом
     * прибор и падал. */
    ipradio_preset_t p;
    if (!ipradio_storage_get_preset((int) cell, &p) ||
        p.type != IPRADIO_MODE_NET || !p.url[0]) {
        return false;
    }

    snprintf(url, url_len, "%s", p.url);
    snprintf(name, name_len, "%s", p.name);
    return true;
}

static void apply_stream(const ipradio_snapshot_t *s)
{
    bool want_net = (s->mode == IPRADIO_MODE_NET);

    if (!want_net) {
        /* Ушли в эфир. Поток останавливаем, но конвейер не разбираем:
         * §5.2 требует, чтобы возврат в интернет не стоил лишних
         * секунд, а разбор и повторная сборка стоили бы. */
        if (ipradio_netradio_active()) {
            ipradio_netradio_stop();
        }
        return;
    }

    /* В интернет-режиме перезапускаем поток только при смене станции.
     * Сверять по адресу было бы надёжнее, но номер ячейки меняется
     * ровно тогда же, а хранить копию адреса ради этого не стоит. */
    if (s->active_preset == s_applied_preset && ipradio_netradio_active()) {
        return;
    }

    /* Станция уже признана не отвечающей - НЕ дёргаем её снова.
     *
     * Без этой проверки получался вечный цикл: конвейер после трёх
     * неудачных попыток выставляет ERROR и гасит себя, а сюда мы
     * попадаем на каждом событии автомата, включая секундный тик.
     * Поток неактивен, ячейка та же - условие выше не срабатывает,
     * и всё начиналось заново. Прибор молотил бы в сеть без конца,
     * показывая при этом «станция не отвечает».
     *
     * Запрет снимается сменой ячейки или режима: и то и другое
     * меняет s_applied_preset или проводит нас через ветку выше. */
    if (s->play == IPRADIO_PLAY_ERROR &&
        s->active_preset == s_dead_preset) {
        return;
    }
    s_dead_preset = -2;

    char url[IPRADIO_URL_MAX];
    char name[IPRADIO_NAME_MAX];

    if (!preset_url(s->active_preset, url, sizeof(url), name, sizeof(name))) {
        /* Ячейка интернетная, а адреса в ней нет. Пока это норма:
         * записывать адрес умеет только поиск по каталогу, а его
         * ещё нет. Молчать об этом нельзя — иначе выглядит как
         * поломка звука. */
        ESP_LOGW(TAG, "у ячейки %d нет адреса потока", s->active_preset);
        s_dead_preset = s->active_preset;
        ipradio_post_simple(IPRADIO_EV_PLAY_STATE, IPRADIO_PLAY_ERROR);
        return;
    }

    /* Запомним, на чём стоим: если конвейер сообщит об отказе,
     * запрет ляжет именно на эту ячейку. */
    s_dead_preset = s->active_preset;
    if (ipradio_netradio_play(url, name) != ESP_OK) {
        return;
    }
    s_dead_preset = -2;   /* пошло - запрета нет */
}

/* ------------------------------------------------------------------ *
 *  Выключение
 * ------------------------------------------------------------------ */

/* Уход в глубокий сон. Порядок важен и потому расписан по шагам:
 * сначала снимаем звук, потом свет, и только затем засыпаем. Обратный
 * порядок дал бы щелчок в динамике и вспышку подсветки. */
static void power_off_task(void *arg)
{
    (void) arg;

    ESP_LOGI(TAG, "выключение");

    ipradio_netradio_stop();
    ipradio_tuner_set_mute(true);
    ipradio_tuner_set_hiz(true);
    ipradio_audio_amp_enable(false);

    bsp_display_backlight_off();

    /* Пауза на затухание: усилитель выключается не мгновенно,
     * и без неё в динамике слышен хлопок. */
    vTaskDelay(pdMS_TO_TICKS(120));

    /* Просыпаемся по той же кнопке питания. Она сидит на GPIO3,
     * а это домен LP — только его выводы умеют будить кристалл
     * (docs/26-firmware-spec.md, §2.7). Кнопка замыкает на землю,
     * поэтому пробуждение по низкому уровню. */
    esp_deep_sleep_enable_gpio_wakeup(1ULL << PIN_POWER_BUTTON,
                                      ESP_GPIO_WAKEUP_GPIO_LOW);

    esp_deep_sleep_start();   /* не возвращается */
}

/* ------------------------------------------------------------------ *
 *  Точка входа
 * ------------------------------------------------------------------ */

static void on_state(const ipradio_snapshot_t *s, void *ctx)
{
    (void) ctx;

    if (s->power_off) {
        /* Сон нельзя запускать из задачи автомата: он не вернётся,
         * а очередь событий останется с ним. Отдельная задача, и та
         * запускается один раз. */
        if (!s_powering_off) {
            s_powering_off = true;
            xTaskCreate(power_off_task, "ipradio_off", 3072, NULL, 5, NULL);
        }
        return;
    }

    apply_tuner(s);
    apply_stream(s);

    /* Звук последним: он выставляет mute и hi-Z по режиму, и делать
     * это надо после того, как источник уже перестроен. Иначе
     * на долю секунды слышна прежняя станция. */
    ipradio_audio_apply(s);

    s_applied_mode   = s->mode;
    s_applied_preset = s->active_preset;
}

/* Разовая проверка при старте, зовётся по секундному тику.
 * Возвращает true, когда решение принято и звать больше не надо. */
bool ipradio_bridge_boot_check(void)
{
    if (s_boot_decided) {
        return true;
    }

    /* Последняя станция была эфирной - решать нечего, эфир уже играет. */
    if (!s_wanted_net_at_boot) {
        s_boot_decided = true;
        return true;
    }

    ipradio_snapshot_t s;
    ipradio_get(&s);

    if (s.net == IPRADIO_NET_CONNECTED) {
        /* Сеть успела. Возвращаем человека туда, где он выключился:
         * это не самоуправство, а восстановление его же выбора. */
        s_boot_decided = true;
        ESP_LOGI(TAG, "сеть поднялась, возвращаем интернет-станцию");

        ipradio_store_t store;
        ipradio_storage_get(&store);
        if (store.settings.last_preset >= 1) {
            ipradio_post_simple(IPRADIO_EV_PRESET_PRESSED,
                                store.settings.last_preset);
        }
        return true;
    }

    if ((esp_timer_get_time() - s_boot_us) < NET_WAIT_AT_BOOT_MS * 1000LL) {
        return false;                /* ещё ждём */
    }

    /* Не дождались. Эфир уже играет - автомат стартует в эфирном
     * режиме именно на этот случай. Остаётся сказать об этом. */
    s_boot_decided = true;
    ESP_LOGW(TAG, "сеть за %d с не поднялась, играет эфир",
             NET_WAIT_AT_BOOT_MS / 1000);

    ipradio_event_t ev = {
        .type = IPRADIO_EV_ICY_UPDATE,
        .text = "интернет недоступен, играет эфир",
    };
    ipradio_post(&ev);
    return true;
}

esp_err_t ipradio_bridge_init(void)
{
    s_boot_us = esp_timer_get_time();

    /* Чего человек хотел, когда выключался. Читаем ДО подписки:
     * дальше состояние начнёт меняться. */
    {
        ipradio_settings_t set;
        ipradio_storage_get_settings(&set);
        s_wanted_net_at_boot = (set.last_mode == IPRADIO_MODE_NET);
    }

    esp_err_t err = ipradio_subscribe(on_state, NULL);
    if (err != ESP_OK) {
        return err;
    }

    /* Применить то, что уже прочитано из хранилища при старте:
     * подписка сработает только на следующее событие, а прибор
     * должен зазвучать сразу. */
    ipradio_snapshot_t snap;
    ipradio_get(&snap);
    on_state(&snap, NULL);

    ESP_LOGI(TAG, "мост поднят: состояние переносится на железо");
    return ESP_OK;
}
