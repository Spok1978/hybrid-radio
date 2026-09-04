/*
 * ipradio_ui_dialog.h — плашка поверх играющего экрана.
 *
 * Обслуживает экраны 10, 11 и 12 из docs/28-screen-mockups.md — три
 * случая, когда интернет-радио недоступно. Один модуль на три экрана
 * потому, что форма у них общая, а различаются текст и набор кнопок.
 *
 * Ключевое требование, ради которого это вообще отдельная сущность
 * (docs/26-firmware-spec.md, §5.2, правило 3): ПРИБОР НЕ ЗАМОЛКАЕТ.
 * Диалог показывается ПОВЕРХ играющего эфира и прямо об этом пишет.
 * Полноэкранная ошибка на весь дисплей заставила бы человека решить,
 * что сломалось всё, а не только интернет.
 *
 * Второе требование оттуда же: у трёх случаев РАЗНОЕ поведение.
 * «Не настроен» ведёт в настройку, «связи нет» — не ведёт никуда,
 * потому что связь скорее всего вернётся сама, а «станция молчит»
 * сразу предлагает другие станции. Поэтому это не один диалог
 * с меняющимся текстом, а три с общей формой.
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    IPRADIO_DIALOG_NONE = 0,
    IPRADIO_DIALOG_WIFI_NOT_CONFIGURED,  /**< экран 10 */
    IPRADIO_DIALOG_WIFI_NO_LINK,         /**< экран 11 */
    IPRADIO_DIALOG_STATION_DEAD,         /**< экран 12 */
} ipradio_dialog_kind_t;

/** Что выбрал человек. Разбирает вызывающий: диалог сам никуда
 *  не переключает — он только сообщает решение. */
typedef enum {
    IPRADIO_DIALOG_ACT_DISMISS = 0,  /**< «Не сейчас», «Понятно»       */
    IPRADIO_DIALOG_ACT_SETUP_WIFI,   /**< «Настроить Wi-Fi»            */
    IPRADIO_DIALOG_ACT_PICK_STATION, /**< «Выбрать другую станцию»     */
    IPRADIO_DIALOG_ACT_BACK_TO_FM,   /**< «Вернуться в эфир»           */
} ipradio_dialog_action_t;

typedef void (*ipradio_dialog_cb_t)(ipradio_dialog_action_t action, void *ctx);

/** Построить (один раз, при подъёме интерфейса). Ничего не показывает. */
esp_err_t ipradio_dialog_init(lv_obj_t *parent);

/** Показать нужный вид.
 *  @param detail  подстановка в текст: имя сети для случая 11,
 *                 имя станции для случая 12. Может быть NULL. */
void ipradio_dialog_show(ipradio_dialog_kind_t kind, const char *detail,
                         ipradio_dialog_cb_t cb, void *ctx);

/** Убрать. Зовётся и по выбору человека, и само собой — когда связь
 *  вернулась (§5.2, правило 5: ничего делать для этого не надо). */
void ipradio_dialog_hide(void);

/** Что показано прямо сейчас. */
ipradio_dialog_kind_t ipradio_dialog_current(void);

/* Управление с органов: диалог обязан быть проходим без касания
 * (требование R4.1). Регулятор 1 переставляет выделение, его кнопка
 * подтверждает. */
void ipradio_dialog_move(int delta);
void ipradio_dialog_select(void);

#ifdef __cplusplus
}
#endif
