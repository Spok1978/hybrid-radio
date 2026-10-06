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

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "freertos/idf_additions.h"

#include "board_pins.h"
#include "ipradio_state.h"
#include "ipradio_storage.h"
#include "ipradio_watchdog.h"
#include "ipradio_tuner.h"

static const char *TAG = "state";

#define EVENT_QUEUE_LEN     24
/* Восемь килобайт, а не четыре.
 *
 * На плате 2026-08-30 задача упала с защитой стека при первом же
 * нажатии пресета. Причина: структура банка около 2,5 КБ, и она
 * ложилась на стек - сперва в обработчике события, потом ещё раз
 * в подписчике, который зовётся из этой же задачи.
 *
 * Горячие пути переведены на выборочное чтение (одна ячейка вместо
 * банка), но перебор станций и запись пресета целую копию всё же
 * берут. Плюс подписчики: мост зовёт конвейер ADF, а тот - свои
 * функции с немаленькими кадрами. */
/* Шесть килобайт: восемь были взяты с запасом, а внутренней памяти
 * на плате в обрез - её просят стеки задач конвейера ADF, которым
 * PSRAM недоступна. Горячие пути читают банк по одной ячейке,
 * так что запас можно ужать. */
#define STATE_TASK_STACK    6144
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
    /* Поворот регулятора снимает mute и сразу выставляет уровень (§6.3):
     * иначе пользователь крутит, ничего не происходит, и при снятии
     * mute получается неожиданно громко. */
    if (s_state.muted) {
        s_state.muted = false;
    }
    s_state.volume = clamp_volume((int32_t) s_state.volume + steps);
}

/* Последняя ячейка КАЖДОГО режима.
 *
 * Банки разделены, и номер ячейки сам по себе ничего не значит без
 * режима: третья эфирная и третья интернетная - разные станции.
 * s_state.active_preset показывает ячейку ТЕКУЩЕГО режима, а эти два
 * поля помнят, на чём мы стояли в каждом. Без них переключение
 * режима оставляло бы номер от прежнего банка, и мост включал бы
 * станцию, которую человек не выбирал. */
static int8_t s_preset_fm  = -1;
static int8_t s_preset_net = -1;

/* Запомнить текущую ячейку за уходящим режимом и достать ту,
 * на которой стояли в приходящем. */
static void switch_mode_preset(ipradio_mode_t from, ipradio_mode_t to)
{
    if (from == IPRADIO_MODE_NET) {
        s_preset_net = s_state.active_preset;
    } else {
        s_preset_fm = s_state.active_preset;
    }

    int8_t want = (to == IPRADIO_MODE_NET) ? s_preset_net : s_preset_fm;

    /* Ячейку, на которой стояли, могли за это время очистить - из меню
     * долгого нажатия или из списка станций. Вставать на пустую нельзя:
     * мост попробует её включить и покажет «станция не отвечает» про
     * станцию, которой больше нет. */
    ipradio_preset_t p;
    if (want >= 1 && !ipradio_storage_get_preset(to, want, &p)) {
        want = -1;
    }
    s_state.active_preset = want;
}

