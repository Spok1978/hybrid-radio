/*
 * ipradio_ui_brightness.c — яркость экрана.
 *
 * Замысел — в ipradio_ui_brightness.h.
 */

#include <stdio.h>

#include "esp_log.h"

#include "bsp/display.h"

#include "ipradio_fonts.h"
#include "ipradio_storage.h"
#include "ipradio_ui_brightness.h"
#include "ipradio_ui_theme.h"

static const char *TAG = "ui.bright";

static lv_obj_t *s_screen;
static lv_obj_t *s_value;
static lv_obj_t *s_bar;
static lv_obj_t *s_fill;

static int  s_level;
static int  s_level_on_entry;
static bool s_visible;

static void (*s_on_close)(void *ctx);
static void  *s_ctx;

#define BAR_W  760
#define BAR_H   28

/* ------------------------------------------------------------------ *
 *  Показ
 * ------------------------------------------------------------------ */

static void render(void)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d %%", s_level);
    lv_label_set_text(s_value, buf);

    lv_obj_set_width(s_fill, (BAR_W - 4) * s_level / 100);
}

static void apply(int level)
{
    if (level < IPRADIO_BRIGHTNESS_MIN) {
        level = IPRADIO_BRIGHTNESS_MIN;
    }
    if (level > 100) {
        level = 100;
    }

    s_level = level;
    bsp_display_brightness_set(level);
    render();
}

/* ------------------------------------------------------------------ *
 *  Сборка
 * ------------------------------------------------------------------ */

esp_err_t ipradio_brightness_ui_init(lv_obj_t *parent)
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
                                      COL_TEXT, "Яркость экрана");
    lv_obj_align(head, LV_ALIGN_TOP_MID, 0, 90);

    s_value = ipradio_ui_label(s_screen, ipradio_font_48b, COL_AMBER, "");
    lv_obj_align(s_value, LV_ALIGN_TOP_MID, 0, 190);

    s_bar = ipradio_ui_panel(s_screen, BAR_W, BAR_H,
                             COL_SURFACE, COL_BORDER, BAR_H / 2);
    lv_obj_align(s_bar, LV_ALIGN_CENTER, 0, 60);
    lv_obj_set_style_pad_all(s_bar, 0, 0);

    /* Заполнение — отдельный прямоугольник внутри полосы, а не
     * lv_bar: нам от него нужна только ширина, а стиль должен
     * совпадать с остальными экранами. */
    s_fill = lv_obj_create(s_bar);
    lv_obj_set_height(s_fill, BAR_H - 8);
    lv_obj_set_style_bg_color(s_fill, COL_AMBER, 0);
    lv_obj_set_style_bg_opa(s_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_fill, 0, 0);
    lv_obj_set_style_radius(s_fill, (BAR_H - 8) / 2, 0);
    lv_obj_clear_flag(s_fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_fill, LV_ALIGN_LEFT_MID, 2, 0);

    lv_obj_t *hints = ipradio_ui_label(s_screen, ipradio_font_20,
        COL_TEXT,
        "Регулятор 1 — яркость   •   нажатие — принять   •   "
        "нажатие регулятора 2 — вернуть прежнюю");
    lv_obj_align(hints, LV_ALIGN_BOTTOM_MID, 0, -28);

    lv_obj_add_flag(s_screen, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "экран яркости готов");
    return ESP_OK;
}

/* ------------------------------------------------------------------ *
 *  Открытие и закрытие
 * ------------------------------------------------------------------ */

void ipradio_brightness_ui_open(void (*on_close)(void *ctx), void *ctx)
{
    if (!s_screen) {
        return;
    }

    s_on_close = on_close;
    s_ctx      = ctx;
    s_visible  = true;

    int cur = bsp_display_brightness_get();
    if (cur < IPRADIO_BRIGHTNESS_MIN || cur > 100) {
        cur = 80;
    }
    s_level_on_entry = cur;
    apply(cur);

    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_screen);
}

static void finish(bool keep)
{
    if (!s_screen || !s_visible) {
        return;
    }

    if (keep) {
        ipradio_store_t st;
        ipradio_storage_get(&st);
        st.settings.brightness = (uint8_t) s_level;

        if (ipradio_storage_save_settings(&st.settings) != ESP_OK) {
            /* Записать не вышло. Яркость всё равно оставляем такой,
             * какую человек выбрал: до выключения она будет верной,
             * а спорить с ним из-за карты незачем. */
            ESP_LOGW(TAG, "яркость не сохранилась на карту");
        }
    } else {
        apply(s_level_on_entry);
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

void ipradio_brightness_ui_close(void)
{
    finish(false);
}

bool ipradio_brightness_ui_visible(void)
{
    return s_visible;
}

/* ------------------------------------------------------------------ *
 *  Органы управления
 * ------------------------------------------------------------------ */

void ipradio_brightness_ui_move(int delta)
{
    if (!s_visible || delta == 0) {
        return;
    }
    apply(s_level + delta * IPRADIO_BRIGHTNESS_STEP);
}

void ipradio_brightness_ui_select(void)
{
    finish(true);
}

void ipradio_brightness_ui_back(void)
{
    finish(false);
}
