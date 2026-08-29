/*
 * ipradio_state.c — реализация конечного автомата.
 *
 * Устройство: одна задача, одна очередь, один мьютекс на снимок.
 * Все изменения состояния происходят ТОЛЬКО в этой задаче, поэтому
 * гонок между обработчиками событий нет по построению.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_log.h"

#include "board_pins.h"
#include "ipradio_state.h"
#include "ipradio_storage.h"
#include "ipradio_watchdog.h"
#include "ipradio_tuner.h"

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

/* ------------------------------------------------------------------ *
 *  Пресеты
 * ------------------------------------------------------------------ */

/* Перенести содержимое ячейки в состояние.
 *
 * Тип хранится в самой ячейке, поэтому нажатие пресета САМО переключает
 * режим (docs/22-mode-switching.md). Из-за этого и отпал тумблер:
 * переключать было нечего — режим следует за станцией. */
static bool load_preset(const ipradio_preset_t *p, int8_t cell)
{
    if (!p->used) {
        return false;
    }

    if (p->type == IPRADIO_MODE_NET && !net_playable()) {
        /* Интернетная ячейка при недоступной сети. Ведём себя так же,
         * как с кнопкой MODE: эфир не прерываем, а причину покажет
         * интерфейс. По §5.2 такие ячейки и гаснуть должны заранее,
         * но нажатие всё равно надо обработать — кнопка физическая,
         * погасить её невозможно. */
        s_state.mode_denied_seq++;
        ESP_LOGI(TAG, "пресет %d интернетный, а сети нет", cell);
        return false;
    }

    s_state.mode          = p->type;
    s_state.active_preset = cell;

    if (p->type == IPRADIO_MODE_FM) {
        s_state.band     = p->band;
        s_state.freq_khz = p->freq_khz;
        s_state.play     = IPRADIO_PLAY_PLAYING;

        /* Имя из ячейки кладём в RDS-поле только как заглушку до
         * первого настоящего RDS: иначе экран остался бы пустым
         * на те секунды, что чип ловит синхронизацию. Признак
         * rds_valid при этом НЕ ставим — это не данные из эфира. */
        snprintf(s_state.rds_name, sizeof(s_state.rds_name), "%s", p->name);
        s_state.rds_valid = false;
    } else {
        snprintf(s_state.station_name, sizeof(s_state.station_name),
                 "%s", p->name);
        s_state.icy_title[0] = '\0';
        s_state.play = IPRADIO_PLAY_BUFFERING;
    }

    return true;
}

static void apply_preset_pressed(int32_t cell)
{
    if (cell < 1 || cell > IPRADIO_PRESET_MAX) {
        return;
    }

    ipradio_store_t store;
    ipradio_storage_get(&store);
    load_preset(&store.presets[cell - 1], (int8_t) cell);
}

