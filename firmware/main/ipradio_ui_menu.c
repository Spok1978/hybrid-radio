/*
 * ipradio_ui_menu.c — меню настроек, экран 05.
 *
 * Замысел — в ipradio_ui_menu.h.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"

#include "bsp/display.h"

#include "ipradio_fonts.h"
#include "ipradio_ui_menu.h"
#include "ipradio_ui_theme.h"

static const char *TAG = "ui.menu";

#define ROW_H        84
#define SIDE_PAD     48
#define VALUE_MAX    40

/* Название и краткое пояснение. Пояснение мелким шрифтом под названием:
 * «Wi-Fi» само по себе не говорит, что там настраивается, а второй
 * заход в пункт ради выяснения — это ровно то, чего экран должен
 * избегать. */
static const struct {
    const char *title;
    const char *hint;
} ITEMS[IPRADIO_MENU_ITEM_COUNT] = {
    [IPRADIO_MENU_TUNE_FM]      = { "Настройка эфира",
                                    "шкала, автопоиск, запись в пресеты" },
    [IPRADIO_MENU_FIND_NET]     = { "Поиск интернет-станций",
                                    "по названию станции" },
    [IPRADIO_MENU_WIFI]         = { "Wi-Fi",
                                    "сети и пароли" },
    [IPRADIO_MENU_CLOCK]        = { "Часы",
                                    "NTP или вручную, часовой пояс, формат" },
    [IPRADIO_MENU_BRIGHTNESS]   = { "Яркость экрана",
                                    "в процентах" },
    [IPRADIO_MENU_STATIONS]     = { "Список станций",
                                    "переименование, удаление, пресеты" },
    [IPRADIO_MENU_DIAGNOSTICS]  = { "Состояние прибора",
                                    "сеть, карта, питание, версия прошивки" },
};

static lv_obj_t *s_screen;
static lv_obj_t *s_list;
static lv_obj_t *s_row[IPRADIO_MENU_ITEM_COUNT];
static lv_obj_t *s_value[IPRADIO_MENU_ITEM_COUNT];
static lv_obj_t *s_title[IPRADIO_MENU_ITEM_COUNT];
static lv_obj_t *s_hint[IPRADIO_MENU_ITEM_COUNT];

static int  s_focus;
static bool s_visible;

static ipradio_menu_cb_t s_cb;
static ipradio_menu_cb_t s_on_close;
static void             *s_ctx;

/* ------------------------------------------------------------------ *
 *  Выделение
 * ------------------------------------------------------------------ */

static void paint_focus(void)
{
    for (int i = 0; i < IPRADIO_MENU_ITEM_COUNT; i++) {
        bool on = (i == s_focus);

        lv_obj_set_style_bg_color(s_row[i], on ? lv_color_hex(0x1e2228)
                                               : COL_SURFACE, 0);
        lv_obj_set_style_border_color(s_row[i], on ? COL_AMBER : COL_BORDER, 0);
        lv_obj_set_style_border_width(s_row[i], on ? 2 : 1, 0);
        lv_obj_set_style_text_color(s_title[i], on ? COL_TEXT : COL_TEXT_DIM, 0);
        lv_obj_set_style_text_color(s_hint[i],
                                    on ? COL_TEXT_DIM : COL_TEXT_FAINT, 0);
    }

    /* Выделенная строка подтягивается в видимую часть сама: пунктов
     * больше, чем помещается, и человек не должен догадываться,
     * что список прокручивается. */
    lv_obj_scroll_to_view(s_row[s_focus], LV_ANIM_ON);
}

static void on_row_click(lv_event_t *e)
{
    int idx = (int) (intptr_t) lv_event_get_user_data(e);
    if (idx < 0 || idx >= IPRADIO_MENU_ITEM_COUNT) {
        return;
    }
    s_focus = idx;
    paint_focus();
    ipradio_menu_select();
}

/* ------------------------------------------------------------------ *
 *  Сборка
 * ------------------------------------------------------------------ */

