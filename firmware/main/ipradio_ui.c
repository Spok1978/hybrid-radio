/*
 * ipradio_ui.c — главный экран воспроизведения.
 *
 * Здесь пока один экран из двенадцати — тот, который прибор показывает
 * почти всё время. Остальные (меню, шкала настройки, клавиатура,
 * ждущий режим, три состояния недоступности сети) добавляются поверх
 * этой же основы: макеты нарисованы в design/, соглашения по цвету
 * и шрифтам заданы в docs/28-screen-mockups.md.
 *
 * Два правила, которые определяют устройство файла:
 *
 *   1. Отрисовка идёт ТОЛЬКО в задаче LVGL. Автомат зовёт нас из своей
 *      задачи, поэтому его вызов лишь копирует снимок и ставит флаг.
 *      Иначе получили бы правку виджетов из двух задач сразу.
 *   2. Цвет кодирует режим: янтарь — эфир, циан — интернет. Это не
 *      украшение: режим должен читаться раньше, чем прочитан текст.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"

#include "ipradio_state.h"
#include "ipradio_storage.h"
#include "ipradio_ui.h"
#include "ipradio_fonts.h"
#include "ipradio_ui_theme.h"
#include "ipradio_ui_dialog.h"
#include "ipradio_ui_idle.h"
#include "ipradio_ui_menu.h"
#include "ipradio_ui_tune.h"
#include "ipradio_ui_keyboard.h"
#include "ipradio_ui_wifi.h"
#include "ipradio_ui_search.h"
#include "ipradio_ui_stations.h"
#include "ipradio_ui_diag.h"
#include "ipradio_ui_brightness.h"
#include "ipradio_ui_clock.h"
#include "ipradio_watchdog.h"
#include "ipradio_input.h"

static const char *TAG = "ui";

static void apply_snapshot(const ipradio_snapshot_t *s);

#define PRESET_CELLS    8

static SemaphoreHandle_t  s_lock;      /* защищает снимок и очередь ниже */
static ipradio_snapshot_t s_pending;
static volatile bool      s_dirty;

/* Последний увиденный счётчик отказов кнопке MODE. Диалог поднимается,
 * когда значение в снимке от него отличается, — то есть в ответ
 * на нажатие, а не просто потому, что сеть недоступна. */
static uint16_t s_seen_denied_seq;

/* Показан ли уже диалог про молчащую станцию. Без этого он всплывал бы
 * заново при каждом снимке: IPRADIO_PLAY_ERROR — состояние, а не
 * событие, и держится, пока его не сменят. */
static bool s_station_dead_shown;

/* Команды диалогу от органов управления.
 *
 * Фильтр событий работает в задаче автомата, а трогать виджеты можно
 * только из задачи LVGL (правило 1 в шапке файла). Поэтому фильтр
 * не рисует, а складывает команду сюда, и её разбирает ui_task.
 * Кольцо короткое: быстрее, чем человек крутит энкодер, команды
 * не приходят, а переполнение означало бы, что интерфейс завис —
 * и лишние повороты тогда всё равно не нужны. */
#define MODAL_QUEUE_LEN 8

typedef enum {
    MODAL_MOVE = 0,   /* сдвинуть выделение   */
    MODAL_SELECT,     /* подтвердить          */
    MODAL_LONG,       /* второе действие строки */
    MODAL_BACK,       /* уровень вверх        */
    MODAL_OPEN_MENU,  /* открыть меню         */
} modal_kind_t;

typedef struct {
    modal_kind_t kind;
    int          delta;
} modal_cmd_t;

static modal_cmd_t s_modal_q[MODAL_QUEUE_LEN];
static uint8_t     s_modal_head;
static uint8_t     s_modal_count;

/* Виджеты, которые меняются по состоянию. */
static lv_obj_t *s_clock;
static lv_obj_t *s_mode_badge;
static lv_obj_t *s_mode_text;
static lv_obj_t *s_vol_text;
static lv_obj_t *s_title;        /* крупное название или частота  */
static lv_obj_t *s_subtitle;     /* частота, метаданные, состояние */
static lv_obj_t *s_detail;       /* мелкая строка снизу            */
static lv_obj_t *s_mute_badge;
static lv_obj_t *s_presets[PRESET_CELLS];
static lv_obj_t *s_preset_names[PRESET_CELLS];
static lv_obj_t *s_preset_kind[PRESET_CELLS];

/* Копия банка для отрисовки полосы пресетов. Перечитываем только
 * когда номер поколения изменился: файл занимает пару килобайт,
 * а полоса перерисовывается на каждое событие автомата. */
static ipradio_store_t s_bank;
static uint32_t        s_bank_gen = UINT32_MAX;

/* ------------------------------------------------------------------ *
 *  Сборка экрана
 * ------------------------------------------------------------------ */

/* Раньше здесь был свой make_label. Уехал в ipradio_ui_theme.c,
 * когда экранов стало больше одного: цвета и форма подписей
 * должны быть общими, иначе разъедутся. */
#define make_label ipradio_ui_label