static void apply_mode_toggle(void)
{
    if (s_state.mode == IPRADIO_MODE_NET) {
        /* Обратно в эфир — всегда можно, звук появится сразу. */
        switch_mode_preset(IPRADIO_MODE_NET, IPRADIO_MODE_FM);
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

    switch_mode_preset(IPRADIO_MODE_FM, IPRADIO_MODE_NET);
    s_state.mode = IPRADIO_MODE_NET;
    /* Без выбранной ячейки наполнять нечего: «буферизация» висела бы
     * на экране вечно. */
    s_state.play = (s_state.active_preset >= 1) ? IPRADIO_PLAY_BUFFERING
                                                : IPRADIO_PLAY_IDLE;
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

    /* Отмечаем сам факт нажатия, до всего прочего: человек попросил,
     * и мост должен об этом узнать, даже если ячейка та же самая. */
    s_state.preset_seq++;

    /* Банк берём по ТЕКУЩЕМУ режиму.
     *
     * До 2026-09-15 банк был один на оба режима, тип лежал в самой
     * ячейке, и нажатие пресета само переключало эфир на интернет
     * и обратно. Теперь банки разные: в эфире работают эфирные
     * ячейки, в интернете - интернетные, и нажатие режим не меняет.
     * Чтобы перейти в другой режим, есть плашка вверху экрана.
     *
     * Одна ячейка, а не весь банк: полная копия не влезает в стек
     * этой задачи вместе с тем, что зовётся дальше. */
    ipradio_preset_t p;
    if (ipradio_storage_get_preset(s_state.mode, (int) cell, &p)) {
        load_preset(&p, (int8_t) cell);
    }
}

/* Долгое нажатие — записать в ячейку то, что играет сейчас (§6.2). */
static void apply_preset_hold(int32_t cell)
{
    if (cell < 1 || cell > IPRADIO_PRESET_MAX) {
        return;
    }

    /* Правим хранилище на месте, под одним замком: прежде здесь
     * читали копию, меняли и писали целиком, и между чтением
     * и записью правка другой задачи терялась. Заодно уходит
     * копия на 2548 байт со стека. */
    ipradio_store_t *store_p = ipradio_storage_edit_begin();
    if (!store_p) {
        return;
    }

    /* Адрес потока копируем В ЛОКАЛЬНЫЕ буферы до очистки ячейки.
     * Источник и приёмник лежат в одной структуре store, и прямое
     * копирование между ними - перекрытие: компилятор справедливо
     * на это ругается. Плюс писать могут поверх той самой ячейки,
     * что сейчас играет. */
    char src_url[IPRADIO_URL_MAX]  = { 0 };
    char src_uuid[IPRADIO_UUID_MAX] = { 0 };

    /* Оба обращения - к банку ТЕКУЩЕГО режима: записываем то, что
     * играет сейчас, а играет оно в этом режиме. */
    ipradio_preset_t *bank = ipradio_storage_bank(store_p, s_state.mode);

    const int8_t src = s_state.active_preset;
    if (src >= 1 && src <= IPRADIO_PRESET_MAX) {
        const ipradio_preset_t *from = &bank[src - 1];
        if (from->used && s_state.mode == IPRADIO_MODE_NET) {
            memcpy(src_url,  from->url,         sizeof(src_url));
            memcpy(src_uuid, from->stationuuid, sizeof(src_uuid));
        }
    }

    ipradio_preset_t *p = &bank[cell - 1];
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

    if (ipradio_storage_edit_end(true) == ESP_OK) {
        ESP_LOGI(TAG, "пресет %d записан", (int) cell);
    } else {
        ESP_LOGW(TAG, "пресет %d записать не удалось", (int) cell);
    }
}

/* Ячейку поправили с экрана: переименовали или очистили.
 * arg - IPRADIO_PRESET_ARG(режим, ячейка).
 *
 * Поток здесь НЕ трогаем никогда: preset_seq не поднимаем,
 * active_preset не меняем. И то и другое мост понял бы как просьбу
 * включить станцию заново (ipradio_bridge.c, apply_stream), а она
 * и так играет. */
static void apply_preset_edited(int32_t arg)
{
    const int            cell = (int) (arg & 0xFF);
    const ipradio_mode_t mode = (ipradio_mode_t) ((arg >> 8) & 0xFF);

    if (cell < 1 || cell > IPRADIO_PRESET_MAX) {
        return;
    }

    ipradio_preset_t p;
    const bool used = ipradio_storage_get_preset(mode, cell, &p);

    if (mode != s_state.mode) {
        /* Поправили банк другого режима. Если очистили ту ячейку,
         * которую он помнит, - забываем её, иначе при возврате
         * встали бы на пустую. */
        int8_t *mem = (mode == IPRADIO_MODE_NET) ? &s_preset_net
                                                 : &s_preset_fm;
        if (!used && *mem == cell) {
            *mem = -1;
        }
        return;
    }

    if (cell != s_state.active_preset || !used) {
        /* Не та, что играет, - на экране воспроизведения менять нечего.
         *
         * Очищенная играющая остаётся отмеченной до следующего выбора:
         * звук идёт, а снять отметку значило бы сказать мосту, что
         * выбрана другая ячейка. При уходе в другой режим и возврате
         * на пустую уже не встанем - это сторожит switch_mode_preset. */
        return;
    }

    /* Переименовали ту, что играет, - новое имя сразу на экран, а не
     * при следующем нажатии. */
    if (mode == IPRADIO_MODE_NET) {
        snprintf(s_state.station_name, sizeof(s_state.station_name),
                 "%s", p.name);
    } else if (!s_state.rds_valid) {
        /* Имя эфирной станции - заглушка до настоящего RDS, см.
         * load_preset. Настоящее RDS перебивать нельзя. */
        snprintf(s_state.rds_name, sizeof(s_state.rds_name), "%s", p.name);
    }
}

/* Включить интернетную ячейку из любого режима.
 *
 * PRESET_PRESSED для этого не годится с 2026-09-15: банки разделены,
 * и нажатие берёт ячейку ТЕКУЩЕГО режима. Было две беды:
 *
 *   - мост при загрузке возвращал последнюю интернет-станцию событием
 *     PRESET_PRESSED, а прибор в этот момент в эфире - бралась эфирная
 *     ячейка с тем же номером;
 *   - поиск по каталогу, открытый в эфире, записывал станцию
 *     в интернетный банк, а включал эфирную ячейку с её номером.
 *
 * Режим и ячейка меняются одним событием, а не MODE_TOGGLE плюс
 * PRESET_PRESSED: после первого мост успел бы запустить прежнюю
 * интернетную станцию, а мгновением позже - новую. */
static void apply_net_preset_pressed(int32_t cell)
{
    if (cell < 1 || cell > IPRADIO_PRESET_MAX) {
        return;
    }

    /* Пустую ячейку не включаем и режим ради неё не меняем: в интернете
     * без станции человек увидел бы тишину. */
    ipradio_preset_t p;
    if (!ipradio_storage_get_preset(IPRADIO_MODE_NET, (int) cell, &p)) {
        return;
    }

    if (s_state.mode != IPRADIO_MODE_NET) {
        if (!net_playable()) {
            /* Как у кнопки MODE: эфир не прерываем, причину покажет
             * интерфейс по счётчику. */
            s_state.mode_denied_seq++;
            return;
        }
        switch_mode_preset(IPRADIO_MODE_FM, IPRADIO_MODE_NET);
        s_state.mode = IPRADIO_MODE_NET;
    }

    apply_preset_pressed(cell);
}

/* Перебор сохранённых станций регулятором 1 (§6.1).
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

    /* Только читаем - но берём хранилище на месте, а не копией:
     * копия это 2548 байт на стеке, а стек у этой задачи в PSRAM
     * и не резиновый. Замок отпускаем сразу, как дочитали. */
    const ipradio_store_t *store_p = ipradio_storage_edit_begin();
    if (!store_p) {
        return;
    }

    int8_t list[IPRADIO_PRESET_MAX];
    int    n = 0;
    int    cur = -1;

    /* Отбор по типу больше не нужен: банк и так принадлежит режиму. */
    const ipradio_preset_t *bank =
        ipradio_storage_bank((ipradio_store_t *) store_p, s_state.mode);

    for (int i = 0; i < IPRADIO_PRESET_MAX; i++) {
        if (!bank[i].used) {
            continue;
        }
        if (s_state.active_preset == i + 1) {
            cur = n;
        }
        list[n++] = (int8_t) (i + 1);
    }

    if (n == 0) {
        ipradio_storage_edit_end(false);
        return;   /* перебирать нечего */
    }

    /* По кругу: список короткий, и упор в край заставлял бы крутить
     * обратно. Если текущей станции в списке нет (только включились
     * или сменили режим), начинаем с первой. */
    int next = (cur < 0) ? 0 : (cur + (int) steps) % n;
    if (next < 0) {
        next += n;
    }

    ipradio_preset_t chosen = bank[list[next] - 1];
    int8_t chosen_cell = list[next];
    ipradio_storage_edit_end(false);

    load_preset(&chosen, chosen_cell);
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
    ipradio_settings_t set;
    ipradio_storage_get_settings(&set);

    set.volume        = s_state.volume;
    set.last_mode     = s_state.mode;
    set.last_band     = s_state.band;
    set.last_freq_khz = s_state.freq_khz;
    /* Текущую ячейку сначала кладём в поле своего режима, иначе
     * она бы не сохранилась - в s_preset_* её записывает только
     * переключение режима. */
    if (s_state.mode == IPRADIO_MODE_NET) {
        s_preset_net = s_state.active_preset;
    } else {
        s_preset_fm = s_state.active_preset;
    }
    set.last_preset_fm  = s_preset_fm;
    set.last_preset_net = s_preset_net;
    ipradio_storage_save_settings(&set);

    s_state.power_off = true;
    ESP_LOGI(TAG, "запрошено выключение, настройки сохранены");
}

