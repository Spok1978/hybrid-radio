/*
 * ipradio_ui_presetmenu.c — меню ячейки по долгому нажатию.
 *
 * Замысел — в ipradio_ui_presetmenu.h.
 *
 * Все правки банка - через ipradio_preset_edit.c: там они проверены
 * на машине разработчика, включая то, что замок хранилища отпускается
 * на каждом пути.
 */

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"

#include "ipradio_fonts.h"
#include "ipradio_preset_edit.h"
#include "ipradio_storage.h"
#include "ipradio_ui_keyboard.h"
#include "ipradio_ui_presetmenu.h"
#include "ipradio_ui_search.h"
#include "ipradio_ui_theme.h"

static const char *TAG = "ui.cellmenu";

#define MENU_W        640
#define MENU_BTN_H     72
#define MENU_MAX_BTN    4

typedef enum {
    ACT_RENAME,
    ACT_REPLACE,
    ACT_ASK_DELETE,     /* «Удалить» среди действий: только спросить */
    ACT_DELETE,         /* «Удалить» в вопросе: очистить ячейку     */
    ACT_BACK,           /* «Отмена» в вопросе: назад к действиям     */
    ACT_CLOSE,          /* «Отмена» среди действий: закрыть          */
} act_t;

static lv_obj_t *s_shade;
static lv_obj_t *s_panel;
static lv_obj_t *s_head;     /* «Ячейка П3 • интернет» или вопрос        */
static lv_obj_t *s_name;     /* название станции                         */
static lv_obj_t *s_detail;   /* адрес, частота, пояснение или ошибка     */
static lv_obj_t *s_btn[MENU_MAX_BTN];
static lv_obj_t *s_btn_text[MENU_MAX_BTN];
static act_t     s_btn_act[MENU_MAX_BTN];
static int       s_btn_count;
static int       s_focus;
static bool      s_visible;
static bool      s_asking;   /* показан вопрос об удалении */

static ipradio_mode_t s_mode;
static int            s_cell;       /* 1…IPRADIO_PRESET_MAX */
static uint32_t       s_freq_khz;   /* где стоит приёмник, на момент открытия */

/* Копия ячейки: из неё подписи и имя для клавиатуры. Около трёхсот
 * байт - в PSRAM: внутренняя память на этой плате на счету
 * (docs/34-memory.md). Трогает её только задача интерфейса. */
EXT_RAM_BSS_ATTR static ipradio_preset_t s_preset;

/* ------------------------------------------------------------------ *
 *  Подписи
 * ------------------------------------------------------------------ */

static bool is_net(void)
{
    return s_mode == IPRADIO_MODE_NET;
}

static void freq_text(char *buf, size_t n, uint32_t khz)
{
    snprintf(buf, n, "%u.%02u МГц",
             (unsigned) (khz / 1000), (unsigned) ((khz % 1000) / 10));
}

/* Имя ячейки - теми же словами, что на главном экране
 * (render_presets в ipradio_ui.c): иначе человек не узнает её. */
static void display_name(char *buf, size_t n)
{
    if (s_preset.name[0]) {
        snprintf(buf, n, "%s", s_preset.name);
    } else if (!is_net()) {
        freq_text(buf, n, s_preset.freq_khz);
    } else {
        snprintf(buf, n, "без названия");
    }
}

static const char *pedit_text(ipradio_pedit_t r)
{
    switch (r) {
    case IPRADIO_PEDIT_NO_CARD:
        return "Карты памяти нет - записать некуда.";
    case IPRADIO_PEDIT_WRITE_FAILED:
        return "На карту записать не удалось: после перезагрузки "
               "изменение пропадёт.";
    case IPRADIO_PEDIT_EMPTY:
        return "Ячейка уже свободна.";
    default:
        return "Не получилось.";
    }
}

/* ------------------------------------------------------------------ *
 *  Кнопки
 * ------------------------------------------------------------------ */

static void paint_focus(void)
{
    for (int i = 0; i < MENU_MAX_BTN; i++) {
        bool on     = (i == s_focus) && (i < s_btn_count);
        bool danger = (s_btn_act[i] == ACT_ASK_DELETE ||
                       s_btn_act[i] == ACT_DELETE);
        lv_color_t mark = danger ? COL_RED : COL_TEXT;

        /* Выделение - заливкой, как в диалогах (ipradio_ui_dialog.c):
         * одного цвета текста мало, чтобы читалось через комнату.
         * Удаление красное и без выделения - видно издалека. */
        lv_obj_set_style_bg_color(s_btn[i], on ? mark : COL_SURFACE, 0);
        lv_obj_set_style_border_color(s_btn[i],
                                      (on || danger) ? mark : COL_BORDER, 0);
        lv_obj_set_style_border_width(s_btn[i], on ? 2 : 1, 0);
        lv_obj_set_style_text_color(s_btn_text[i], on ? COL_BG : mark, 0);
    }
}