static void build_status_bar(lv_obj_t *root)
{
    lv_obj_t *bar = lv_obj_create(root);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), 48);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_hor(bar, 32, 0);

    /* Время — МЕЛКИМ шрифтом в ЛЕВОМ верхнем углу. Явное требование
     * ТЗ: оно не должно перекрывать название станции. */
    s_clock = make_label(bar, ipradio_font_20, COL_TEXT_DIM, "--:--");
    lv_obj_align(s_clock, LV_ALIGN_LEFT_MID, 0, 0);

    /* Справа: громкость и бейдж режима. */
    s_vol_text = make_label(bar, ipradio_font_16, COL_TEXT_DIM, "0");
    lv_obj_align(s_vol_text, LV_ALIGN_RIGHT_MID, -150, 0);

    s_mode_badge = lv_obj_create(bar);
    lv_obj_remove_style_all(s_mode_badge);
    lv_obj_set_size(s_mode_badge, 130, 34);
    lv_obj_align(s_mode_badge, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(s_mode_badge, 17, 0);
    lv_obj_set_style_bg_opa(s_mode_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_mode_badge, 1, 0);

    s_mode_text = make_label(s_mode_badge, ipradio_font_14,
                             COL_AMBER, "ЭФИР");
    lv_obj_center(s_mode_text);
}

static void build_center(lv_obj_t *root)
{
    /* Крупное название — требование по читаемости с расстояния,
     * а не оформление. Ради него взят самый большой доступный шрифт. */
    s_title = make_label(root, ipradio_font_48b, COL_TEXT, "");
    lv_obj_set_width(s_title, LV_PCT(90));
    lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_DOT);
    lv_obj_align(s_title, LV_ALIGN_CENTER, 0, -60);

    s_subtitle = make_label(root, ipradio_font_28, COL_AMBER, "");
    lv_obj_align(s_subtitle, LV_ALIGN_CENTER, 0, 10);

    s_detail = make_label(root, ipradio_font_16, COL_TEXT_FAINT, "");
    lv_obj_align(s_detail, LV_ALIGN_CENTER, 0, 56);

    /* Плашка приглушения. Значка в углу мало: человек решит, что прибор
     * сломался, а не что он сам выключил звук. */
    s_mute_badge = lv_obj_create(root);
    lv_obj_remove_style_all(s_mute_badge);
    lv_obj_set_size(s_mute_badge, 300, 56);
    lv_obj_align(s_mute_badge, LV_ALIGN_CENTER, 0, 110);
    lv_obj_set_style_radius(s_mute_badge, 28, 0);
    lv_obj_set_style_bg_color(s_mute_badge, lv_color_hex(0x2a1210), 0);
    lv_obj_set_style_bg_opa(s_mute_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_mute_badge, lv_color_hex(0x6b2a26), 0);
    lv_obj_set_style_border_width(s_mute_badge, 1, 0);

    lv_obj_t *mt = make_label(s_mute_badge, ipradio_font_22,
                              COL_RED, "ЗВУК ВЫКЛЮЧЕН");
    lv_obj_center(mt);
    lv_obj_add_flag(s_mute_badge, LV_OBJ_FLAG_HIDDEN);
}