static void apply_event(const queued_event_t *e)
{
    switch (e->type) {

    case IPRADIO_EV_VOLUME_DELTA:
        apply_volume_delta(e->arg);
        break;

    /* Ползунок задаёт уровень целиком, а не шагами: палец ставят
     * сразу туда, где хотят. Mute снимаем по той же причине, что
     * и при повороте регулятора - иначе человек ведёт ползунок,
     * а звука нет. */
    case IPRADIO_EV_VOLUME_SET:
        s_state.muted = false;
        s_state.volume = clamp_volume(e->arg);
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

    case IPRADIO_EV_PRESET_EDITED:
        apply_preset_edited(e->arg);
        break;

    case IPRADIO_EV_NET_PRESET_PRESSED:
        apply_net_preset_pressed(e->arg);
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

    /* Битрейт приходит от декодера один раз на станцию, вместе
     * с частотой дискретизации. Раньше поле в снимке состояния
     * никто не заполнял, и строка под названием станции показывала
     * «0 кбит/с» при исправно играющем потоке. */
    case IPRADIO_EV_BITRATE:
        s_state.bitrate_kbps = (uint16_t) (e->arg < 0 ? 0 :
                               (e->arg > 65535 ? 65535 : e->arg));
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

#ifdef IPRADIO_HOST_TEST
/* Разобрать всё, что накопилось в очереди, здесь и сейчас.
 *
 * Только для самопроверки на машине разработчика. Там задачи автомата
 * нет - заглушка FreeRTOS её не создаёт, и это сделано намеренно:
 * тест должен подать событие и тут же посмотреть состояние, без
 * планировщика и ожидания. На плате эта функция не собирается вовсе.
 *
 * Замок берём, как и настоящая задача: пусть путь исполнения будет
 * тот же, даже если в тесте замок и не нужен. */
void ipradio_state_pump(void)
{
    queued_event_t e;
    while (xQueueReceive(s_queue, &e, 0) == pdTRUE) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        apply_event(&e);
        xSemaphoreGive(s_lock);
    }
}
#endif

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
         * Он может забрать событие себе: пока висит диалог, регулятор 1
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

    /* Только настройки: банки здесь не нужны, а целое хранилище
     * после разделения весит около пяти килобайт - стек этой задачи
     * такого не выдержит. */
    ipradio_settings_t set;
    ipradio_storage_get_settings(&set);

    s_state.volume        = set.volume;
#if IPRADIO_ENABLE_OIRT
    s_state.band          = set.last_band;
#else
    /* Диапазон один. Читать его из файла нельзя: там может лежать
     * УКВ от сборки, где он был включён, и тогда приёмник встал бы
     * на частоту, которой в FM не существует. */
    s_state.band          = IPRADIO_BAND_CCIR;
#endif
    s_state.freq_khz      = set.last_freq_khz;
    s_preset_fm  = set.last_preset_fm;
    s_preset_net = set.last_preset_net;
    s_state.active_preset = (s_state.mode == IPRADIO_MODE_NET)
                            ? s_preset_net : s_preset_fm;
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

/* Стек этой задачи - В PSRAM.
     *
     * Внутренней памяти на плате в обрез, и просят её те, кому
     * без неё нельзя: буферы связи с сопроцессором Wi-Fi, рукопожатие
     * TLS и стеки задач конвейера ADF (тот не умеет внешние стеки
     * без патчей к ESP-IDF, которых мы не ставили).
     *
     * Наша задача таких ограничений не имеет: она не работает при
     * запрещённом кэше и не вызывается из прерываний. Значит её стек
     * и надо уступить. */
    BaseType_t ok = xTaskCreateWithCaps(state_task, "ipradio_state",
                                        STATE_TASK_STACK, NULL,
                                        STATE_TASK_PRIO, NULL,
                                        MALLOC_CAP_SPIRAM);
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
