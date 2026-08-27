/*
 * ipradio_state.c — реализация конечного автомата.
 *
 * Устройство: одна задача, одна очередь, один мьютекс на снимок.
 * Все изменения состояния происходят ТОЛЬКО в этой задаче, поэтому
 * гонок между обработчиками событий нет по построению.
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_log.h"

#include "board_pins.h"
#include "ipradio_state.h"

static const char *TAG = "state";

#define EVENT_QUEUE_LEN     24
#define STATE_TASK_STACK    4096
#define STATE_TASK_PRIO     6
#define MAX_OBSERVERS       4

/* Событие во внутренней очереди: текст копируется, чтобы отправитель
 * мог освободить свой буфер сразу после вызова. */
typedef struct {
    ipradio_event_type_t type;
    int32_t              arg;
    char                 text[IPRADIO_NAME_MAX];
    bool                 has_text;
} queued_event_t;

static QueueHandle_t       s_queue;
static SemaphoreHandle_t   s_lock;
static ipradio_snapshot_t  s_state;

static struct {
    ipradio_observer_t cb;
    void              *ctx;
} s_observers[MAX_OBSERVERS];
static size_t s_observer_count;

static ipradio_event_filter_t s_filter;
static void                  *s_filter_ctx;

/* ------------------------------------------------------------------ *
 *  Громкость
 * ------------------------------------------------------------------ */

/* Поправка на режим (§6.3): уровни источников сведены железом, остаток
 * разницы добирается здесь, а не перепайкой. Значения подбираются
 * на макете; пока нейтральные. */
static const int8_t s_volume_trim[] = {
    [IPRADIO_MODE_FM]  = 0,
    [IPRADIO_MODE_NET] = 0,
};

static uint8_t clamp_volume(int32_t v)
{
    if (v < 0)   return 0;
    if (v > 100) return 100;
    return (uint8_t) v;
}

/* Перенос логического значения 0…100 на активный механизм.
 * Здесь только вычисление; собственно запись в тюнер или в микшер
 * делают модули fm_tuner и audio, читая снимок. */
uint8_t ipradio_volume_for_mode(uint8_t logical, ipradio_mode_t mode)
{
    int32_t v = (int32_t) logical + s_volume_trim[mode];
    return clamp_volume(v);
}

/* ------------------------------------------------------------------ *
 *  Обработка событий
 * ------------------------------------------------------------------ */

static void notify_observers(void)
{
    for (size_t i = 0; i < s_observer_count; i++) {
        s_observers[i].cb(&s_state, s_observers[i].ctx);
    }
}

/* Можно ли сейчас уйти в интернет-режим. Блокируется ВОСПРОИЗВЕДЕНИЕ,
 * а не переключение как таковое: реакция на MODE зависит от того,
 * почему сети нет (§5.2). Само решение принимает интерфейс, автомат
 * лишь сообщает факт. */
static bool net_playable(void)
{
    return s_state.net == IPRADIO_NET_CONNECTED;
}

static void apply_volume_delta(int32_t steps)
{
    /* Поворот энкодера снимает mute и сразу выставляет уровень (§6.3):
     * иначе пользователь крутит, ничего не происходит, и при снятии
     * mute получается неожиданно громко. */
    if (s_state.muted) {
        s_state.muted = false;
    }
    s_state.volume = clamp_volume((int32_t) s_state.volume + steps);
}

static void apply_mode_toggle(void)
{
    if (s_state.mode == IPRADIO_MODE_NET) {
        /* Обратно в эфир — всегда можно, звук появится сразу. */
        s_state.mode = IPRADIO_MODE_FM;
        s_state.play = IPRADIO_PLAY_PLAYING;
        return;
    }

    if (!net_playable()) {
        /* Переключение заблокировано. Эфир продолжает играть, а причину
         * покажет интерфейс — он читает поле net.
         *
         * Счётчик поднимаем здесь: без него интерфейс не отличит
         * «сети нет, и человек только что об этом спросил» от
         * «сети нет, и никого это не занимает». Диалог должен
         * появляться в ответ на нажатие, а не висеть сам по себе. */
        s_state.mode_denied_seq++;
        ESP_LOGI(TAG, "MODE: сети нет (%d), остаёмся в эфире", s_state.net);
        return;
    }

    s_state.mode = IPRADIO_MODE_NET;
    s_state.play = IPRADIO_PLAY_BUFFERING;
}

