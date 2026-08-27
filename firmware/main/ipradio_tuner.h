/*
 * ipradio_tuner.h — эфирный тюнер RDA5807M.
 *
 * Два диапазона (docs/26-firmware-spec.md, §5.1):
 *   УКВ  65,8–74 МГц   — регистр BAND=3 плюс бит 65M_50M MODE
 *   FM   87,5–108 МГц  — регистр BAND=0
 *
 * На экране подписываются коротко, «УКВ» и «FM», без OIRT и CCIR.
 *
 * Автопоиск аппаратный: SEEK вверх и вниз с порогом по уровню,
 * то есть одна команда в чип, а не перебор частот в коде.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "driver/i2c_master.h"

#include "ipradio_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Границы диапазонов в килогерцах. */
#define TUNER_OIRT_MIN_KHZ   65800
#define TUNER_OIRT_MAX_KHZ   74000
#define TUNER_CCIR_MIN_KHZ   87500
#define TUNER_CCIR_MAX_KHZ  108000

/** Шаг сетки. Для России 50 кГц у УКВ и 100 кГц у FM. */
#define TUNER_STEP_OIRT_KHZ    50
#define TUNER_STEP_CCIR_KHZ   100

/** Поднять тюнер на уже готовой шине I²C.
 *  Шину создаёт вызывающий: она общая с тачем и кодеками платы,
 *  и второй раз её открывать нельзя. */
esp_err_t ipradio_tuner_init(i2c_master_bus_handle_t bus);

/** Есть ли чип на шине. */
bool ipradio_tuner_present(void);

/** Переключить диапазон. Частота подтягивается в его границы. */
esp_err_t ipradio_tuner_set_band(ipradio_band_t band);

/** Настроиться на частоту в килогерцах. Значение округляется
 *  до ближайшего узла сетки текущего диапазона. */
esp_err_t ipradio_tuner_set_freq(uint32_t khz);

/** Сдвинуть на N шагов сетки, знак задаёт направление. */
esp_err_t ipradio_tuner_step(int steps);

/** Запустить аппаратный автопоиск. up = вверх по частоте.
 *  Возврат немедленный: результат придёт событием, когда чип
 *  выставит флаг завершения. */
esp_err_t ipradio_tuner_seek(bool up);

/** Громкость 0…100 в логической шкале прибора.
 *  Внутри переводится в четырёхбитное поле VOLUME, у которого
 *  всего 16 ступеней и логарифмическая шкала (docs/23-volume-control.md). */
esp_err_t ipradio_tuner_set_volume(uint8_t logical);

/** Приглушить. Это ОТДЕЛЬНЫЙ бит DMUTE, а не громкость в ноль:
 *  в схеме с пассивной суммой неактивный источник должен быть
 *  заглушён по-настоящему. */
esp_err_t ipradio_tuner_set_mute(bool mute);

/** Полностью отключить выход чипа от сумматора (бит DHIZ).
 *  Используется при уходе в интернет-режим. */
esp_err_t ipradio_tuner_set_hiz(bool hiz);

/** Уровень принимаемого сигнала, 0…100. */
uint8_t ipradio_tuner_signal(void);

#ifdef __cplusplus
}
#endif