static void build_presets(lv_obj_t *root)
{
    static int32_t cols[PRESET_CELLS + 1];
    for (int i = 0; i < PRESET_CELLS; i++) {
        cols[i] = LV_GRID_FR(1);
    }
    cols[PRESET_CELLS] = LV_GRID_TEMPLATE_LAST;
    static int32_t rows[] = { 88, LV_GRID_TEMPLATE_LAST };

    lv_obj_t *strip = lv_obj_create(root);
    lv_obj_remove_style_all(strip);
    lv_obj_set_size(strip, LV_PCT(100), 110);
    lv_obj_align(strip, LV_ALIGN_BOTTOM_MID, 0, -46);
    lv_obj_set_style_pad_hor(strip, 32, 0);
    lv_obj_set_style_pad_column(strip, 12, 0);
    lv_obj_set_grid_dsc_array(strip, cols, rows);
    lv_obj_set_layout(strip, LV_LAYOUT_GRID);

    for (int i = 0; i < PRESET_CELLS; i++) {
        lv_obj_t *cell = lv_obj_create(strip);
        lv_obj_remove_style_all(cell);
        lv_obj_set_grid_cell(cell, LV_GRID_ALIGN_STRETCH, i, 1,
                                   LV_GRID_ALIGN_STRETCH, 0, 1);
        lv_obj_set_style_bg_color(cell, COL_SURFACE, 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(cell, 12, 0);
        lv_obj_set_style_border_color(cell, COL_BORDER, 0);
        lv_obj_set_style_border_width(cell, 1, 0);
        lv_obj_set_style_pad_all(cell, 12, 0);

        char num[8];
        snprintf(num, sizeof(num), "П%d", i + 1);
        lv_obj_t *n = make_label(cell, ipradio_font_14,
                                 COL_TEXT_FAINT, num);
        lv_obj_align(n, LV_ALIGN_TOP_LEFT, 0, 0);

        /* Тип ячейки - в правом верхнем углу, рядом с номером.
         * Он должен читаться ДО нажатия: ячейки разного типа
         * переключают режим, и человек вправе знать это заранее. */
        s_preset_kind[i] = make_label(cell, ipradio_font_14,
                                      COL_TEXT_FAINT, "");
        lv_obj_align(s_preset_kind[i], LV_ALIGN_TOP_RIGHT, 0, 0);

        s_preset_names[i] = make_label(cell, ipradio_font_16,
                                       COL_TEXT_DIM, "");
        lv_obj_set_width(s_preset_names[i], LV_PCT(100));
        lv_label_set_long_mode(s_preset_names[i], LV_LABEL_LONG_DOT);
        lv_obj_align(s_preset_names[i], LV_ALIGN_BOTTOM_LEFT, 0, 0);

        s_presets[i] = cell;
    }
}

static void build_hints(lv_obj_t *root)
{
    /* Полоса подсказок по физическим органам. Следствие R4.1:
     * человек должен видеть, что всё делается ручками. */
    lv_obj_t *line = lv_obj_create(root);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, LV_PCT(100), 44);
    lv_obj_align(line, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_border_side(line, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(line, lv_color_hex(0x1c1f23), 0);
    lv_obj_set_style_border_width(line, 1, 0);

    lv_obj_t *t = make_label(line, ipradio_font_14, COL_TEXT_FAINT,
        "Энкодер 1 — станции   •   Энкодер 2 — громкость   •   нажатие — звук");
    lv_obj_center(t);
}

/* ------------------------------------------------------------------ *
 *  Обновление по состоянию
 * ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ *
 *  Диалоги недоступности сети
 * ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ *
 *  Стопка модальных экранов
 * ------------------------------------------------------------------ */

/* Экранов, забирающих себе органы управления, стало восемь, и цепочка
 * «если открыт этот, иначе если тот» перестала читаться. Здесь она
 * сведена в таблицу.
 *
 * ПОРЯДОК СТРОК — ПОРЯДОК НАЛОЖЕНИЯ, сверху вниз. Клавиатура первая,
 * потому что открывается поверх всего, включая выбор сети и поиск
 * станций; диалог последний, потому что лежит под всеми. Кто открыт
 * и лежит выше — тот и получает поворот энкодера.
 *
 * Добавить экран теперь значит добавить строку. Раньше это значило
 * найти и поправить семь мест, и одно из них забыть. */
typedef struct {
    bool  (*visible)(void);
    void  (*move)(int delta);
    void  (*select)(void);
    void  (*long_press)(void);   /* второе действие строки, если есть */
    void  (*back)(void);
} modal_screen_t;

static bool dialog_visible(void)
{
    return ipradio_dialog_current() != IPRADIO_DIALOG_NONE;
}

/* Диалог закрывается своими кнопками, «назад» у него нет: у каждого
 * есть кнопка отказа, и уйти из него молча нельзя — он задал вопрос. */
static const modal_screen_t MODALS[] = {
    { ipradio_keyboard_visible,   ipradio_keyboard_move,
      ipradio_keyboard_select,    NULL,
      ipradio_keyboard_back },

    { ipradio_brightness_ui_visible, ipradio_brightness_ui_move,
      ipradio_brightness_ui_select,  NULL,
      ipradio_brightness_ui_back },

    { ipradio_clock_ui_visible,   ipradio_clock_ui_move,
      ipradio_clock_ui_select,    NULL,
      ipradio_clock_ui_back },

    { ipradio_diag_ui_visible,    NULL,
      NULL,                       NULL,
      ipradio_diag_ui_back },

    { ipradio_stations_ui_visible, ipradio_stations_ui_move,
      ipradio_stations_ui_select,  ipradio_stations_ui_long_select,
      ipradio_stations_ui_back },

    { ipradio_search_ui_visible,  ipradio_search_ui_move,
      ipradio_search_ui_select,   NULL,
      ipradio_search_ui_back },

    { ipradio_wifi_ui_visible,    ipradio_wifi_ui_move,
      ipradio_wifi_ui_select,     NULL,
      ipradio_wifi_ui_back },

    { ipradio_tune_visible,       ipradio_tune_move,
      ipradio_tune_select,        NULL,
      ipradio_tune_back },

    { ipradio_menu_visible,       ipradio_menu_move,
      ipradio_menu_select,        NULL,
      ipradio_menu_back },

    { dialog_visible,             ipradio_dialog_move,
      ipradio_dialog_select,      NULL,
      NULL },
};

#define MODAL_COUNT (sizeof(MODALS) / sizeof(MODALS[0]))

/* Самый верхний из открытых, либо NULL. */
static const modal_screen_t *modal_top(void)
{
    for (size_t i = 0; i < MODAL_COUNT; i++) {
        if (MODALS[i].visible()) {
            return &MODALS[i];
        }
    }
    return NULL;
}

/* Открыт ли хоть один. Отдельно от modal_top, потому что спрашивают
 * об этом чаще, чем нужен сам экран. */
static bool modal_any(void)
{
    return modal_top() != NULL;
}

/* Фильтр событий. Ставится один раз при подъёме интерфейса и висит
 * всегда: ставить и снимать его при каждом диалоге значило бы
 * заводить ещё одно состояние, которое можно рассинхронизировать.
 * Решение принимается здесь, по обстановке.
 *
 * Работает в задаче автомата, поэтому только складывает команду
 * в кольцо — рисует ui_task (правило 1 в шапке файла). */
static void modal_push(modal_kind_t kind, int delta)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_modal_count < MODAL_QUEUE_LEN) {
        uint8_t tail = (uint8_t) ((s_modal_head + s_modal_count) % MODAL_QUEUE_LEN);
        s_modal_q[tail].kind  = kind;
        s_modal_q[tail].delta = delta;
        s_modal_count++;
    }
    xSemaphoreGive(s_lock);
}

