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
static bool            s_powering_off;

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

    ipradio_store_t store;
    ipradio_storage_get(&store);

    const ipradio_preset_t *p = &store.presets[cell - 1];
    if (!p->used || p->type != IPRADIO_MODE_NET || !p->url[0]) {
        return false;
    }

    strncpy(url, p->url, url_len - 1);
    url[url_len - 1] = '\0';
    strncpy(name, p->name, name_len - 1);
    name[name_len - 1] = '\0';
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

    char url[IPRADIO_URL_MAX];
    char name[IPRADIO_NAME_MAX];

    if (!preset_url(s->active_preset, url, sizeof(url), name, sizeof(name))) {
        /* Ячейка интернетная, а адреса в ней нет. Пока это норма:
         * записывать адрес умеет только поиск по каталогу, а его
         * ещё нет. Молчать об этом нельзя — иначе выглядит как
         * поломка звука. */
        ESP_LOGW(TAG, "у ячейки %d нет адреса потока", s->active_preset);
        ipradio_post_simple(IPRADIO_EV_PLAY_STATE, IPRADIO_PLAY_ERROR);
        return;
    }

    ipradio_netradio_play(url, name);
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

esp_err_t ipradio_bridge_init(void)
{
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
