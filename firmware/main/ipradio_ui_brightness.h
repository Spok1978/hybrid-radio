/*
 * ipradio_ui_brightness.h — яркость экрана, §5.3.3.3.
 *
 * Самая простая настройка из всех, и единственная, где важен способ
 * показа: яркость надо крутить, ГЛЯДЯ на результат. Поэтому значение
 * применяется сразу при вращении, а не при выходе. Настройка яркости
 * вслепую, с применением по подтверждению, — это подбор наугад.
 *
 * Обратная сторона: если человек уйдёт кнопкой «назад», экран должен
 * вернуться к прежней яркости, а не остаться на подобранной. Прежнее
 * значение поэтому запоминается при входе.
 *
 * Нижний предел не ноль. Ноль гасит подсветку полностью, и вернуть
 * яркость станет нечем — экран, на котором её крутят, сам погаснет.
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Ниже этого не опускаем: см. пояснение выше. */
#define IPRADIO_BRIGHTNESS_MIN  10

/** Шаг на один щелчок регулятора. Пять процентов — двадцать щелчков
 *  на весь ход: достаточно точно и не утомительно. */
#define IPRADIO_BRIGHTNESS_STEP  5

esp_err_t ipradio_brightness_ui_init(lv_obj_t *parent);
void ipradio_brightness_ui_open(void (*on_close)(void *ctx), void *ctx);
void ipradio_brightness_ui_close(void);
bool ipradio_brightness_ui_visible(void);

void ipradio_brightness_ui_move(int delta);
void ipradio_brightness_ui_select(void);   /**< принять и выйти */
void ipradio_brightness_ui_back(void);     /**< вернуть прежнюю */

#ifdef __cplusplus
}
#endif