static bool modal_filter(const ipradio_event_t *ev, void *ctx)
{
    (void) ctx;

    const modal_screen_t *top = modal_top();

    /* Долгое нажатие энкодера 1. С экрана воспроизведения открывает
     * меню; на экране, где у строки два действия, - второе из них;
     * на прочих модальных просто гасится, чтобы из диалога нельзя
     * было провалиться в меню: прибор задал вопрос и ждёт ответа. */
    if (ev->type == IPRADIO_EV_MENU) {
        if (!top) {
            modal_push(MODAL_OPEN_MENU, 0);
        } else if (top->long_press) {
            modal_push(MODAL_LONG, 0);
        }
        return true;
    }

    if (!top) {
        return false;   /* обычный экран, ничего не перехватываем */
    }

    switch (ev->type) {
    case IPRADIO_EV_TUNE_DELTA:
        modal_push(MODAL_MOVE, (int) ev->arg);
        return true;

    case IPRADIO_EV_SELECT:
        modal_push(MODAL_SELECT, 0);
        return true;

    /* Нажатие энкодера 2. На экранах настройки это «назад» (§5.3),
     * в диалоге - по-прежнему mute: диалог закрывается своими
     * кнопками, а звук человеку может понадобиться именно в тот
     * момент, когда прибор что-то от него хочет. Отсюда и NULL
     * в столбце back у диалога. */
    case IPRADIO_EV_MUTE_TOGGLE:
        if (top->back) {
            modal_push(MODAL_BACK, 0);
            return true;
        }
        return false;

    /* Громкость и питание проходят насквозь всегда: прибор не должен
     * становиться неуправляемым из-за плашки на экране. */
    default:
        return false;
    }
}

/* ------------------------------------------------------------------ *
 *  Меню
 * ------------------------------------------------------------------ */

static void on_tune_closed(void *ctx)
{
    (void) ctx;

    /* Вернулись на меню, а не на воспроизведение: экран настройки
     * открывался из меню, и выход должен возвращать туда, откуда
     * вошли. Меню всё это время было под нами и никуда не делось. */
    ipradio_snapshot_t snap;
    ipradio_get(&snap);
    ipradio_menu_update(&snap);
}

static void on_wifi_closed(void *ctx)
{
    (void) ctx;

    /* Возвращаемся туда, откуда пришли. Экран сети открывается
     * из двух мест — из меню и из диалога 10, — и в первом случае
     * под нами меню, которому надо обновить значения справа:
     * состояние Wi-Fi там как раз и написано. */
    ipradio_snapshot_t snap;
    ipradio_get(&snap);
    if (ipradio_menu_visible()) {
        ipradio_menu_update(&snap);
    } else {
        apply_snapshot(&snap);
    }
}

static void on_search_closed(void *ctx)
{
    (void) ctx;

    ipradio_snapshot_t snap;
    ipradio_get(&snap);
    if (ipradio_menu_visible()) {
        ipradio_menu_update(&snap);
    } else {
        apply_snapshot(&snap);
    }
}

static void on_menu_item(ipradio_menu_item_t item, void *ctx)
{
    (void) ctx;

    switch (item) {
    case IPRADIO_MENU_TUNE_FM:
        ipradio_tune_open(on_tune_closed, NULL);
        break;

    case IPRADIO_MENU_WIFI:
        ipradio_wifi_ui_open(on_wifi_closed, NULL);
        break;

    case IPRADIO_MENU_FIND_NET:
        ipradio_search_ui_open(on_search_closed, NULL);
        break;

    case IPRADIO_MENU_STATIONS:
        ipradio_stations_ui_open(on_search_closed, NULL);
        break;

    case IPRADIO_MENU_DIAGNOSTICS:
        ipradio_diag_ui_open(on_search_closed, NULL);
        break;

    case IPRADIO_MENU_BRIGHTNESS:
        ipradio_brightness_ui_open(on_search_closed, NULL);
        break;

    case IPRADIO_MENU_CLOCK:
        ipradio_clock_ui_open(on_search_closed, NULL);
        break;

    /* Все семь пунктов разобраны выше, попасть сюда нельзя. Ветка
     * оставлена сторожем: если в меню добавят пункт и забудут
     * добавить его сюда, это будет видно в журнале, а не проявится
     * молчащей строкой. */
    default:
        ESP_LOGW(TAG, "пункт меню %d никем не обслуживается", (int) item);
        break;
    }
}

static void on_menu_closed(ipradio_menu_item_t item, void *ctx)
{
    (void) item;
    (void) ctx;

    /* Меню закрылось — экран под ним мог устареть, пока его не было
     * видно: снимки в это время приходили, но рисовать их было некуда. */
    ipradio_snapshot_t snap;
    ipradio_get(&snap);
    apply_snapshot(&snap);
}

