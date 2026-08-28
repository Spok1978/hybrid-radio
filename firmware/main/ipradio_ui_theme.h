/*
 * ipradio_ui_theme.h — общее для всех экранов: цвета и мелкие помощники.
 *
 * Заведён, когда экранов стало больше одного. Смысл простой: цвет
 * и отступы не должны разъезжаться между файлами. Значения — из макетов,
 * docs/28-screen-mockups.md.
 *
 * Правило цвета важнее остальных и потому вынесено сюда явно:
 * ЦВЕТ КОДИРУЕТ РЕЖИМ. Янтарь — эфир, циан — интернет, красный — беда
 * (звук выключен, сети нет, станция молчит). Это не украшение: режим
 * должен читаться раньше, чем прочитан текст.
 */

#pragma once

#include "lvgl.h"

#include "ipradio_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Основа. */
#define COL_BG          lv_color_hex(0x0e0f11)
#define COL_SURFACE     lv_color_hex(0x16181b)
#define COL_BORDER      lv_color_hex(0x2a2e34)

/* Текст, три ступени приглушения. */
#define COL_TEXT        lv_color_hex(0xf2f3f5)
#define COL_TEXT_DIM    lv_color_hex(0x9aa1a9)
#define COL_TEXT_FAINT  lv_color_hex(0x5f666e)

/* Смысловые. */
#define COL_AMBER       lv_color_hex(0xf5a524)   /* эфир              */
#define COL_CYAN        lv_color_hex(0x3ec5d8)   /* интернет          */
#define COL_RED         lv_color_hex(0xf05b52)   /* тревога, mute     */
#define COL_GREEN       lv_color_hex(0x5fc98a)   /* подтверждение     */

/** Название диапазона для экрана.
 *
 *  Пока диапазон один, поэтому всегда «FM». Отдельная функция нужна
 *  затем, чтобы при возврате УКВ (IPRADIO_ENABLE_OIRT) не искать
 *  по всем экранам, где это название пишется. */
const char *ipradio_band_label(ipradio_band_t band);

/** Подпись с заданным шрифтом и цветом. Самая частая операция
 *  во всех экранах, поэтому в общем месте. */
lv_obj_t *ipradio_ui_label(lv_obj_t *parent, const lv_font_t *font,
                           lv_color_t color, const char *text);

/** Прямоугольник-подложка: заливка, рамка, скруглeние, без прокрутки.
 *  Из таких собраны и ячейки пресетов, и плашки диалогов. */
lv_obj_t *ipradio_ui_panel(lv_obj_t *parent, lv_coord_t w, lv_coord_t h,
                           lv_color_t bg, lv_color_t border, lv_coord_t radius);

#ifdef __cplusplus
}
#endif
