/*
 * ipradio_ui_stations.c — список станций.
 *
 * Замысел — в ipradio_ui_stations.h.
 */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "ipradio_fonts.h"
#include "ipradio_state.h"
#include "ipradio_storage.h"
#include "ipradio_ui_keyboard.h"
#include "ipradio_ui_stations.h"
#include "ipradio_ui_theme.h"

static const char *TAG = "ui.stations";

#define ROW_H     76
#define SIDE_PAD  48

static lv_obj_t *s_screen;
static lv_obj_t *s_status;
static lv_obj_t *s_list;
static lv_obj_t *s_row[IPRADIO_PRESET_MAX];
static lv_obj_t *s_row_cell[IPRADIO_PRESET_MAX];
static lv_obj_t *s_row_name[IPRADIO_PRESET_MAX];
static lv_obj_t *s_row_info[IPRADIO_PRESET_MAX];

static ipradio_store_t s_store;
static int  s_focus;
static bool s_visible;
static int  s_editing = -1;   /* какую ячейку переименовываем */

static void (*s_on_close)(void *ctx);
static void  *s_ctx;

/* ------------------------------------------------------------------ *
 *  Отрисовка
 * ------------------------------------------------------------------ */

static void paint_focus(void)
{
    for (int i = 0; i < IPRADIO_PRESET_MAX; i++) {
        bool on = (i == s_focus);
        bool fm = (s_store.presets[i].type == IPRADIO_MODE_FM);
        lv_color_t accent = fm ? COL_AMBER : COL_CYAN;

        lv_obj_set_style_bg_color(s_row[i], on ? lv_color_hex(0x1e2228)
                                               : COL_SURFACE, 0);
        lv_obj_set_style_border_color(s_row[i], on ? accent : COL_BORDER, 0);
        lv_obj_set_style_border_width(s_row[i], on ? 2 : 1, 0);
        lv_obj_set_style_text_color(s_row_name[i],
                                    on ? COL_TEXT : COL_TEXT_DIM, 0);
    }
    lv_obj_scroll_to_view(s_row[s_focus], LV_ANIM_ON);
}

static void render(void)
{
    for (int i = 0; i < IPRADIO_PRESET_MAX; i++) {
        const ipradio_preset_t *p = &s_store.presets[i];

        char cell[8];
        snprintf(cell, sizeof(cell), "П%d", i + 1);
        lv_label_set_text(s_row_cell[i], cell);

        if (!p->used) {
            lv_label_set_text(s_row_name[i], "— свободно —");
            lv_label_set_text(s_row_info[i], "");
            lv_obj_set_style_text_color(s_row_name[i], COL_TEXT_FAINT, 0);
            lv_obj_set_style_text_color(s_row_cell[i], COL_TEXT_FAINT, 0);
            continue;
        }

        bool fm = (p->type == IPRADIO_MODE_FM);
        lv_obj_set_style_text_color(s_row_cell[i],
                                    fm ? COL_AMBER : COL_CYAN, 0);

        /* Безымянная станция — обычное дело после автопоиска. Пишем
         * это прямо и предлагаем действие, а не оставляем пустоту:
         * пустая строка выглядит как ошибка записи. */
        if (p->name[0]) {
            lv_label_set_text(s_row_name[i], p->name);
        } else {
            lv_label_set_text(s_row_name[i], "без названия");
        }
        lv_obj_set_style_text_color(s_row_name[i], COL_TEXT_DIM, 0);

        char info[160];
        if (fm) {
            snprintf(info, sizeof(info), "эфир   •   %u.%02u МГц",
                     (unsigned) (p->freq_khz / 1000),
                     (unsigned) ((p->freq_khz % 1000) / 10));
        } else {
            snprintf(info, sizeof(info), "интернет   •   %s",
                     p->url[0] ? p->url : "адрес не записан");
        }
        lv_label_set_text(s_row_info[i], info);
    }

    paint_focus();
}

/* ------------------------------------------------------------------ *
 *  Переименование и удаление
 * ------------------------------------------------------------------ */