esp_err_t ipradio_menu_init(lv_obj_t *parent)
{
    s_screen = lv_obj_create(parent);
    lv_obj_set_size(s_screen, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_screen, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_radius(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *head = ipradio_ui_label(s_screen, ipradio_font_28,
                                      COL_TEXT, "Настройки");
    lv_obj_align(head, LV_ALIGN_TOP_LEFT, SIDE_PAD, 36);

    s_list = lv_obj_create(s_screen);
    lv_obj_set_size(s_list, LV_PCT(100), 720 - 150 - 60);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 106);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_hor(s_list, SIDE_PAD, 0);
    lv_obj_set_style_pad_ver(s_list, 0, 0);
    lv_obj_set_style_pad_row(s_list, 10, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);

    for (int i = 0; i < IPRADIO_MENU_ITEM_COUNT; i++) {
        s_row[i] = ipradio_ui_panel(s_list, LV_PCT(100), ROW_H,
                                    COL_SURFACE, COL_BORDER, 12);
        lv_obj_set_style_pad_hor(s_row[i], 24, 0);
        lv_obj_add_flag(s_row[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_row[i], on_row_click, LV_EVENT_CLICKED,
                            (void *) (intptr_t) i);

        s_title[i] = ipradio_ui_label(s_row[i], ipradio_font_22,
                                      COL_TEXT_DIM, ITEMS[i].title);
        lv_obj_align(s_title[i], LV_ALIGN_LEFT_MID, 0, -13);

        s_hint[i] = ipradio_ui_label(s_row[i], ipradio_font_14,
                                     COL_TEXT_FAINT, ITEMS[i].hint);
        lv_obj_align(s_hint[i], LV_ALIGN_LEFT_MID, 0, 15);

        /* Значение справа — то, ради чего экран так свёрстан.
         * Янтарём, потому что это единственное на строке, что меняется. */
        s_value[i] = ipradio_ui_label(s_row[i], ipradio_font_20,
                                      COL_AMBER, "");
        lv_obj_align(s_value[i], LV_ALIGN_RIGHT_MID, 0, 0);
    }

    lv_obj_t *hints = ipradio_ui_label(s_screen, ipradio_font_14,
        COL_TEXT_FAINT,
        "Энкодер 1 — выбор   •   нажатие — войти   •   "
        "нажатие энкодера 2 — назад");
    lv_obj_align(hints, LV_ALIGN_BOTTOM_MID, 0, -28);

    lv_obj_add_flag(s_screen, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "меню готово: %d пунктов", IPRADIO_MENU_ITEM_COUNT);
    return ESP_OK;
}

/* ------------------------------------------------------------------ *
 *  Значения справа
 * ------------------------------------------------------------------ */

void ipradio_menu_update(const ipradio_snapshot_t *s)
{
    if (!s_screen || !s_visible) {
        return;
    }

    char buf[VALUE_MAX];

    /* Эфир: текущая частота и диапазон. */
    snprintf(buf, sizeof(buf), "%u.%02u МГц",
             (unsigned) (s->freq_khz / 1000),
             (unsigned) ((s->freq_khz % 1000) / 10));
    lv_label_set_text(s_value[IPRADIO_MENU_TUNE_FM], buf);

    /* Интернет: доступен ли поиск вообще. Написать «нет сети» прямо
     * здесь честнее, чем пустить человека в пункт и показать ошибку
     * уже внутри. */
    lv_label_set_text(s_value[IPRADIO_MENU_FIND_NET],
        (s->net == IPRADIO_NET_CONNECTED) ? "доступен" : "нет сети");
    lv_obj_set_style_text_color(s_value[IPRADIO_MENU_FIND_NET],
        (s->net == IPRADIO_NET_CONNECTED) ? COL_AMBER : COL_TEXT_FAINT, 0);

    /* Wi-Fi: состояние словами, а не значком. Значок пришлось бы
     * запоминать, слово — нет. */
    const char *net_text;
    lv_color_t  net_color = COL_AMBER;
    switch (s->net) {
    case IPRADIO_NET_NOT_CONFIGURED:
        net_text  = "не настроен";
        net_color = COL_TEXT_FAINT;
        break;
    case IPRADIO_NET_DISCONNECTED:
        net_text  = "нет связи";
        net_color = COL_RED;
        break;
    case IPRADIO_NET_CONNECTING:
        net_text  = "подключение";
        break;
    case IPRADIO_NET_CONNECTED:
    default:
        net_text  = "подключён";
        net_color = COL_GREEN;
        break;
    }
    lv_label_set_text(s_value[IPRADIO_MENU_WIFI], net_text);
    lv_obj_set_style_text_color(s_value[IPRADIO_MENU_WIFI], net_color, 0);

    /* Часы: показываем текущее время — заодно видно, синхронизировались
     * ли они вообще. */
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(buf, sizeof(buf), "%H:%M", &tmv);
    lv_label_set_text(s_value[IPRADIO_MENU_CLOCK], buf);

    int br = bsp_display_brightness_get();
    if (br >= 0) {
        snprintf(buf, sizeof(buf), "%d %%", br);
        lv_label_set_text(s_value[IPRADIO_MENU_BRIGHTNESS], buf);
    }

    /* Число станций сюда придёт из хранилища, когда меню начнут
     * открывать по-настоящему; пока строка пустая, а не выдуманная. */
    lv_label_set_text(s_value[IPRADIO_MENU_STATIONS], "");

    lv_label_set_text(s_value[IPRADIO_MENU_DIAGNOSTICS], "");
}

/* ------------------------------------------------------------------ *
 *  Открытие и закрытие
 * ------------------------------------------------------------------ */

void ipradio_menu_open(ipradio_menu_cb_t cb, ipradio_menu_cb_t on_close,
                       void *ctx)
{
    if (!s_screen) {
        return;
    }

    s_cb       = cb;
    s_on_close = on_close;
    s_ctx      = ctx;
    s_focus    = 0;
    s_visible  = true;

    paint_focus();

    ipradio_snapshot_t snap;
    ipradio_get(&snap);
    ipradio_menu_update(&snap);

    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_screen);
}

void ipradio_menu_close(void)
{
    if (!s_screen || !s_visible) {
        return;
    }

    ipradio_menu_cb_t cb = s_on_close;
    void *ctx = s_ctx;

    s_visible  = false;
    s_cb       = NULL;
    s_on_close = NULL;
    s_ctx      = NULL;

    lv_obj_add_flag(s_screen, LV_OBJ_FLAG_HIDDEN);

    /* Обратный вызов последним: он снимает перехват органов,
     * и до этого момента экран должен быть уже убран. */
    if (cb) {
        cb(IPRADIO_MENU_ITEM_COUNT, ctx);
    }
}

bool ipradio_menu_visible(void)
{
    return s_visible;
}

/* ------------------------------------------------------------------ *
 *  Органы управления
 * ------------------------------------------------------------------ */

void ipradio_menu_move(int delta)
{
    if (!s_visible || delta == 0) {
        return;
    }

    /* Здесь список заворачивается по кругу — в отличие от диалога.
     * Разница осмысленная: в диалоге две-три кнопки и заворот путает,
     * а список из семи пунктов человек крутит, и упор в край
     * заставляет крутить обратно. */
    s_focus = (s_focus + delta) % IPRADIO_MENU_ITEM_COUNT;
    if (s_focus < 0) {
        s_focus += IPRADIO_MENU_ITEM_COUNT;
    }
    paint_focus();
}

void ipradio_menu_select(void)
{
    if (!s_visible || !s_cb) {
        return;
    }
    s_cb((ipradio_menu_item_t) s_focus, s_ctx);
}

void ipradio_menu_back(void)
{
    ipradio_menu_close();
}