/* Долгое нажатие — записать в ячейку то, что играет сейчас (§6.2). */
static void apply_preset_hold(int32_t cell)
{
    if (cell < 1 || cell > IPRADIO_PRESET_MAX) {
        return;
    }

    ipradio_store_t store;
    ipradio_storage_get(&store);

    /* Адрес потока копируем В ЛОКАЛЬНЫЕ буферы до очистки ячейки.
     * Источник и приёмник лежат в одной структуре store, и прямое
     * копирование между ними - перекрытие: компилятор справедливо
     * на это ругается. Плюс писать могут поверх той самой ячейки,
     * что сейчас играет. */
    char src_url[IPRADIO_URL_MAX]  = { 0 };
    char src_uuid[IPRADIO_UUID_MAX] = { 0 };

    const int8_t src = s_state.active_preset;
    if (src >= 1 && src <= IPRADIO_PRESET_MAX) {
        const ipradio_preset_t *from = &store.presets[src - 1];
        if (from->used && from->type == IPRADIO_MODE_NET) {
            memcpy(src_url,  from->url,         sizeof(src_url));
            memcpy(src_uuid, from->stationuuid, sizeof(src_uuid));
        }
    }

    ipradio_preset_t *p = &store.presets[cell - 1];
    memset(p, 0, sizeof(*p));
    p->used = true;
    p->type = s_state.mode;

    if (s_state.mode == IPRADIO_MODE_FM) {
        p->band     = s_state.band;
        p->freq_khz = s_state.freq_khz;
        /* Имя берём из RDS, если оно настоящее. Подставлять частоту
         * вместо имени не надо: она и так на экране, а пустое имя
         * честно говорит, что станцию стоит подписать вручную. */
        if (s_state.rds_valid) {
            snprintf(p->name, sizeof(p->name), "%s", s_state.rds_name);
        }
    } else {
        snprintf(p->name, sizeof(p->name), "%s", s_state.station_name);

        /* Адрес берём из ячейки, которая играет сейчас.
         *
         * Раньше сохранялось только имя, и получалась ячейка без
         * адреса: при нажатии человек видел «станция не отвечает»,
         * хотя станция жива, а перенести было нечего. Сообщение
         * вводило в заблуждение.
         *
         * Источник адреса - активная ячейка: играть в интернет-режиме
         * мы можем только из неё. */
        memcpy(p->url,         src_url,  sizeof(p->url));
        memcpy(p->stationuuid, src_uuid, sizeof(p->stationuuid));

        if (p->url[0] == '\0') {
            ESP_LOGW(TAG, "ячейка %d записана без адреса потока",
                     (int) cell);
        }
    }

    s_state.active_preset = (int8_t) cell;

    if (ipradio_storage_save(&store) == ESP_OK) {
        ESP_LOGI(TAG, "пресет %d записан", (int) cell);
    } else {
        ESP_LOGW(TAG, "пресет %d записать не удалось", (int) cell);
    }
}

/* Перебор сохранённых станций энкодером 1 (§6.1).
 *
 * Перебираем ТОЛЬКО ячейки текущего режима. Иначе один щелчок мог бы
 * увести из эфира в интернет и обратно, и человек не понимал бы,
 * почему звук пропадает на секунды. Смена режима — дело кнопки MODE
 * и прямого нажатия на ячейку другого типа. */
static void apply_station_step(int32_t steps)
{
    if (steps == 0) {
        return;
    }

    ipradio_store_t store;
    ipradio_storage_get(&store);

    int8_t list[IPRADIO_PRESET_MAX];
    int    n = 0;
    int    cur = -1;

    for (int i = 0; i < IPRADIO_PRESET_MAX; i++) {
        if (!store.presets[i].used || store.presets[i].type != s_state.mode) {
            continue;
        }
        if (s_state.active_preset == i + 1) {
            cur = n;
        }
        list[n++] = (int8_t) (i + 1);
    }

    if (n == 0) {
        return;   /* перебирать нечего */
    }

    /* По кругу: список короткий, и упор в край заставлял бы крутить
     * обратно. Если текущей станции в списке нет (только включились
     * или сменили режим), начинаем с первой. */
    int next = (cur < 0) ? 0 : (cur + (int) steps) % n;
    if (next < 0) {
        next += n;
    }

    load_preset(&store.presets[list[next] - 1], list[next]);
}

/* ------------------------------------------------------------------ *
 *  Перестройка частоты
 * ------------------------------------------------------------------ */

static void apply_freq_delta(int32_t steps)
{
    if (s_state.mode != IPRADIO_MODE_FM || steps == 0) {
        return;
    }

#if IPRADIO_ENABLE_OIRT
    bool oirt = (s_state.band == IPRADIO_BAND_OIRT);
#else
    const bool oirt = false;
#endif
    uint32_t step = oirt ? TUNER_STEP_OIRT_KHZ : TUNER_STEP_CCIR_KHZ;
    uint32_t lo   = oirt ? TUNER_OIRT_MIN_KHZ  : TUNER_CCIR_MIN_KHZ;
    uint32_t hi   = oirt ? TUNER_OIRT_MAX_KHZ  : TUNER_CCIR_MAX_KHZ;

    int64_t f = (int64_t) s_state.freq_khz + (int64_t) steps * (int64_t) step;

    /* У края останавливаемся, а не заворачиваемся. Заворот выглядел бы
     * как скачок через весь диапазон — на приёмнике так не бывает
     * и быть не должно. */
    if (f < (int64_t) lo) f = lo;
    if (f > (int64_t) hi) f = hi;

    s_state.freq_khz = (uint32_t) f;

    /* Ручная перестройка уводит с пресета: мы больше не на нём. */
    s_state.active_preset = -1;
    s_state.rds_name[0]   = '\0';
    s_state.rds_valid     = false;
}

