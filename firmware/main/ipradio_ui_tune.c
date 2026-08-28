/*
 * ipradio_ui_tune.c — настройка эфира, экран 06.
 *
 * Замысел — в ipradio_ui_tune.h.
 */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "ipradio_fonts.h"
#include "ipradio_tuner.h"
#include "ipradio_ui_theme.h"
#include "ipradio_ui_tune.h"

static const char *TAG = "ui.tune";

/* Геометрия шкалы. Отсчитывается от краёв экрана, а не от центра:
 * шкала должна быть симметричной при любой ширине. */
#define SCALE_SIDE_PAD   80
#define SCALE_Y         330
#define SCALE_H          10
#define TICK_H           18
#define TICK_MAJOR_H     30
#define MARK_H           44
#define CURSOR_W          4
#define CURSOR_H         84

static lv_obj_t *s_screen;
static lv_obj_t *s_scale;        /* полоса шкалы            */
static lv_obj_t *s_cursor;       /* стрелка текущей частоты */
static lv_obj_t *s_freq;         /* частота крупно          */
static lv_obj_t *s_band_label;
static lv_obj_t *s_rds;
static lv_obj_t *s_signal;
static lv_obj_t *s_status;       /* «идёт автопоиск» и подобное */
static lv_obj_t *s_edge_lo;
static lv_obj_t *s_edge_hi;

static lv_obj_t *s_mark[IPRADIO_TUNE_MAX_MARKS];
static uint32_t  s_mark_khz[IPRADIO_TUNE_MAX_MARKS];
static int       s_mark_count;

static bool           s_visible;
static ipradio_band_t s_band = IPRADIO_BAND_CCIR;
static void         (*s_on_close)(void *ctx);
static void          *s_ctx;

/* ------------------------------------------------------------------ *
 *  Пересчёт частоты в положение на шкале
 * ------------------------------------------------------------------ */

static uint32_t band_lo(ipradio_band_t b)
{
    return (b == IPRADIO_BAND_OIRT) ? TUNER_OIRT_MIN_KHZ : TUNER_CCIR_MIN_KHZ;
}

static uint32_t band_hi(ipradio_band_t b)
{
    return (b == IPRADIO_BAND_OIRT) ? TUNER_OIRT_MAX_KHZ : TUNER_CCIR_MAX_KHZ;
}

static lv_coord_t scale_width(void)
{
    return (lv_coord_t) (lv_obj_get_width(s_screen) - 2 * SCALE_SIDE_PAD);
}

/* Частота → смещение в точках от левого края шкалы.
 *
 * Считаем в 64 разрядах: ширина шкалы около 1120 точек, разность
 * частот до 20 500 кГц, их произведение вылезает за 32 разряда
 * и молча превратилось бы в мусор ровно в середине диапазона. */
static lv_coord_t freq_to_x(uint32_t khz, ipradio_band_t b)
{
    uint32_t lo = band_lo(b);
    uint32_t hi = band_hi(b);

    if (khz <= lo) return 0;
    if (khz >= hi) return scale_width();

    return (lv_coord_t) (((uint64_t) (khz - lo) * (uint64_t) scale_width())
                         / (uint64_t) (hi - lo));
}

/* ------------------------------------------------------------------ *
 *  Засечки
 * ------------------------------------------------------------------ */

static void place_mark(int i)
{
    lv_coord_t x = freq_to_x(s_mark_khz[i], s_band);
    lv_obj_align(s_mark[i], LV_ALIGN_TOP_LEFT,
                 SCALE_SIDE_PAD + x - 1, SCALE_Y - MARK_H);
}