static void drain_modal_queue(void)
{
    for (;;) {
        modal_cmd_t cmd;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_modal_count == 0) {
            xSemaphoreGive(s_lock);
            return;
        }
        cmd = s_modal_q[s_modal_head];
        s_modal_head = (uint8_t) ((s_modal_head + 1) % MODAL_QUEUE_LEN);
        s_modal_count--;
        xSemaphoreGive(s_lock);

        if (cmd.kind == MODAL_OPEN_MENU) {
            ipradio_menu_open(on_menu_item, on_menu_closed, NULL);
            continue;
        }

        /* Верхний экран спрашиваем заново на каждую команду: пока
         * они лежали в кольце, стопка могла измениться - например,
         * предыдущая команда закрыла экран. */
        const modal_screen_t *top = modal_top();
        if (!top) {
            continue;
        }

        switch (cmd.kind) {
        case MODAL_MOVE:
            if (top->move) {
                top->move(cmd.delta);
            }
            break;

        case MODAL_SELECT:
            if (top->select) {
                top->select();
            }
            break;

        case MODAL_LONG:
            if (top->long_press) {
                top->long_press();
            }
            break;

        case MODAL_BACK:
            if (top->back) {
                top->back();
            }
            break;

        default:
            break;
        }
    }
}

static void on_dialog_action(ipradio_dialog_action_t action, void *ctx)
{
    (void) ctx;

    switch (action) {
    case IPRADIO_DIALOG_ACT_BACK_TO_FM:
        /* Возврат в эфир разрешён всегда — на этом стоит §5.2. */
        ipradio_post_simple(IPRADIO_EV_MODE_TOGGLE, 0);
        break;

    case IPRADIO_DIALOG_ACT_SETUP_WIFI:
        /* Прямой переход в настройку сети — то, ради чего диалог 10
         * вообще показывается (§5.2, случай 1). */
        ipradio_wifi_ui_open(on_wifi_closed, NULL);
        break;

    case IPRADIO_DIALOG_ACT_PICK_STATION:
        /* «Станция не отвечает» — предлагаем поискать другую.
         * Это и есть «сразу предложены другие» из §5.2, случай 3. */
        ipradio_search_ui_open(on_search_closed, NULL);
        break;

    case IPRADIO_DIALOG_ACT_DISMISS:
    default:
        break;
    }
}

static void raise_dialog(ipradio_dialog_kind_t kind, const char *detail)
{
    ipradio_dialog_show(kind, detail, on_dialog_action, NULL);
}

static void update_dialogs(const ipradio_snapshot_t *s)
{
    /* 1. Человек нажал MODE, а сети нет. Счётчик изменился — значит
     *    нажатие было именно сейчас, а не когда-то раньше. */
    if (s->mode_denied_seq != s_seen_denied_seq) {
        s_seen_denied_seq = s->mode_denied_seq;

        if (s->net == IPRADIO_NET_NOT_CONFIGURED) {
            raise_dialog(IPRADIO_DIALOG_WIFI_NOT_CONFIGURED, NULL);
        } else {
            /* Имя сети сюда придёт из модуля сети, когда появится
             * его хранение; пока показываем безымянный вариант. */
            raise_dialog(IPRADIO_DIALOG_WIFI_NO_LINK, NULL);
        }
        return;
    }

    /* 2. Станция молчит. Это состояние, а не событие, поэтому
     *    поднимаем один раз за переход. */
    bool dead = (s->mode == IPRADIO_MODE_NET) &&
                (s->play == IPRADIO_PLAY_ERROR);

    if (dead && !s_station_dead_shown) {
        s_station_dead_shown = true;
        raise_dialog(IPRADIO_DIALOG_STATION_DEAD,
                     s->station_name[0] ? s->station_name : NULL);
    } else if (!dead) {
        s_station_dead_shown = false;
    }

    /* 3. Связь вернулась — плашку убираем сами, без участия человека.
     *    Это правило 5 из §5.2: как только сеть есть, всё работает
     *    снова и ничего нажимать не надо. */
    if (s->net == IPRADIO_NET_CONNECTED) {
        ipradio_dialog_kind_t k = ipradio_dialog_current();
        if (k == IPRADIO_DIALOG_WIFI_NO_LINK ||
            k == IPRADIO_DIALOG_WIFI_NOT_CONFIGURED) {
            ipradio_dialog_hide();
        }
    }
}

/* ------------------------------------------------------------------ *
 *  Полоса пресетов
 * ------------------------------------------------------------------ */

/* Требование §5.2, правило 2: интернет-ячейки ГАСНУТ ЗАРАНЕЕ, когда
 * сети нет, и получают об этом пометку. Недоступность должна быть
 * видна ДО нажатия, а не после: нажатие не должно отвечать тишиной.
 *
 * Это и есть причина, по которой полоса читает банк, а не показывает
 * одни номера. Номер на кнопке человек видит и так - он написан
 * на самой кнопке. Экран нужен, чтобы сказать, ЧТО за ней и работает
 * ли она сейчас. */