static void on_renamed(const char *text, void *ctx)
{
    (void) ctx;

    int cell = s_editing;
    s_editing = -1;

    if (!text || cell < 0 || cell >= IPRADIO_PRESET_MAX) {
        return;                       /* отказались */
    }

    snprintf(s_store.presets[cell].name, IPRADIO_NAME_MAX, "%s", text);

    if (ipradio_storage_save(&s_store) != ESP_OK) {
        lv_label_set_text(s_status, "Не удалось записать на карту");
        /* Читаем банк заново: в памяти у нас теперь не то, что
         * на карте, и показывать несохранённое имя нельзя - человек
         * решит, что всё получилось. */
        ipradio_storage_get(&s_store);
    } else {
        lv_label_set_text(s_status, "Название сохранено");
        ESP_LOGI(TAG, "ячейка %d переименована в «%s»", cell + 1, text);
    }

    render();
}

static void rename_cell(int cell)
{
    if (!s_store.presets[cell].used) {
        lv_label_set_text(s_status,
            "Ячейка свободна. Записать в неё станцию можно долгим "
            "нажатием её кнопки во время воспроизведения.");
        return;
    }

    s_editing = cell;

    char title[96];
    snprintf(title, sizeof(title), "Название для ячейки П%d", cell + 1);

    /* Показываем то, что уже записано: чаще всего имя надо поправить,
     * а не набрать заново. */
    ipradio_keyboard_open(title, s_store.presets[cell].name, false,
                          on_renamed, NULL);
}

static void delete_cell(int cell)
{
    if (!s_store.presets[cell].used) {
        return;
    }

    /* Подтверждения нет намеренно. Удаление здесь дёшево обратимо:
     * эфирную станцию вернёт автопоиск, интернетную - поиск
     * по каталогу. Диалог на каждое нажатие стоил бы дороже, чем
     * редкая ошибка, а долгое нажатие само по себе не случайно. */
    ESP_LOGI(TAG, "ячейка %d очищена", cell + 1);
    memset(&s_store.presets[cell], 0, sizeof(s_store.presets[cell]));

    if (ipradio_storage_save(&s_store) != ESP_OK) {
        lv_label_set_text(s_status, "Не удалось записать на карту");
        ipradio_storage_get(&s_store);
    } else {
        lv_label_set_text(s_status, "Ячейка освобождена");
    }

    render();
}

/* ------------------------------------------------------------------ *
 *  Сборка
 * ------------------------------------------------------------------ */

static void on_row_click(lv_event_t *e)
{
    int idx = (int) (intptr_t) lv_event_get_user_data(e);
    if (idx < 0 || idx >= IPRADIO_PRESET_MAX) {
        return;
    }
    s_focus = idx;
    paint_focus();
    rename_cell(idx);
}

