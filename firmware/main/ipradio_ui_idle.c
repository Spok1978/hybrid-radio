/*
 * ipradio_ui_idle.c — ждущий режим, экран 08.
 *
 * Замысел — в ipradio_ui_idle.h.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"

#include "bsp/display.h"

#include "ipradio_fonts.h"
#include "ipradio_ui_idle.h"
#include "ipradio_ui_theme.h"

static const char *TAG = "ui.idle";

/* Яркость в ждущем режиме — §5.4. Возвращаемое значение берём
 * из настроек, а не из константы: человек мог убавить яркость сам,
 * и выход из ждущего режима не должен её самовольно поднимать. */
#define IDLE_BRIGHTNESS   50

static lv_obj_t *s_screen;
static lv_obj_t *s_clock;
static lv_obj_t *s_date;
static lv_obj_t *s_now_playing;
static lv_obj_t *s_hint;

static bool    s_active;
static uint8_t s_saved_brightness = 80;   /* столько же по умолчанию,
                                             сколько в ipradio_storage.c */

/* ------------------------------------------------------------------ *
 *  Сборка
 * ------------------------------------------------------------------ */

esp_err_t ipradio_idle_init(lv_obj_t *parent)
{
    s_screen = lv_obj_create(parent);
    lv_obj_set_size(s_screen, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_screen, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_radius(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    /* Часы. Крупнее в наборе кеглей ничего нет, и это правильный
     * предел: буквы выше примерно четверти высоты экрана начинают
     * выглядеть не «издалека видно», а «сломалось». */
    s_clock = ipradio_ui_label(s_screen, ipradio_font_48b, COL_TEXT, "--:--");
    lv_obj_align(s_clock, LV_ALIGN_CENTER, 0, -40);

    /* Дата приглушена сильно: она нужна взглядом, а не с порога. */
    s_date = ipradio_ui_label(s_screen, ipradio_font_20, COL_TEXT_FAINT, "");
    lv_obj_align_to(s_date, s_clock, LV_ALIGN_OUT_BOTTOM_MID, 0, 16);

    /* Что играет. Цветом кодируется режим — то же правило, что
     * на главном экране: янтарь эфир, циан интернет. */
    s_now_playing = ipradio_ui_label(s_screen, ipradio_font_22, COL_AMBER, "");
    lv_obj_set_width(s_now_playing, LV_PCT(80));
    lv_label_set_long_mode(s_now_playing, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_now_playing, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_now_playing, LV_ALIGN_BOTTOM_MID, 0, -74);

    s_hint = ipradio_ui_label(s_screen, ipradio_font_14, COL_TEXT_FAINT,
                              "Любое действие вернёт экран");
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -30);

    lv_obj_add_flag(s_screen, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "ждущий режим готов, порог %d мин",
             IPRADIO_IDLE_TIMEOUT_MS / 60000);
    return ESP_OK;
}

/* ------------------------------------------------------------------ *
 *  Включение и выключение
 * ------------------------------------------------------------------ */

void ipradio_idle_set_active(bool active)
{
    if (!s_screen || active == s_active) {
        return;
    }
    s_active = active;

    if (active) {
        /* Запоминаем текущую яркость, чтобы вернуть именно её.
         * Константа 80 здесь была бы ошибкой: человек мог убавить
         * яркость сам, и выход из ждущего режима не должен её
         * самовольно поднимать. */
        int cur = bsp_display_brightness_get();
        if (cur >= 0 && cur <= 100) {
            s_saved_brightness = (uint8_t) cur;
        }

        bsp_display_brightness_set(IDLE_BRIGHTNESS);
        lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_screen);
    } else {
        lv_obj_add_flag(s_screen, LV_OBJ_FLAG_HIDDEN);
        bsp_display_brightness_set(s_saved_brightness);
    }

    ESP_LOGI(TAG, "ждущий режим %s", active ? "включён" : "снят");
}

bool ipradio_idle_active(void)
{
    return s_active;
}

/* ------------------------------------------------------------------ *
 *  Содержимое
 * ------------------------------------------------------------------ */

/* День недели и месяц — словами и в родительном падеже: «27 августа».
 * Через strftime это не получить: локали в прошивке нет, а %B дал бы
 * именительный падеж даже там, где локаль есть. */
static const char *const MONTHS[12] = {
    "января", "февраля", "марта",    "апреля",  "мая",    "июня",
    "июля",   "августа", "сентября", "октября", "ноября", "декабря",
};

static const char *const WEEKDAYS[7] = {
    "воскресенье", "понедельник", "вторник", "среда",
    "четверг",     "пятница",     "суббота",
};

void ipradio_idle_update(const ipradio_snapshot_t *s)
{
    if (!s_screen || !s_active) {
        return;   /* невидимое не рисуем */
    }

    char buf[128];

    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);

    strftime(buf, sizeof(buf), "%H:%M", &tmv);
    lv_label_set_text(s_clock, buf);

    if (tmv.tm_mon >= 0 && tmv.tm_mon < 12 &&
        tmv.tm_wday >= 0 && tmv.tm_wday < 7) {
        snprintf(buf, sizeof(buf), "%s, %d %s",
                 WEEKDAYS[tmv.tm_wday], tmv.tm_mday, MONTHS[tmv.tm_mon]);
        lv_label_set_text(s_date, buf);
    }

    /* Что играет. Пишем ровно то же, что было бы на главном экране,
     * чтобы не приходилось выходить из ждущего режима ради проверки. */
    bool fm = (s->mode == IPRADIO_MODE_FM);
    lv_obj_set_style_text_color(s_now_playing, fm ? COL_AMBER : COL_CYAN, 0);

    if (s->muted) {
        lv_obj_set_style_text_color(s_now_playing, COL_RED, 0);
        lv_label_set_text(s_now_playing, "Звук выключен");
    } else if (fm) {
        if (s->rds_valid && s->rds_name[0]) {
            snprintf(buf, sizeof(buf), "%s   •   %u.%02u МГц",
                     s->rds_name,
                     (unsigned) (s->freq_khz / 1000),
                     (unsigned) ((s->freq_khz % 1000) / 10));
        } else {
            snprintf(buf, sizeof(buf), "%u.%02u МГц   •   %s",
                     (unsigned) (s->freq_khz / 1000),
                     (unsigned) ((s->freq_khz % 1000) / 10),
                     ipradio_band_label(s->band));
        }
        lv_label_set_text(s_now_playing, buf);
    } else {
        lv_label_set_text(s_now_playing,
                          s->station_name[0] ? s->station_name : "—");
    }
}
