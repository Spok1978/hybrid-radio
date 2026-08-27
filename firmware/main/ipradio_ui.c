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

#include "ipradio_state.h"
#include "ipradio_ui.h"
#include "ipradio_fonts.h"

static const char *TAG = "ui";

/* Цвета из макетов. */
#define COL_BG          lv_color_hex(0x0e0f11)
#define COL_SURFACE     lv_color_hex(0x16181b)
#define COL_BORDER      lv_color_hex(0x2a2e34)
#define COL_TEXT        lv_color_hex(0xf2f3f5)
#define COL_TEXT_DIM    lv_color_hex(0x9aa1a9)
#define COL_TEXT_FAINT  lv_color_hex(0x5f666e)
#define COL_AMBER       lv_color_hex(0xf5a524)   /* эфир    */
#define COL_CYAN        lv_color_hex(0x3ec5d8)   /* интернет */
#define COL_RED         lv_color_hex(0xf05b52)   /* mute    */

#define PRESET_CELLS    8

static SemaphoreHandle_t  s_lock;      /* защищает снимок ниже */
static ipradio_snapshot_t s_pending;
static volatile bool      s_dirty;

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

/* ------------------------------------------------------------------ *
 *  Сборка экрана
 * ------------------------------------------------------------------ */

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font,
                            lv_color_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text ? text : "");
    return l;
}

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

        s_preset_names[i] = make_label(cell, ipradio_font_16,
                                       COL_TEXT_DIM, "-");
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
                (s->band == IPRADIO_BAND_OIRT) ? "УКВ" : "FM");
        } else {
            snprintf(buf, sizeof(buf), "%u.%02u МГц",
                     (unsigned) (s->freq_khz / 1000),
                     (unsigned) ((s->freq_khz % 1000) / 10));
            lv_label_set_text(s_title, buf);
            lv_label_set_text(s_subtitle,
                (s->band == IPRADIO_BAND_OIRT) ? "УКВ" : "FM");
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

    /* Ячейки пресетов: активная подсвечивается цветом своего типа. */
    for (int i = 0; i < PRESET_CELLS; i++) {
        bool active = (s->active_preset == i + 1);
        lv_obj_set_style_border_color(s_presets[i],
            active ? accent : COL_BORDER, 0);
        lv_obj_set_style_border_width(s_presets[i], active ? 2 : 1, 0);
        lv_obj_set_style_bg_color(s_presets[i],
            active ? (fm ? lv_color_hex(0x2a2110) : lv_color_hex(0x0f2529))
                   : COL_SURFACE, 0);
    }
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

static void ui_task(void *arg)
{
    (void) arg;

    for (;;) {
        if (s_dirty) {
            ipradio_snapshot_t snap;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            snap    = s_pending;
            s_dirty = false;
            xSemaphoreGive(s_lock);

            apply_snapshot(&snap);
        }

        uint32_t next = lv_timer_handler();
        if (next == LV_NO_TIMER_READY) {
            next = 20;
        }
        vTaskDelay(pdMS_TO_TICKS(next > 20 ? 20 : next));
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

    /* Шрифты - раньше всего: первый же make_label к ним обратится. */
    esp_err_t err = ipradio_fonts_init();
    if (err != ESP_OK) {
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

    /* Первая отрисовка по текущему состоянию, чтобы экран не был
     * пустым до первого события. */
    ipradio_snapshot_t snap;
    ipradio_get(&snap);
    apply_snapshot(&snap);

    ipradio_subscribe(on_state, NULL);

    xTaskCreate(ui_task, "ipradio_ui", 6144, NULL, 4, NULL);

    ESP_LOGI(TAG, "интерфейс поднят: главный экран");
    ESP_LOGW(TAG, "надписи латиницей: кириллический шрифт LVGL");
    ESP_LOGW(TAG, "  ещё не сгенерирован (docs/26, §7)");

    return ESP_OK;
}