esp_err_t ipradio_stations_ui_init(lv_obj_t *parent)
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
                                      COL_TEXT, "Список станций");
    lv_obj_align(head, LV_ALIGN_TOP_LEFT, SIDE_PAD, 30);

    s_status = ipradio_ui_label(s_screen, ipradio_font_16, COL_TEXT_DIM, "");
    lv_obj_set_width(s_status, LV_PCT(88));
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, SIDE_PAD, 74);

    s_list = lv_obj_create(s_screen);
    lv_obj_set_size(s_list, LV_PCT(100), 720 - 190 - 60);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 124);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_hor(s_list, SIDE_PAD, 0);
    lv_obj_set_style_pad_ver(s_list, 0, 0);
    lv_obj_set_style_pad_row(s_list, 8, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);

    for (int i = 0; i < IPRADIO_PRESET_MAX; i++) {
        s_row[i] = ipradio_ui_panel(s_list, LV_PCT(100), ROW_H,
                                    COL_SURFACE, COL_BORDER, 12);
        lv_obj_set_style_pad_hor(s_row[i], 22, 0);
        lv_obj_add_flag(s_row[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_row[i], on_row_click, LV_EVENT_CLICKED,
                            (void *) (intptr_t) i);

        /* Номер ячейки слева: он же написан на физической кнопке,
         * и связь между строкой и кнопкой должна читаться сразу. */
        s_row_cell[i] = ipradio_ui_label(s_row[i], ipradio_font_22,
                                         COL_TEXT_FAINT, "");
        lv_obj_align(s_row_cell[i], LV_ALIGN_LEFT_MID, 0, 0);

        s_row_name[i] = ipradio_ui_label(s_row[i], ipradio_font_22,
                                         COL_TEXT_DIM, "");
        lv_obj_set_width(s_row_name[i], LV_PCT(80));
        lv_label_set_long_mode(s_row_name[i], LV_LABEL_LONG_DOT);
        lv_obj_align(s_row_name[i], LV_ALIGN_LEFT_MID, 64, -11);

        s_row_info[i] = ipradio_ui_label(s_row[i], ipradio_font_14,
                                         COL_TEXT_FAINT, "");
        lv_obj_set_width(s_row_info[i], LV_PCT(80));
        lv_label_set_long_mode(s_row_info[i], LV_LABEL_LONG_DOT);
        lv_obj_align(s_row_info[i], LV_ALIGN_LEFT_MID, 64, 16);
    }

    lv_obj_t *hints = ipradio_ui_label(s_screen, ipradio_font_14,
        COL_TEXT_FAINT,
        "Энкодер 1 — выбор   •   нажатие — переименовать   •   "
        "долгое нажатие — удалить   •   энкодер 2 — назад");
    lv_obj_align(hints, LV_ALIGN_BOTTOM_MID, 0, -28);

    lv_obj_add_flag(s_screen, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "экран списка станций готов");
    return ESP_OK;
}

/* ------------------------------------------------------------------ *
 *  Открытие и закрытие
 * ------------------------------------------------------------------ */

void ipradio_stations_ui_open(void (*on_close)(void *ctx), void *ctx)
{
    if (!s_screen) {
        return;
    }

    s_on_close = on_close;
    s_ctx      = ctx;
    s_visible  = true;
    s_focus    = 0;
    s_editing  = -1;

    /* Банк читаем заново при каждом открытии: между открытиями его
     * мог поменять автопоиск, поиск по каталогу или долгое нажатие
     * кнопки пресета. */
    ipradio_storage_get(&s_store);

    lv_label_set_text(s_status,
        ipradio_storage_ready() ? "" : "Карты нет — изменения не сохранятся");

    render();

    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_screen);
}

void ipradio_stations_ui_close(void)
{
    if (!s_screen || !s_visible) {
        return;
    }

    void (*cb)(void *) = s_on_close;
    void *ctx = s_ctx;

    s_visible  = false;
    s_on_close = NULL;
    s_ctx      = NULL;

    lv_obj_add_flag(s_screen, LV_OBJ_FLAG_HIDDEN);

    if (cb) {
        cb(ctx);
    }
}

bool ipradio_stations_ui_visible(void)
{
    return s_visible;
}

/* ------------------------------------------------------------------ *
 *  Органы управления
 * ------------------------------------------------------------------ */

void ipradio_stations_ui_move(int delta)
{
    if (!s_visible || delta == 0) {
        return;
    }

    s_focus = (s_focus + delta) % IPRADIO_PRESET_MAX;
    if (s_focus < 0) {
        s_focus += IPRADIO_PRESET_MAX;
    }
    paint_focus();
}

void ipradio_stations_ui_select(void)
{
    if (s_visible && s_focus >= 0 && s_focus < IPRADIO_PRESET_MAX) {
        rename_cell(s_focus);
    }
}

void ipradio_stations_ui_long_select(void)
{
    if (s_visible && s_focus >= 0 && s_focus < IPRADIO_PRESET_MAX) {
        delete_cell(s_focus);
    }
}

void ipradio_stations_ui_back(void)
{
    ipradio_stations_ui_close();
}
