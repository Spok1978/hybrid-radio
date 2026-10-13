/*
 * ipradio_fmprobe.h — проверка приёма эфира на столе: проход по
 * диапазону и уровень сигнала каждой найденной станции в журнал.
 * Только для отладки, ключ IPRADIO_DEBUG_FM_PROBE в board_pins.h.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Запустить проверку в своей задаче. */
void ipradio_fmprobe_start(void);

#ifdef __cplusplus
}
#endif