static void clear_buttons(void)
{
    for (int i = 0; i < MENU_MAX_BTN; i++) {
        lv_obj_add_flag(s_btn[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_btn_count = 0;
}

static void add_button(const char *text, act_t act)
{
    if (s_btn_count >= MENU_MAX_BTN) {
        return;
    }
    int i = s_btn_count++;
    lv_label_set_text(s_btn_text[i], text);
    s_btn_act[i] = act;
    lv_obj_clear_flag(s_btn[i], LV_OBJ_FLAG_HIDDEN);
}

static void set_detail(const char *text, bool trouble)
{
    lv_label_set_text(s_detail, text ? text : "");
    lv_obj_set_style_text_color(s_detail, trouble ? COL_RED : COL_TEXT_DIM, 0);
}

/* ------------------------------------------------------------------ *
 *  Два вида: действия и вопрос
 * ------------------------------------------------------------------ */

/* note - строка о неудаче, показывается вместо пояснения. */
static void show_actions(const char *note)
{
    char buf[200];
    char freq[24];

    s_asking = false;

    snprintf(buf, sizeof(buf), "Ячейка П%d   •   %s",
             s_cell, is_net() ? "интернет" : "эфир");
    lv_label_set_text(s_head, buf);
    lv_obj_set_style_text_color(s_head, is_net() ? COL_CYAN : COL_AMBER, 0);

    freq_text(freq, sizeof(freq), s_freq_khz);
    clear_buttons();

    if (s_preset.used) {
        display_name(buf, sizeof(buf));
        lv_label_set_text(s_name, buf);

        if (is_net()) {
            /* Адрес - чтобы отличить одноимённые станции. Обрезаем явно
             * и с многоточием: где адрес оборвался, должно быть видно. */
            snprintf(buf, sizeof(buf), "%.120s%s",
                     s_preset.url[0] ? s_preset.url : "адрес не записан",
                     strlen(s_preset.url) > 120 ? "…" : "");
        } else {
            char f[24];
            freq_text(f, sizeof(f), s_preset.freq_khz);
            snprintf(buf, sizeof(buf), "Частота %s", f);
        }
        set_detail(buf, false);

        add_button("Переименовать", ACT_RENAME);
        if (is_net()) {
            add_button("Заменить: найти в каталоге", ACT_REPLACE);
        } else {
            snprintf(buf, sizeof(buf), "Заменить на %s", freq);
            add_button(buf, ACT_REPLACE);
        }
        add_button("Удалить", ACT_ASK_DELETE);
    } else {
        lv_label_set_text(s_name, "Свободна");
        if (is_net()) {
            set_detail("Сюда можно записать станцию из каталога.", false);
            add_button("Найти станцию в каталоге", ACT_REPLACE);
        } else {
            set_detail("Сюда можно записать частоту, на которой "
                       "приёмник стоит сейчас.", false);
            snprintf(buf, sizeof(buf), "Записать %s", freq);
            add_button(buf, ACT_REPLACE);
        }
    }
    add_button("Отмена", ACT_CLOSE);

    if (note) {
        set_detail(note, true);
    }

    s_focus = 0;
    paint_focus();
}

static void show_question(void)
{
    char name[IPRADIO_NAME_MAX + 16];
    char buf[200];

    s_asking = true;

    lv_label_set_text(s_head, "Удалить станцию?");
    lv_obj_set_style_text_color(s_head, COL_RED, 0);

    display_name(name, sizeof(name));
    lv_label_set_text(s_name, name);

    snprintf(buf, sizeof(buf), "Ячейка П%d станет свободной. %s", s_cell,
             is_net() ? "Вернуть станцию можно будет только новым "
                        "поиском в каталоге."
                      : "Вернуть станцию можно будет настройкой "
                        "или автопоиском.");
    set_detail(buf, false);

    clear_buttons();
    add_button("Удалить", ACT_DELETE);
    add_button("Отмена", ACT_BACK);

    /* Выделена «Отмена»: случайный щелчок регулятора не должен
     * ничего стирать. */
    s_focus = 1;
    paint_focus();
}

/* ------------------------------------------------------------------ *
 *  Показ
 * ------------------------------------------------------------------ */

static void show(void)
{
    s_visible = true;
    lv_obj_clear_flag(s_shade, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_shade);
}

/* Перечитать ячейку и показать действия заново - после неудачи,
 * когда плашка уже была закрыта (переименование идёт через
 * клавиатуру, и меню к тому времени убрано). */
static void reopen_with_note(const char *note)
{
    ipradio_storage_get_preset(s_mode, s_cell, &s_preset);
    show_actions(note);
    show();
}

void ipradio_presetmenu_close(void)
{
    if (!s_shade) {
        return;
    }
    s_visible = false;
    s_asking  = false;
    lv_obj_add_flag(s_shade, LV_OBJ_FLAG_HIDDEN);
}

/* ------------------------------------------------------------------ *
 *  Действия
 * ------------------------------------------------------------------ */

static void on_renamed(const char *text, void *ctx)
{
    (void) ctx;

    if (!text) {
        return;                       /* отказались от ввода */
    }

    ipradio_pedit_t r = ipradio_preset_rename(s_mode, s_cell, text);

    /* WRITE_FAILED - имя в памяти уже новое, экран должен это знать. */
    if (r == IPRADIO_PEDIT_OK || r == IPRADIO_PEDIT_WRITE_FAILED) {
        ipradio_post_simple(IPRADIO_EV_PRESET_EDITED,
                            IPRADIO_PRESET_ARG(s_mode, s_cell));
    }
    if (r == IPRADIO_PEDIT_OK) {
        /* Подтверждение - новое имя на полосе ячеек, говорить
         * об успехе отдельно незачем. */
        ESP_LOGI(TAG, "ячейка %d переименована", s_cell);
        return;
    }

    ESP_LOGW(TAG, "ячейка %d: переименовать не удалось (%d)", s_cell, (int) r);
    reopen_with_note(pedit_text(r));
}

static void do_rename(void)
{
    char title[96];
    snprintf(title, sizeof(title), "Название для ячейки П%d", s_cell);

    /* Меню убираем ДО клавиатуры: после ввода человек должен попасть
     * на главный экран, а не обратно в плашку. */
    ipradio_presetmenu_close();

    /* Начинаем с того, что записано: чаще имя правят, а не набирают
     * заново. */
    ipradio_keyboard_open(title, s_preset.name, false, on_renamed, NULL);
}

static void do_delete(void)
{
    ipradio_pedit_t r = ipradio_preset_clear(s_mode, s_cell);

    if (r == IPRADIO_PEDIT_OK || r == IPRADIO_PEDIT_WRITE_FAILED) {
        ipradio_post_simple(IPRADIO_EV_PRESET_EDITED,
                            IPRADIO_PRESET_ARG(s_mode, s_cell));
    }
    if (r == IPRADIO_PEDIT_OK || r == IPRADIO_PEDIT_EMPTY) {
        ESP_LOGI(TAG, "ячейка %d очищена", s_cell);
        ipradio_presetmenu_close();
        return;
    }

    ESP_LOGW(TAG, "ячейка %d: очистить не удалось (%d)", s_cell, (int) r);
    ipradio_storage_get_preset(s_mode, s_cell, &s_preset);
    show_actions(pedit_text(r));
}

static void do_replace(void)
{
    if (is_net()) {
        /* Поиск по каталогу, как из меню прибора, только найденное
         * ляжет в ЭТУ ячейку и сразу заиграет. */
        int cell = s_cell;
        ipradio_presetmenu_close();
        ipradio_search_ui_open_into(cell, NULL, NULL);
        return;
    }

    /* Эфир: записать частоту, на которой стоит приёмник. Это то же, что
     * удержание кнопки пресета (§6.2), и идёт тем же путём - через
     * автомат: только он знает настоящую частоту и имя из RDS.
     *
     * Автомат пишет в банк ТЕКУЩЕГО режима. Если режим успели
     * переключить, пока открыто меню, запись ушла бы не туда. */
    ipradio_snapshot_t snap;
    ipradio_get(&snap);
    if (snap.mode != IPRADIO_MODE_FM) {
        show_actions("Прибор уже не в эфире - записать частоту нельзя.");
        return;
    }

    ipradio_post_simple(IPRADIO_EV_PRESET_HOLD, s_cell);
    ESP_LOGI(TAG, "в ячейку %d записывается текущая частота", s_cell);
    ipradio_presetmenu_close();
}

static void fire(act_t act)
{
    switch (act) {
    case ACT_RENAME:     do_rename();        break;
    case ACT_REPLACE:    do_replace();       break;
    case ACT_ASK_DELETE: show_question();    break;
    case ACT_DELETE:     do_delete();        break;
    case ACT_BACK:       show_actions(NULL); break;
    case ACT_CLOSE:
    default:             ipradio_presetmenu_close(); break;
    }
}

static void on_button_click(lv_event_t *e)
{
    int idx = (int) (intptr_t) lv_event_get_user_data(e);
    if (s_visible && idx >= 0 && idx < s_btn_count) {
        s_focus = idx;
        fire(s_btn_act[idx]);
    }
}

/* Касание мимо плашки - отказ, как кнопка «Отмена». */
static void on_shade_click(lv_event_t *e)
{
    if (lv_event_get_target(e) == s_shade) {
        ipradio_presetmenu_close();
    }
}

/* ------------------------------------------------------------------ *
 *  Сборка
 * ------------------------------------------------------------------ */

esp_err_t ipradio_presetmenu_init(lv_obj_t *parent)
{
    /* Затемнение неполное, как у диалогов: полоса ячеек должна
     * просвечивать - видно, с какой ячейкой работаешь. */
    s_shade = lv_obj_create(parent);
    lv_obj_set_size(s_shade, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_shade, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_shade, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_shade, 0, 0);
    lv_obj_set_style_pad_all(s_shade, 0, 0);
    lv_obj_set_style_radius(s_shade, 0, 0);
    lv_obj_clear_flag(s_shade, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_shade, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_shade, on_shade_click, LV_EVENT_CLICKED, NULL);

    s_panel = ipradio_ui_panel(s_shade, MENU_W, LV_SIZE_CONTENT,
                               COL_SURFACE, COL_BORDER, 18);
    lv_obj_center(s_panel);
    lv_obj_set_style_pad_all(s_panel, 28, 0);
    lv_obj_set_flex_flow(s_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_panel, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_panel, 10, 0);
    /* Плашка сама кликабельна (так создаёт LVGL), поэтому касание
     * по ней, мимо кнопок, до затемнения не доходит и меню
     * не закрывает. */

    s_head = ipradio_ui_label(s_panel, ipradio_font_22, COL_TEXT_DIM, "");

    s_name = ipradio_ui_label(s_panel, ipradio_font_40, COL_TEXT, "");
    lv_obj_set_width(s_name, MENU_W - 56);
    lv_label_set_long_mode(s_name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_name, LV_TEXT_ALIGN_CENTER, 0);

    s_detail = ipradio_ui_label(s_panel, ipradio_font_16, COL_TEXT_DIM, "");
    lv_obj_set_width(s_detail, MENU_W - 56);
    lv_label_set_long_mode(s_detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_bottom(s_detail, 8, 0);

    for (int i = 0; i < MENU_MAX_BTN; i++) {
        s_btn[i] = ipradio_ui_panel(s_panel, MENU_W - 56, MENU_BTN_H,
                                    COL_SURFACE, COL_BORDER, 12);
        lv_obj_add_flag(s_btn[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_btn[i], on_button_click, LV_EVENT_CLICKED,
                            (void *) (intptr_t) i);

        s_btn_text[i] = ipradio_ui_label(s_btn[i], ipradio_font_28,
                                         COL_TEXT, "");
        lv_obj_center(s_btn_text[i]);
        lv_obj_add_flag(s_btn[i], LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_add_flag(s_shade, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "меню ячейки готово");
    return ESP_OK;
}

/* ------------------------------------------------------------------ *
 *  Открытие и органы управления
 * ------------------------------------------------------------------ */

void ipradio_presetmenu_open(const ipradio_snapshot_t *snap, int cell)
{
    if (!s_shade || !snap || cell < 1 || cell > IPRADIO_PRESET_MAX) {
        return;
    }

    s_mode     = snap->mode;
    s_cell     = cell;
    s_freq_khz = snap->freq_khz;

    /* Одна ячейка, не весь банк: полная копия хранилища - около пяти
     * килобайт, а зовёмся из задачи lvgl. */
    ipradio_storage_get_preset(s_mode, s_cell, &s_preset);

    show_actions(NULL);
    show();
}

bool ipradio_presetmenu_visible(void)
{
    return s_visible;
}

void ipradio_presetmenu_move(int delta)
{
    if (!s_visible || s_btn_count <= 0) {
        return;
    }
    /* Упирается в края, как в диалогах: на трёх-четырёх кнопках заворот
     * только путает. */
    s_focus += delta;
    if (s_focus < 0) {
        s_focus = 0;
    } else if (s_focus >= s_btn_count) {
        s_focus = s_btn_count - 1;
    }
    paint_focus();
}

void ipradio_presetmenu_select(void)
{
    if (s_visible && s_focus >= 0 && s_focus < s_btn_count) {
        fire(s_btn_act[s_focus]);
    }
}

void ipradio_presetmenu_back(void)
{
    if (!s_visible) {
        return;
    }
    /* Шаг назад, а не выход целиком: «Назад» из вопроса - к действиям. */
    if (s_asking) {
        show_actions(NULL);
    } else {
        ipradio_presetmenu_close();
    }
}
