/*
 * ipradio_ui_dialog.c — три состояния недоступности интернет-радио.
 *
 * Замысел и обоснование — в ipradio_ui_dialog.h.
 */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "ipradio_fonts.h"
#include "ipradio_ui_dialog.h"
#include "ipradio_ui_theme.h"

static const char *TAG = "ui.dlg";

#define DIALOG_W        720
#define DIALOG_MAX_BTN    3

/* Плашка занимает середину экрана, но не весь: сверху остаётся видна
 * строка состояния, снизу — что играет. Это и есть «поверх эфира»
 * из §5.2: человек видит, что прибор работает. */
#define DIALOG_TOP      170

static lv_obj_t *s_shade;       /* затемнение фона, но не полное */
static lv_obj_t *s_panel;
static lv_obj_t *s_icon;
static lv_obj_t *s_title;
static lv_obj_t *s_body;
static lv_obj_t *s_footer;      /* «эфир при этом не прерывался» */

static lv_obj_t *s_btn[DIALOG_MAX_BTN];
static lv_obj_t *s_btn_text[DIALOG_MAX_BTN];
static ipradio_dialog_action_t s_btn_action[DIALOG_MAX_BTN];
static int s_btn_count;
static int s_focus;

static ipradio_dialog_kind_t s_kind;
static ipradio_dialog_cb_t   s_cb;
static void                 *s_ctx;

/* ------------------------------------------------------------------ *
 *  Выделение
 * ------------------------------------------------------------------ */

static void paint_focus(void)
{
    for (int i = 0; i < DIALOG_MAX_BTN; i++) {
        if (!s_btn[i]) {
            continue;
        }
        bool on = (i == s_focus) && (i < s_btn_count);

        /* Выделение делается заливкой и рамкой, а не только цветом
         * текста: на янтарно-циановой палитре одного цвета текста
         * мало, чтобы выделение читалось через комнату. */
        lv_obj_set_style_bg_color(s_btn[i], on ? COL_TEXT : COL_SURFACE, 0);
        lv_obj_set_style_border_color(s_btn[i], on ? COL_TEXT : COL_BORDER, 0);
        lv_obj_set_style_border_width(s_btn[i], on ? 2 : 1, 0);
        lv_obj_set_style_text_color(s_btn_text[i],
                                    on ? COL_BG : COL_TEXT_DIM, 0);
    }
}

static void fire(ipradio_dialog_action_t action)
{
    ipradio_dialog_cb_t cb = s_cb;
    void *ctx = s_ctx;

    /* Диалог убирается ДО обратного вызова: обработчик может открыть
     * следующий экран, и он не должен рисоваться под нашей плашкой. */
    ipradio_dialog_hide();

    if (cb) {
        cb(action, ctx);
    }
}

static void on_button_click(lv_event_t *e)
{
    int idx = (int) (intptr_t) lv_event_get_user_data(e);
    if (idx >= 0 && idx < s_btn_count) {
        s_focus = idx;
        fire(s_btn_action[idx]);
    }
}

/* ------------------------------------------------------------------ *
 *  Сборка
 * ------------------------------------------------------------------ */