static void apply_band_toggle(void)
{
#if IPRADIO_ENABLE_OIRT
    if (s_state.mode != IPRADIO_MODE_FM) {
        return;
    }

    s_state.band = (s_state.band == IPRADIO_BAND_OIRT)
                   ? IPRADIO_BAND_CCIR : IPRADIO_BAND_OIRT;

    /* Частота из прежнего диапазона в новом невозможна — становимся
     * на его нижнюю границу. */
    s_state.freq_khz = (s_state.band == IPRADIO_BAND_OIRT)
                       ? TUNER_OIRT_MIN_KHZ : TUNER_CCIR_MIN_KHZ;

    s_state.active_preset = -1;
    s_state.rds_name[0]   = '\0';
    s_state.rds_valid     = false;
#else
    /* Диапазон один, переключать нечего. Событие оставлено, чтобы
     * при возврате УКВ не пришлось трогать ни ввод, ни интерфейс. */
#endif
}

/* ------------------------------------------------------------------ *
 *  Выключение
 * ------------------------------------------------------------------ */

static void apply_power(void)
{
    /* Сохраняем то, что должно пережить выключение, ПОКА питание есть.
     * Откладывать это на мост нельзя: он в этот момент уже глушит
     * железо, и порядок операций становится важен. */
    ipradio_store_t store;
    ipradio_storage_get(&store);

    store.settings.volume        = s_state.volume;
    store.settings.last_mode     = s_state.mode;
    store.settings.last_band     = s_state.band;
    store.settings.last_freq_khz = s_state.freq_khz;
    store.settings.last_preset   = s_state.active_preset;
    ipradio_storage_save_settings(&store.settings);

    s_state.power_off = true;
    ESP_LOGI(TAG, "запрошено выключение, настройки сохранены");
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

    case IPRADIO_EV_TUNE_DELTA:
        apply_station_step(e->arg);
        break;

    case IPRADIO_EV_FREQ_DELTA:
        apply_freq_delta(e->arg);
        break;

    case IPRADIO_EV_BAND_TOGGLE:
        apply_band_toggle();
        break;

    case IPRADIO_EV_PRESET_PRESSED:
        apply_preset_pressed(e->arg);
        break;

    case IPRADIO_EV_PRESET_HOLD:
        apply_preset_hold(e->arg);
        break;

    case IPRADIO_EV_POWER:
        apply_power();
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

    int wdt = ipradio_watchdog_register("state", 3000,
                                        IPRADIO_WDT_F_BLOCKED_OK);

    for (;;) {
        if (xQueueReceive(s_queue, &e, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        ipradio_watchdog_feed(wdt);

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

    /* Стартовое состояние берём из хранилища: прибор должен включаться
     * там же, где его выключили. Если карты нет или файл побит,
     * storage отдаёт умолчания — отдельной ветки на этот случай
     * здесь не нужно. */
    memset(&s_state, 0, sizeof(s_state));

    ipradio_store_t store;
    ipradio_storage_get(&store);

    s_state.volume        = store.settings.volume;
#if IPRADIO_ENABLE_OIRT
    s_state.band          = store.settings.last_band;
#else
    /* Диапазон один. Читать его из файла нельзя: там может лежать
     * УКВ от сборки, где он был включён, и тогда приёмник встал бы
     * на частоту, которой в FM не существует. */
    s_state.band          = IPRADIO_BAND_CCIR;
#endif
    s_state.freq_khz      = store.settings.last_freq_khz;
    s_state.active_preset = store.settings.last_preset;
    s_state.muted         = false;
    s_state.play          = IPRADIO_PLAY_IDLE;
    s_state.net           = IPRADIO_NET_NOT_CONFIGURED;

    /* Режим при старте — ВСЕГДА эфирный, даже если выключились
     * в интернетном. Сети на этот момент ещё нет, и стартовать
     * в режиме, который заведомо молчит, нельзя (§5.2, правило 4).
     * Вернуться к последней интернетной станции — дело моста,
     * и только когда сеть действительно поднимется. */
    s_state.mode = IPRADIO_MODE_FM;

    if (s_state.freq_khz == 0) {
        s_state.freq_khz = 102500;
    }

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