static void apply_event(const queued_event_t *e)
{
    switch (e->type) {

    case IPRADIO_EV_VOLUME_DELTA:
        apply_volume_delta(e->arg);
        break;

    case IPRADIO_EV_MUTE_TOGGLE:
        /* Mute — отдельный бит, а не громкость в ноль (§6.3).
         * Уровень при этом сохраняется. */
        s_state.muted = !s_state.muted;
        break;

    case IPRADIO_EV_MODE_TOGGLE:
        apply_mode_toggle();
        break;

    case IPRADIO_EV_NET_STATE:
        s_state.net = (ipradio_net_state_t) e->arg;
        break;

    case IPRADIO_EV_PLAY_STATE:
        s_state.play = (ipradio_play_state_t) e->arg;
        break;

    case IPRADIO_EV_SIGNAL_LEVEL:
        s_state.signal_level = (uint8_t) e->arg;
        break;

    case IPRADIO_EV_BUFFER_FILL:
        s_state.buffer_fill = (uint8_t) e->arg;
        break;

    case IPRADIO_EV_RDS_UPDATE:
        if (e->has_text) {
            strncpy(s_state.rds_name, e->text, IPRADIO_NAME_MAX - 1);
            s_state.rds_name[IPRADIO_NAME_MAX - 1] = '\0';
            s_state.rds_valid = (s_state.rds_name[0] != '\0');
        }
        break;

    case IPRADIO_EV_ICY_UPDATE:
        if (e->has_text) {
            strncpy(s_state.icy_title, e->text, IPRADIO_NAME_MAX - 1);
            s_state.icy_title[IPRADIO_NAME_MAX - 1] = '\0';
        }
        break;

    case IPRADIO_EV_TICK:
        /* Часы и ждущий режим — забота интерфейса; автомат только
         * будит его, чтобы перерисовать время. */
        break;

    default:
        /* Пресеты, перестройка, меню и питание появятся вместе
         * с модулями, которые их обслуживают. Пока молча пропускаем,
         * чтобы очередь не забивалась. */
        ESP_LOGD(TAG, "событие %d пока не обслуживается", e->type);
        break;
    }
}

static void state_task(void *arg)
{
    (void) arg;
    queued_event_t e;

    ESP_LOGI(TAG, "автомат запущен");

    for (;;) {
        if (xQueueReceive(s_queue, &e, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* Фильтр — до захвата замка и до применения события.
         * Он может забрать событие себе: пока висит диалог, энкодер 1
         * переставляет выделение в нём, а не перебирает станции. */
        if (s_filter) {
            ipradio_event_t ev = {
                .type     = e.type,
                .arg      = e.arg,
                .text     = e.has_text ? e.text : NULL,
            };
            if (s_filter(&ev, s_filter_ctx)) {
                continue;
            }
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        apply_event(&e);
        xSemaphoreGive(s_lock);

        notify_observers();
    }
}

/* ------------------------------------------------------------------ *
 *  Интерфейс
 * ------------------------------------------------------------------ */

esp_err_t ipradio_state_init(void)
{
    s_queue = xQueueCreate(EVENT_QUEUE_LEN, sizeof(queued_event_t));
    s_lock  = xSemaphoreCreateMutex();
    if (!s_queue || !s_lock) {
        return ESP_ERR_NO_MEM;
    }

    /* Состояние при старте. Настоящие значения приедут из хранилища,
     * когда появится модуль storage; пока — разумные умолчания. */
    memset(&s_state, 0, sizeof(s_state));
    s_state.mode          = IPRADIO_MODE_FM;
    s_state.band          = IPRADIO_BAND_CCIR;
    s_state.freq_khz      = 102500;
    s_state.volume        = 40;
    s_state.muted         = false;
    s_state.play          = IPRADIO_PLAY_IDLE;
    s_state.net           = IPRADIO_NET_NOT_CONFIGURED;
    s_state.active_preset = -1;

    BaseType_t ok = xTaskCreate(state_task, "ipradio_state",
                                STATE_TASK_STACK, NULL,
                                STATE_TASK_PRIO, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t ipradio_post(const ipradio_event_t *ev)
{
    if (!ev || !s_queue) {
        return ESP_ERR_INVALID_STATE;
    }

    queued_event_t e = {
        .type     = ev->type,
        .arg      = ev->arg,
        .has_text = false,
    };

    if (ev->text) {
        strncpy(e.text, ev->text, IPRADIO_NAME_MAX - 1);
        e.text[IPRADIO_NAME_MAX - 1] = '\0';
        e.has_text = true;
    }

    /* Не блокируемся: очередь переполнилась — значит автомат не успевает,
     * и лучше потерять событие, чем повесить отправителя. */
    return (xQueueSend(s_queue, &e, 0) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t ipradio_post_simple(ipradio_event_type_t type, int32_t arg)
{
    ipradio_event_t ev = { .type = type, .arg = arg, .text = NULL };
    return ipradio_post(&ev);
}

void ipradio_get(ipradio_snapshot_t *out)
{
    if (!out) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_state;
    xSemaphoreGive(s_lock);
}

esp_err_t ipradio_subscribe(ipradio_observer_t cb, void *ctx)
{
    if (!cb) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_observer_count >= MAX_OBSERVERS) {
        return ESP_ERR_NO_MEM;
    }
    s_observers[s_observer_count].cb  = cb;
    s_observers[s_observer_count].ctx = ctx;
    s_observer_count++;
    return ESP_OK;
}

void ipradio_set_event_filter(ipradio_event_filter_t cb, void *ctx)
{
    /* Порядок присваивания важен. Ставим: сначала контекст, потом
     * указатель — иначе задача автомата может вклиниться между ними
     * и позвать фильтр с чужим контекстом. Снимаем: сначала указатель.
     *
     * Замок здесь не нужен и был бы вреден: фильтр ставит задача
     * интерфейса, а зовёт задача автомата, и брать один замок на два
     * разных потока событий — прямая дорога к взаимной блокировке. */
    if (cb) {
        s_filter_ctx = ctx;
        s_filter     = cb;
    } else {
        s_filter     = NULL;
        s_filter_ctx = NULL;
    }
}