esp_err_t ipradio_dialog_init(lv_obj_t *parent)
{
    /* Затемнение неполное — 70 %. Полное скрыло бы, что эфир играет,
     * а именно это и надо показать (§5.2, правило 3). */
    s_shade = lv_obj_create(parent);
    lv_obj_set_size(s_shade, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_shade, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_shade, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_shade, 0, 0);
    lv_obj_set_style_pad_all(s_shade, 0, 0);
    lv_obj_set_style_radius(s_shade, 0, 0);
    lv_obj_clear_flag(s_shade, LV_OBJ_FLAG_SCROLLABLE);

    s_panel = ipradio_ui_panel(s_shade, DIALOG_W, LV_SIZE_CONTENT,
                               COL_SURFACE, COL_BORDER, 18);
    lv_obj_align(s_panel, LV_ALIGN_TOP_MID, 0, DIALOG_TOP);
    lv_obj_set_style_pad_all(s_panel, 32, 0);
    lv_obj_set_flex_flow(s_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_panel, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_panel, 14, 0);

    s_icon  = ipradio_ui_label(s_panel, ipradio_font_28, COL_RED, "");
    s_title = ipradio_ui_label(s_panel, ipradio_font_28, COL_TEXT, "");

    s_body = ipradio_ui_label(s_panel, ipradio_font_16, COL_TEXT_DIM, "");
    lv_obj_set_width(s_body, DIALOG_W - 64);
    lv_label_set_long_mode(s_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_body, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *row = lv_obj_create(s_panel);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_top(row, 10, 0);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < DIALOG_MAX_BTN; i++) {
        s_btn[i] = ipradio_ui_panel(row, LV_SIZE_CONTENT, 56,
                                    COL_SURFACE, COL_BORDER, 10);
        lv_obj_set_style_pad_hor(s_btn[i], 22, 0);
        lv_obj_add_flag(s_btn[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_btn[i], on_button_click, LV_EVENT_CLICKED,
                            (void *) (intptr_t) i);

        s_btn_text[i] = ipradio_ui_label(s_btn[i], ipradio_font_16,
                                         COL_TEXT_DIM, "");
        lv_obj_center(s_btn_text[i]);
    }

    /* Строка, ради которой всё и затевалось. Мелким шрифтом, но она
     * есть всегда: «эфир при этом не прерывался». */
    s_footer = ipradio_ui_label(s_panel, ipradio_font_14, COL_TEXT_FAINT, "");
    lv_obj_set_width(s_footer, DIALOG_W - 64);
    lv_label_set_long_mode(s_footer, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_footer, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_add_flag(s_shade, LV_OBJ_FLAG_HIDDEN);
    s_kind = IPRADIO_DIALOG_NONE;

    ESP_LOGI(TAG, "диалоги недоступности сети готовы");
    return ESP_OK;
}

/* ------------------------------------------------------------------ *
 *  Показ
 * ------------------------------------------------------------------ */

static void set_button(int idx, const char *text,
                       ipradio_dialog_action_t action)
{
    lv_label_set_text(s_btn_text[idx], text);
    s_btn_action[idx] = action;
    lv_obj_clear_flag(s_btn[idx], LV_OBJ_FLAG_HIDDEN);
}

void ipradio_dialog_show(ipradio_dialog_kind_t kind, const char *detail,
                         ipradio_dialog_cb_t cb, void *ctx)
{
    if (!s_shade || kind == IPRADIO_DIALOG_NONE) {
        return;
    }

    char buf[192];

    s_kind  = kind;
    s_cb    = cb;
    s_ctx   = ctx;
    s_focus = 0;

    for (int i = 0; i < DIALOG_MAX_BTN; i++) {
        lv_obj_add_flag(s_btn[i], LV_OBJ_FLAG_HIDDEN);
    }

    switch (kind) {

    /* --- Экран 10: сети нет в настройках вовсе -------------------- *
     * Единственный из трёх случаев, где мы ведём в настройку: делать
     * нечего, пока человек не введёт сеть. Отсюда и приглашающий,
     * а не тревожный тон — ничего не сломалось. */
    case IPRADIO_DIALOG_WIFI_NOT_CONFIGURED:
        lv_obj_set_style_text_color(s_icon, COL_CYAN, 0);
        lv_label_set_text(s_icon, "▶");
        lv_label_set_text(s_title, "Wi-Fi ещё не настроен");
        lv_label_set_text(s_body,
            "Интернет-радио станет доступно, как только будет задана сеть. "
            "Настройка занимает минуту: выбрать сеть из списка "
            "и ввести пароль.");
        set_button(0, "Настроить Wi-Fi", IPRADIO_DIALOG_ACT_SETUP_WIFI);
        set_button(1, "Не сейчас",       IPRADIO_DIALOG_ACT_DISMISS);
        s_btn_count = 2;
        break;

    /* --- Экран 11: сеть задана, связи нет ------------------------- *
     * В настройки НЕ ведём намеренно (§5.2): связь скорее всего
     * вернётся сама, а человек, отправленный в настройки, начнёт
     * править то, что и так верно. */
    case IPRADIO_DIALOG_WIFI_NO_LINK:
        lv_obj_set_style_text_color(s_icon, COL_RED, 0);
        lv_label_set_text(s_icon, "•");
        if (detail && detail[0]) {
            snprintf(buf, sizeof(buf), "Нет связи с сетью «%s»", detail);
        } else {
            snprintf(buf, sizeof(buf), "Нет связи с сетью");
        }
        lv_label_set_text(s_title, buf);
        lv_label_set_text(s_body,
            "Идёт переподключение. Настройки менять не нужно — "
            "они в порядке, пропала сама связь. "
            "Интернет-радио включится само, как только сеть вернётся.");
        set_button(0, "Понятно", IPRADIO_DIALOG_ACT_DISMISS);
        s_btn_count = 1;
        break;

    /* --- Экран 12: связь есть, молчит станция --------------------- *
     * Здесь важнее всего снять подозрение с сети: человек, решивший,
     * что виноват Wi-Fi, пойдёт чинить исправный роутер. Поэтому
     * про сеть сказано прямо, и сразу предложен выход. */
    case IPRADIO_DIALOG_STATION_DEAD:
        lv_obj_set_style_text_color(s_icon, COL_RED, 0);
        lv_label_set_text(s_icon, "•");
        if (detail && detail[0]) {
            snprintf(buf, sizeof(buf), "«%s» не отвечает", detail);
        } else {
            snprintf(buf, sizeof(buf), "Станция не отвечает");
        }
        lv_label_set_text(s_title, buf);
        lv_label_set_text(s_body,
            "Wi-Fi работает, дело в самой станции: она могла сменить адрес "
            "или временно не вещать. Попыток подключиться было три.");
        set_button(0, "Другая станция",   IPRADIO_DIALOG_ACT_PICK_STATION);
        set_button(1, "Вернуться в эфир", IPRADIO_DIALOG_ACT_BACK_TO_FM);
        set_button(2, "Понятно",          IPRADIO_DIALOG_ACT_DISMISS);
        s_btn_count = 3;
        break;

    default:
        return;
    }

    lv_label_set_text(s_footer, "Эфир при этом не прерывался — "
                                "он продолжает играть.");

    paint_focus();
    lv_obj_clear_flag(s_shade, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_shade);
}

void ipradio_dialog_hide(void)
{
    if (!s_shade) {
        return;
    }
    lv_obj_add_flag(s_shade, LV_OBJ_FLAG_HIDDEN);
    s_kind = IPRADIO_DIALOG_NONE;
    s_cb   = NULL;
    s_ctx  = NULL;
}

ipradio_dialog_kind_t ipradio_dialog_current(void)
{
    return s_kind;
}

/* ------------------------------------------------------------------ *
 *  Органы управления
 * ------------------------------------------------------------------ */

void ipradio_dialog_move(int delta)
{
    if (s_kind == IPRADIO_DIALOG_NONE || s_btn_count <= 0) {
        return;
    }

    /* Выделение упирается в края, а не заворачивается. На двух-трёх
     * кнопках заворот только путает: непонятно, дошёл ли до конца. */
    s_focus += delta;
    if (s_focus < 0) {
        s_focus = 0;
    } else if (s_focus >= s_btn_count) {
        s_focus = s_btn_count - 1;
    }
    paint_focus();
}

void ipradio_dialog_select(void)
{
    if (s_kind == IPRADIO_DIALOG_NONE || s_focus >= s_btn_count) {
        return;
    }
    fire(s_btn_action[s_focus]);
}