void ipradio_tune_add_mark(uint32_t freq_khz)
{
    if (!s_screen || s_mark_count >= IPRADIO_TUNE_MAX_MARKS) {
        return;
    }

    /* Один и тот же канал автопоиск может отдать дважды, если пройти
     * диапазон в обе стороны. Дублировать засечку незачем. */
    for (int i = 0; i < s_mark_count; i++) {
        if (s_mark_khz[i] == freq_khz) {
            return;
        }
    }

    int i = s_mark_count++;
    s_mark_khz[i] = freq_khz;
    lv_obj_clear_flag(s_mark[i], LV_OBJ_FLAG_HIDDEN);
    place_mark(i);
}

void ipradio_tune_clear_marks(void)
{
    for (int i = 0; i < IPRADIO_TUNE_MAX_MARKS; i++) {
        if (s_mark[i]) {
            lv_obj_add_flag(s_mark[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    s_mark_count = 0;
}

/* ------------------------------------------------------------------ *
 *  Сборка
 * ------------------------------------------------------------------ */

/* Засечки шкалы: каждый мегагерц — короткая, каждый пятый — длинная
 * с подписью. Чаще рисовать нельзя: на 87,5–108 МГц штрих на каждые
 * 100 кГц превратился бы в сплошную серую полосу. */
static void build_ticks(lv_obj_t *parent, ipradio_band_t b)
{
    uint32_t lo = band_lo(b);
    uint32_t hi = band_hi(b);

    for (uint32_t mhz = (lo + 999) / 1000; mhz * 1000 <= hi; mhz++) {
        bool major = (mhz % 5 == 0);

        lv_obj_t *t = lv_obj_create(parent);
        lv_obj_set_size(t, major ? 2 : 1, major ? TICK_MAJOR_H : TICK_H);
        lv_obj_set_style_bg_color(t, major ? COL_TEXT_DIM : COL_TEXT_FAINT, 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(t, 0, 0);
        lv_obj_set_style_radius(t, 0, 0);
        lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(t, LV_ALIGN_TOP_LEFT,
                     SCALE_SIDE_PAD + freq_to_x(mhz * 1000, b),
                     SCALE_Y + SCALE_H + 4);

        if (major) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%u", (unsigned) mhz);
            lv_obj_t *l = ipradio_ui_label(parent, ipradio_font_14,
                                           COL_TEXT_FAINT, buf);
            lv_obj_align(l, LV_ALIGN_TOP_LEFT,
                         SCALE_SIDE_PAD + freq_to_x(mhz * 1000, b) - 10,
                         SCALE_Y + SCALE_H + TICK_MAJOR_H + 8);
        }
    }
}

esp_err_t ipradio_tune_init(lv_obj_t *parent)
{
    s_screen = lv_obj_create(parent);
    lv_obj_set_size(s_screen, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_screen, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_radius(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *head = ipradio_ui_label(s_screen, ipradio_font_22,
                                      COL_TEXT_DIM, "Настройка эфира");
    lv_obj_align(head, LV_ALIGN_TOP_LEFT, SCALE_SIDE_PAD, 32);

    s_band_label = ipradio_ui_label(s_screen, ipradio_font_22, COL_AMBER, "FM");
    lv_obj_align(s_band_label, LV_ALIGN_TOP_RIGHT, -SCALE_SIDE_PAD, 32);

    /* Частота крупно — над шкалой, ровно по её центру: взгляд идёт
     * сверху вниз, от числа к его месту на шкале. */
    s_freq = ipradio_ui_label(s_screen, ipradio_font_48b, COL_TEXT, "--.--");
    lv_obj_align(s_freq, LV_ALIGN_TOP_MID, 0, 120);

    s_rds = ipradio_ui_label(s_screen, ipradio_font_22, COL_AMBER, "");
    lv_obj_align(s_rds, LV_ALIGN_TOP_MID, 0, 200);

    s_signal = ipradio_ui_label(s_screen, ipradio_font_14, COL_TEXT_FAINT, "");
    lv_obj_align(s_signal, LV_ALIGN_TOP_MID, 0, 240);

    /* Сама шкала. */
    s_scale = lv_obj_create(s_screen);
    lv_obj_set_size(s_scale, LV_PCT(100), SCALE_H);
    lv_obj_set_style_bg_color(s_scale, COL_SURFACE, 0);
    lv_obj_set_style_bg_opa(s_scale, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_scale, COL_BORDER, 0);
    lv_obj_set_style_border_width(s_scale, 1, 0);
    lv_obj_set_style_radius(s_scale, SCALE_H / 2, 0);
    lv_obj_set_style_pad_all(s_scale, 0, 0);
    lv_obj_clear_flag(s_scale, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(s_scale, lv_pct(100));
    lv_obj_align(s_scale, LV_ALIGN_TOP_MID, 0, SCALE_Y);
    lv_obj_set_style_margin_hor(s_scale, SCALE_SIDE_PAD, 0);

    /* Засечки станций. Создаются заранее все и прячутся: выделять
     * память по ходу автопоиска значило бы дёргать кучу в тот момент,
     * когда чип уже сканирует и любая задержка слышна. */
    for (int i = 0; i < IPRADIO_TUNE_MAX_MARKS; i++) {
        s_mark[i] = lv_obj_create(s_screen);
        lv_obj_set_size(s_mark[i], 3, MARK_H);
        lv_obj_set_style_bg_color(s_mark[i], COL_AMBER, 0);
        lv_obj_set_style_bg_opa(s_mark[i], LV_OPA_70, 0);
        lv_obj_set_style_border_width(s_mark[i], 0, 0);
        lv_obj_set_style_radius(s_mark[i], 1, 0);
        lv_obj_clear_flag(s_mark[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_mark[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* Указатель текущей частоты. Поверх засечек — он главнее. */
    s_cursor = lv_obj_create(s_screen);
    lv_obj_set_size(s_cursor, CURSOR_W, CURSOR_H);
    lv_obj_set_style_bg_color(s_cursor, COL_TEXT, 0);
    lv_obj_set_style_bg_opa(s_cursor, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_cursor, 0, 0);
    lv_obj_set_style_radius(s_cursor, 2, 0);
    lv_obj_clear_flag(s_cursor, LV_OBJ_FLAG_SCROLLABLE);

    s_edge_lo = ipradio_ui_label(s_screen, ipradio_font_14, COL_TEXT_FAINT, "");
    s_edge_hi = ipradio_ui_label(s_screen, ipradio_font_14, COL_TEXT_FAINT, "");

    s_status = ipradio_ui_label(s_screen, ipradio_font_16, COL_TEXT_DIM, "");
    lv_obj_align(s_status, LV_ALIGN_BOTTOM_MID, 0, -76);

    lv_obj_t *hints = ipradio_ui_label(s_screen, ipradio_font_14,
        COL_TEXT_FAINT,
        "Энкодер 1 — перестройка   •   нажатие — автопоиск   •   "
        "нажатие энкодера 2 — назад");
    lv_obj_align(hints, LV_ALIGN_BOTTOM_MID, 0, -28);

    build_ticks(s_screen, IPRADIO_BAND_CCIR);

    lv_obj_add_flag(s_screen, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "экран настройки эфира готов");
    return ESP_OK;
}

/* ------------------------------------------------------------------ *
 *  Отрисовка по снимку
 * ------------------------------------------------------------------ */

void ipradio_tune_update(const ipradio_snapshot_t *s)
{
    if (!s_screen || !s_visible) {
        return;
    }

    char buf[64];

    /* Смена диапазона перестраивает всю шкалу: границы, засечки,
     * подписи. Засечки станций при этом теряют смысл — они относились
     * к прежнему диапазону. */
    if (s->band != s_band) {
        s_band = s->band;
        ipradio_tune_clear_marks();
        /* Штрихи прежнего диапазона остаются на экране до следующей
         * пересборки: перерисовывать их здесь значило бы удалять
         * и создавать полсотни объектов в задаче отрисовки. Смена
         * диапазона делается из меню, там и пересоберём. */
    }

    snprintf(buf, sizeof(buf), "%u.%02u",
             (unsigned) (s->freq_khz / 1000),
             (unsigned) ((s->freq_khz % 1000) / 10));
    lv_label_set_text(s_freq, buf);

    lv_label_set_text(s_band_label,
                      (s->band == IPRADIO_BAND_OIRT) ? "УКВ" : "FM");

    lv_label_set_text(s_rds, (s->rds_valid && s->rds_name[0])
                             ? s->rds_name : "");

    snprintf(buf, sizeof(buf), "сигнал %u %%", (unsigned) s->signal_level);
    lv_label_set_text(s_signal, buf);

    /* Границы диапазона подписаны у краёв шкалы: без них непонятно,
     * что за отрезок вообще нарисован. */
    snprintf(buf, sizeof(buf), "%u.%u",
             (unsigned) (band_lo(s_band) / 1000),
             (unsigned) ((band_lo(s_band) % 1000) / 100));
    lv_label_set_text(s_edge_lo, buf);
    lv_obj_align(s_edge_lo, LV_ALIGN_TOP_LEFT,
                 SCALE_SIDE_PAD - 12, SCALE_Y - 30);

    snprintf(buf, sizeof(buf), "%u",
             (unsigned) (band_hi(s_band) / 1000));
    lv_label_set_text(s_edge_hi, buf);
    lv_obj_align(s_edge_hi, LV_ALIGN_TOP_RIGHT,
                 -SCALE_SIDE_PAD + 12, SCALE_Y - 30);

    lv_obj_align(s_cursor, LV_ALIGN_TOP_LEFT,
                 SCALE_SIDE_PAD + freq_to_x(s->freq_khz, s_band) - CURSOR_W / 2,
                 SCALE_Y - (CURSOR_H - SCALE_H) / 2);

    for (int i = 0; i < s_mark_count; i++) {
        place_mark(i);
    }
}

/* ------------------------------------------------------------------ *
 *  Открытие и закрытие
 * ------------------------------------------------------------------ */

void ipradio_tune_open(void (*on_close)(void *ctx), void *ctx)
{
    if (!s_screen) {
        return;
    }

    s_on_close = on_close;
    s_ctx      = ctx;
    s_visible  = true;

    lv_label_set_text(s_status, "");

    ipradio_snapshot_t snap;
    ipradio_get(&snap);
    ipradio_tune_update(&snap);

    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_screen);
}

void ipradio_tune_close(void)
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

bool ipradio_tune_visible(void)
{
    return s_visible;
}

/* ------------------------------------------------------------------ *
 *  Органы управления
 * ------------------------------------------------------------------ */

void ipradio_tune_move(int delta)
{
    if (!s_visible || delta == 0) {
        return;
    }

    /* Перестройка идёт через автомат, а не прямым вызовом тюнера:
     * иначе состояние в автомате разъедется с тем, что в чипе,
     * и главный экран показал бы старую частоту. */
    ipradio_post_simple(IPRADIO_EV_FREQ_DELTA, delta);
}

void ipradio_tune_select(void)
{
    if (!s_visible) {
        return;
    }

    /* Автопоиск. Засечки от прежнего прохода убираем: показывать
     * вперемешку старые и новые нельзя — непонятно, что из этого
     * найдено сейчас. */
    ipradio_tune_clear_marks();
    lv_label_set_text(s_status, "Идёт автопоиск по диапазону…");

    /* Сам проход по диапазону — дело модуля тюнера: он умеет
     * аппаратный SEEK и знает, когда чип дошёл до края. Экран лишь
     * отмечает найденное, получая ipradio_tune_add_mark(). */
    ESP_LOGI(TAG, "запрошен автопоиск");
}

void ipradio_tune_back(void)
{
    ipradio_tune_close();
}
