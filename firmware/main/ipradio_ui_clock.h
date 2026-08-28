/*
 * ipradio_ui_clock.h — часы, §5.3.3.2.
 *
 * По спецификации: по умолчанию NTP; ручной режим — время, часовой
 * пояс, дата; формат 24 часа по умолчанию, переключаемый на AM/PM.
 *
 * Ручная установка сделана полностью, хотя нужна редко. Оставить её
 * недоделанной было нельзя: переключатель «источник времени» без
 * работающего ручного режима — тупик, который прибор сам же
 * и предлагает.
 *
 * Часовой пояс выбирается из списка, а не набирается. Строка POSIX
 * TZ вроде «MSK-3» — знание, которого от человека требовать нельзя,
 * и знак смещения в ней вдобавок обратный привычному.
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ipradio_clock_ui_init(lv_obj_t *parent);
void ipradio_clock_ui_open(void (*on_close)(void *ctx), void *ctx);
void ipradio_clock_ui_close(void);
bool ipradio_clock_ui_visible(void);

/** Перерисовать: в списке показано текущее время, оно идёт. */
void ipradio_clock_ui_poll(void);

void ipradio_clock_ui_move(int delta);
void ipradio_clock_ui_select(void);
void ipradio_clock_ui_back(void);

#ifdef __cplusplus
}
#endif
