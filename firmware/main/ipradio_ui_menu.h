/*
 * ipradio_ui_menu.h — меню настроек, экран 05.
 *
 * Состав взят из §5.3 спецификации, вёрстка — из макета 05:
 * крупные строки, текущее значение справа у каждой.
 *
 * Значение справа — не украшение, а главное свойство этого экрана.
 * Меню, где надо зайти в пункт, чтобы узнать, что там сейчас, заставляет
 * человека обходить его целиком. Здесь состояние прибора читается
 * с одного взгляда, не заходя никуда.
 *
 * Навигация задана в §5.3 и одинакова для всех экранов настройки:
 *
 *     регулятор 1, вращение   — перемещение по строкам
 *     регулятор 1, нажатие    — вход в пункт
 *     регулятор 2, нажатие    — выход на уровень вверх
 *
 * Кнопка регулятора 2 — это mute на экране воспроизведения, и здесь она
 * же служит «назад». Двойного смысла не возникает: экраны разные,
 * и mute в меню всё равно не нужен.
 *
 * Меню — модальный экран: пока оно открыто, органы принадлежат ему.
 * Перехват ставит вызывающий (ipradio_ui.c) тем же фильтром событий,
 * что и для диалогов.
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

#include "ipradio_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Пункты верхнего уровня. Порядок — порядок на экране. */
typedef enum {
    IPRADIO_MENU_TUNE_FM = 0,  /**< 5.3.1 настройка эфирных станций   */
    IPRADIO_MENU_FIND_NET,     /**< 5.3.2 поиск интернет-станций      */
    IPRADIO_MENU_WIFI,         /**< 5.3.3.1 Wi-Fi                     */
    IPRADIO_MENU_CLOCK,        /**< 5.3.3.2 часы                      */
    IPRADIO_MENU_BRIGHTNESS,   /**< 5.3.3.3 яркость                   */
    IPRADIO_MENU_STATIONS,     /**< 5.3.3.4 список станций            */
    IPRADIO_MENU_DIAGNOSTICS,  /**< 5.3.3.5 состояние прибора         */
    IPRADIO_MENU_ITEM_COUNT,
} ipradio_menu_item_t;

/** Что выбрал человек. Открывать подчинённый экран — дело вызывающего:
 *  меню знает состав, но не знает, чем каждый пункт обслуживается. */
typedef void (*ipradio_menu_cb_t)(ipradio_menu_item_t item, void *ctx);

/** Построить (один раз, при подъёме интерфейса). Не показывает. */
esp_err_t ipradio_menu_init(lv_obj_t *parent);

/** Открыть. cb зовётся при выборе пункта, on_close — при выходе. */
void ipradio_menu_open(ipradio_menu_cb_t cb, ipradio_menu_cb_t on_close,
                       void *ctx);

/** Закрыть. */
void ipradio_menu_close(void);

/** Открыто ли сейчас. */
bool ipradio_menu_visible(void);

/** Обновить значения справа. Зовётся по снимку состояния. */
void ipradio_menu_update(const ipradio_snapshot_t *snap);

/* Органы управления, §5.3. */
void ipradio_menu_move(int delta);
void ipradio_menu_select(void);
void ipradio_menu_back(void);

#ifdef __cplusplus
}
#endif