static void render_presets(const ipradio_snapshot_t *s)
{
    uint32_t gen = ipradio_storage_generation();
    if (gen != s_bank_gen) {
        ipradio_storage_get(&s_bank);
        s_bank_gen = gen;
    }

    bool net_ok = (s->net == IPRADIO_NET_CONNECTED);

    for (int i = 0; i < PRESET_CELLS; i++) {
        const ipradio_preset_t *p = &s_bank.presets[i];
        bool active = (s->active_preset == i + 1);
        bool is_net = p->used && (p->type == IPRADIO_MODE_NET);

        /* Недоступна = интернетная ячейка при отсутствии сети. */
        bool unavailable = is_net && !net_ok;

        lv_color_t cell_accent = is_net ? COL_CYAN : COL_AMBER;

        if (!p->used) {
            lv_label_set_text(s_preset_names[i], "");
            lv_label_set_text(s_preset_kind[i], "");
        } else {
            /* Безымянная эфирная станция - обычное дело после
             * автопоиска. Показываем частоту: она хотя бы говорит,
             * куда попадёшь. */
            if (p->name[0]) {
                lv_label_set_text(s_preset_names[i], p->name);
            } else if (p->type == IPRADIO_MODE_FM) {
                char f[16];
                snprintf(f, sizeof(f), "%u.%02u",
                         (unsigned) (p->freq_khz / 1000),
                         (unsigned) ((p->freq_khz % 1000) / 10));
                lv_label_set_text(s_preset_names[i], f);
            } else {
                lv_label_set_text(s_preset_names[i], "без названия");
            }

            /* Перечёркнутый значок сети нарисовать нечем: в шрифте
             * его нет, а тащить ради одного знака набор пиктограмм
             * незачем. Пишем словом - оно и понятнее. */
            if (unavailable) {
                lv_label_set_text(s_preset_kind[i], "нет сети");
            } else {
                lv_label_set_text(s_preset_kind[i], is_net ? "сеть" : "эфир");
            }
        }

        /* Гашение недоступной ячейки: приглушаем и текст, и рамку.
         * Одного цвета текста мало - ячейка должна выглядеть
         * выключенной целиком. */
        lv_obj_set_style_opa(s_presets[i],
                             unavailable ? LV_OPA_40 : LV_OPA_COVER, 0);

        lv_obj_set_style_text_color(s_preset_kind[i],
            unavailable ? COL_RED : COL_TEXT_FAINT, 0);
        lv_obj_set_style_text_color(s_preset_names[i],
            active ? COL_TEXT : COL_TEXT_DIM, 0);

        lv_obj_set_style_border_color(s_presets[i],
            active ? cell_accent : COL_BORDER, 0);
        lv_obj_set_style_border_width(s_presets[i], active ? 2 : 1, 0);
        lv_obj_set_style_bg_color(s_presets[i],
            active ? (is_net ? lv_color_hex(0x0f2529) : lv_color_hex(0x2a2110))
                   : COL_SURFACE, 0);
    }
}

static void apply_snapshot(const ipradio_snapshot_t *s)
{
    bool fm = (s->mode == IPRADIO_MODE_FM);
    lv_color_t accent = fm ? COL_AMBER : COL_CYAN;

    /* Часы. */
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char buf[64];
    strftime(buf, sizeof(buf), "%H:%M", &tmv);
    lv_label_set_text(s_clock, buf);

    /* Бейдж режима: цвет и есть основной носитель смысла. */
    lv_obj_set_style_bg_color(s_mode_badge,
        fm ? lv_color_hex(0x2a2110) : lv_color_hex(0x0f2529), 0);
    lv_obj_set_style_border_color(s_mode_badge,
        fm ? lv_color_hex(0x6b5216) : lv_color_hex(0x1c5b66), 0);
    lv_obj_set_style_text_color(s_mode_text, accent, 0);
    lv_label_set_text(s_mode_text, fm ? "ЭФИР" : "ИНТЕРНЕТ");

    /* Громкость: при mute зачёркнутой её не сделать без своего шрифта,
     * поэтому просто гасим цвет — сигналом служит плашка по центру. */
    snprintf(buf, sizeof(buf), "%u", (unsigned) s->volume);
    lv_label_set_text(s_vol_text, buf);
    lv_obj_set_style_text_color(s_vol_text,
        s->muted ? COL_RED : COL_TEXT_DIM, 0);

    if (s->muted) {
        lv_obj_clear_flag(s_mute_badge, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_mute_badge, LV_OBJ_FLAG_HIDDEN);
    }

    if (fm) {
        /* Есть RDS — крупно название, частота под ним.
         * Нет RDS — крупно сама частота: пустоты на экране быть
         * не должно, а частота человеку понятна. */
        if (s->rds_valid && s->rds_name[0]) {
            lv_label_set_text(s_title, s->rds_name);
            snprintf(buf, sizeof(buf), "%u.%02u МГц",
                     (unsigned) (s->freq_khz / 1000),
                     (unsigned) ((s->freq_khz % 1000) / 10));
            lv_label_set_text(s_subtitle, buf);
            lv_label_set_text(s_detail,
                ipradio_band_label(s->band));
        } else {
            snprintf(buf, sizeof(buf), "%u.%02u МГц",
                     (unsigned) (s->freq_khz / 1000),
                     (unsigned) ((s->freq_khz % 1000) / 10));
            lv_label_set_text(s_title, buf);
            lv_label_set_text(s_subtitle,
                ipradio_band_label(s->band));
            lv_label_set_text(s_detail, "RDS не передаётся");
        }
    } else {
        lv_label_set_text(s_title,
            s->station_name[0] ? s->station_name : "-");

        switch (s->play) {
        case IPRADIO_PLAY_BUFFERING:
            /* На экране пишем, что происходит, а не молчим:
             * тишина без объяснения читается как поломка. */
            lv_label_set_text(s_subtitle, "Подключение…");
            lv_label_set_text(s_detail, "поток открыт, наполняется буфер");
            break;
        case IPRADIO_PLAY_ERROR:
            lv_label_set_text(s_subtitle, "Станция не отвечает");
            lv_label_set_text(s_detail, "Wi-Fi работает — дело в станции");
            break;
        default:
            lv_label_set_text(s_subtitle,
                s->icy_title[0] ? s->icy_title : "");
            snprintf(buf, sizeof(buf), "%u кбит/с   •   буфер %u %%",
                     (unsigned) s->bitrate_kbps, (unsigned) s->buffer_fill);
            lv_label_set_text(s_detail, buf);
            break;
        }
    }

    lv_obj_set_style_text_color(s_subtitle, accent, 0);

    update_dialogs(s);
    ipradio_menu_update(s);
    ipradio_tune_update(s);
    ipradio_wifi_ui_update(s);
    ipradio_diag_ui_update(s);

    render_presets(s);
}

/* ------------------------------------------------------------------ *
 *  Задача
 * ------------------------------------------------------------------ */

void ipradio_ui_notify(const ipradio_snapshot_t *snap)
{
    /* Зовётся из задачи автомата. Только копируем и ставим флаг:
     * трогать виджеты отсюда нельзя. */
    if (!snap || !s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_pending = *snap;
    s_dirty   = true;
    xSemaphoreGive(s_lock);
}

/* ------------------------------------------------------------------ *
 *  Ждущий режим
 * ------------------------------------------------------------------ */

/* Порог сторожит задача интерфейса, а не автомат: это свойство экрана,
 * а не радио. Прибор в ждущем режиме продолжает играть, и автомату
 * знать о нём незачем.
 *
 * Счётчик бездействия ведёт модуль ввода — он и так опрашивает органы
 * каждые 5 мс, и любое касание сбрасывает счётчик само. Отдельного
 * учёта здесь не нужно. */
static void service_idle(void)
{
    bool should = (ipradio_input_idle_ms() >= IPRADIO_IDLE_TIMEOUT_MS);

    /* Любой модальный экран важнее ждущего режима: если прибор ждёт
     * ответа или человек что-то настраивает, прятать это за часами
     * нельзя. */
    if (modal_any()) {
        should = false;
    }

    if (should != ipradio_idle_active()) {
        ipradio_idle_set_active(should);
    }

    if (should) {
        /* Часы в ждущем режиме обновляются здесь, а не по снимку:
         * событий может не быть минутами, а минута на часах меняется
         * независимо от того, происходит ли что-нибудь с радио. */
        ipradio_snapshot_t snap;
        ipradio_get(&snap);
        ipradio_idle_update(&snap);
    }
}

/* Собственного вызова lv_timer_handler здесь НЕТ, и это важно.
 * LVGL крутит задача, поднятая BSP платы внутри bsp_display_start();
 * второй такой цикл означал бы двух хозяев у одной библиотеки.
 *
 * Наша задача делает другое: переносит снимок состояния на виджеты,
 * разбирает команды модальных экранов и следит за бездействием.
 * Всё это трогает виджеты, поэтому идёт под замком BSP. */
static void ui_task(void *arg)
{
    (void) arg;

    /* Лимит 5 с, а не 2, как было предложено изначально. Шаг
     * цикла и правда 50 мс, но внутри drain_modal_queue лежит
     * запись на карту: удаление станции, сохранение найденной,
     * сохранение настроек часов. Запись на FAT со сбросом на
     * носитель занимает сотни миллисекунд, а на изношенной карте
     * бывает и дольше.
     *
     * Пять секунд всё ещё ловят настоящее зависание, но не выдают
     * за него медленную карту. */
    int wdt = ipradio_watchdog_register("ui", 5000, 0);

    for (;;) {
        ipradio_watchdog_feed(wdt);
        bool have_snap = false;
        ipradio_snapshot_t snap;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_dirty) {
            snap      = s_pending;
            s_dirty   = false;
            have_snap = true;
        }
        xSemaphoreGive(s_lock);

        /* Замок берём один раз на всю пачку работы, а не на каждый
         * вызов: чередоваться с задачей отрисовки посреди перерисовки
         * экрана значит показать человеку полуобновлённую картинку. */
        if (bsp_display_lock(100)) {
            if (have_snap) {
                apply_snapshot(&snap);
            }
            drain_modal_queue();
            service_idle();

            /* Экран выбора сети обновляем здесь, а не только по снимку
             * состояния: результат сканирования приезжает из своей
             * задачи и события автомата не порождает. Ждать очередного
             * тика значило бы показывать «идёт поиск» ещё секунду
             * после того, как он закончился. */
            if (ipradio_wifi_ui_visible()) {
                ipradio_snapshot_t s;
                ipradio_get(&s);
                ipradio_wifi_ui_update(&s);
            }

            /* То же и для прохода по диапазону: найденное приезжает
             * из задачи поиска. */
            if (ipradio_tune_visible()) {
                ipradio_tune_poll();
            }
            if (ipradio_search_ui_visible()) {
                ipradio_search_ui_poll();
            }
            /* На экране часов идут секунды: обновляем чаще, чем
             * приходят события автомата. */
            if (ipradio_clock_ui_visible()) {
                ipradio_clock_ui_poll();
            }

            bsp_display_unlock();
        }

        /* 50 мс - шаг обновления часов и отсчёта бездействия. Чаще
         * незачем: минута на часах меняется раз в минуту, а реакция
         * на энкодер идёт через кольцо команд и от этого шага
         * не зависит. */
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void on_state(const ipradio_snapshot_t *snap, void *ctx)
{
    (void) ctx;
    ipradio_ui_notify(snap);
}

esp_err_t ipradio_ui_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    /* Панель, тач и сам LVGL поднимает BSP платы. Один вызов делает
     * всё: MIPI-DSI, драйвер HX8394, GT911, буферы отрисовки и задачу
     * LVGL. Своего кода это не требует - см. docs/27, §1.4.
     *
     * Поворот на 90 градусов: панель физически книжная, 720x1280,
     * а прибор стоит горизонтально. Поворот делает железо (PPA),
     * не процессор.
     *
     * TEAR_AVOID_MODE_TRIPLE_PARTIAL - значение по умолчанию у BSP:
     * три буфера, частичная отрисовка. Полноэкранные буферы при
     * повороте делят пропускную способность PSRAM с периферией,
     * и интерфейс начинает дёргаться (docs/27, таблица граблей). */
    bsp_display_cfg_t disp_cfg = {
        .lv_adapter_cfg  = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation        = ESP_LV_ADAPTER_ROTATE_90,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL,
        .touch_flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
    };

    if (bsp_display_start_with_config(&disp_cfg) == NULL) {
        /* Панель не поднялась. Раньше этот случай ронял бы весь
         * прибор, потому что дальше шли вызовы LVGL по пустому
         * указателю. Теперь выходим честно: радио продолжит играть
         * вслепую, органы управления работают, а причина видна
         * в журнале. Приёмник без экрана - неудобно; приёмник,
         * который не включается из-за экрана, - сломан. */
        ESP_LOGE(TAG, "панель не поднялась, интерфейса не будет");
        return ESP_ERR_NOT_FOUND;
    }

    bsp_display_backlight_on();

    /* Дальше всё трогает виджеты, а значит идёт под замком. */
    if (!bsp_display_lock(1000)) {
        ESP_LOGE(TAG, "не удалось взять замок LVGL");
        return ESP_ERR_TIMEOUT;
    }

    /* Шрифты - раньше всего: первый же make_label к ним обратится. */
    esp_err_t err = ipradio_fonts_init();
    if (err != ESP_OK) {
        bsp_display_unlock();
        return err;
    }

    lv_obj_t *root = lv_screen_active();
    lv_obj_set_style_bg_color(root, COL_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(root, 0, 0);

    build_status_bar(root);
    build_center(root);
    build_presets(root);
    build_hints(root);

    /* Плашки недоступности сети строятся сразу, а показываются
     * по состоянию. Собирать их в момент, когда сеть уже отвалилась,
     * значило бы выделять память ровно тогда, когда что-то пошло
     * не так, - худший момент из возможных. */
    esp_err_t derr = ipradio_dialog_init(root);
    if (derr != ESP_OK) {
        bsp_display_unlock();
        return derr;
    }

    derr = ipradio_idle_init(root);
    if (derr != ESP_OK) {
        bsp_display_unlock();
        return derr;
    }

    derr = ipradio_menu_init(root);
    if (derr != ESP_OK) {
        bsp_display_unlock();
        return derr;
    }

    derr = ipradio_tune_init(root);
    if (derr != ESP_OK) {
        bsp_display_unlock();
        return derr;
    }

    derr = ipradio_keyboard_init(root);
    if (derr != ESP_OK) {
        bsp_display_unlock();
        return derr;
    }

    derr = ipradio_wifi_ui_init(root);
    if (derr != ESP_OK) {
        bsp_display_unlock();
        return derr;
    }

    derr = ipradio_search_ui_init(root);
    if (derr != ESP_OK) {
        bsp_display_unlock();
        return derr;
    }

    derr = ipradio_stations_ui_init(root);
    if (derr != ESP_OK) {
        bsp_display_unlock();
        return derr;
    }

    derr = ipradio_diag_ui_init(root);
    if (derr != ESP_OK) {
        bsp_display_unlock();
        return derr;
    }

    derr = ipradio_brightness_ui_init(root);
    if (derr != ESP_OK) {
        bsp_display_unlock();
        return derr;
    }

    derr = ipradio_clock_ui_init(root);
    if (derr != ESP_OK) {
        bsp_display_unlock();
        return derr;
    }

    /* Перехват органов управления. Ставится один раз и висит всегда:
     * фильтр сам разбирается, есть ли на экране что-то модальное. */
    ipradio_set_event_filter(modal_filter, NULL);

    /* Первая отрисовка по текущему состоянию, чтобы экран не был
     * пустым до первого события. */
    ipradio_snapshot_t snap;
    ipradio_get(&snap);
    apply_snapshot(&snap);

    bsp_display_unlock();

    ipradio_subscribe(on_state, NULL);

    xTaskCreate(ui_task, "ipradio_ui", 6144, NULL, 4, NULL);

    ESP_LOGI(TAG, "интерфейс поднят: 1280x720, поворот 90, подписи русские");
    return ESP_OK;
}
