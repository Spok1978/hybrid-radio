/*
 * ipradio_ui_netbadge.c — состояние сети поверх всех экранов.
 *
 * Замысел — в ipradio_ui_netbadge.h.
 */

#include "esp_log.h"

#include "ipradio_fonts.h"
#include "ipradio_ui_netbadge.h"
#include "ipradio_ui_theme.h"

static const char *TAG = "ui.net";

#define DOT     18
#define BADGE_H 56

static lv_obj_t *s_dot;
static lv_obj_t *s_text;

static ipradio_net_state_t s_shown = (ipradio_net_state_t) -1;

esp_err_t ipradio_netbadge_init(void)
{
    /* Верхний слой рисуется поверх любого экрана и не принадлежит
     * ни одному из них. Именно поэтому значок и живёт здесь. */
    lv_obj_t *top = lv_layer_top();

    lv_obj_t *box = lv_obj_create(top);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_SIZE_CONTENT, BADGE_H);

    /* Верх по центру: слева часы, справа громкость и режим - середина
     * единственное место, свободное на всех экранах сразу. */
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(box, 12, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    /* Верхний слой по умолчанию перехватывает касания. Нам это
     * не нужно: значок ничего не делает, а перехват сломал бы
     * нажатия на экранах под ним. */
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(top, LV_OBJ_FLAG_CLICKABLE);

    s_dot = lv_obj_create(box);
    lv_obj_remove_style_all(s_dot);
    lv_obj_set_size(s_dot, DOT, DOT);
    lv_obj_set_style_radius(s_dot, DOT / 2, 0);
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_dot, COL_TEXT_FAINT, 0);

    s_text = ipradio_ui_label(box, ipradio_font_28, COL_TEXT_FAINT, "Wi-Fi");

    ESP_LOGI(TAG, "значок сети поднят на верхнем слое");
    return ESP_OK;
}

void ipradio_netbadge_update(const ipradio_snapshot_t *s)
{
    if (!s_dot || !s) {
        return;
    }

    /* Перерисовываем только на смену состояния: значок обновляется
     * из общего пути отрисовки, а тот зовётся на каждое событие. */
    if (s->net == s_shown) {
        return;
    }
    s_shown = s->net;

    const char *word;
    lv_color_t  color;

    switch (s->net) {
    case IPRADIO_NET_CONNECTED:
        word = "Wi-Fi";  color = COL_GREEN;      break;
    case IPRADIO_NET_CONNECTING:
        word = "связь…"; color = COL_AMBER;      break;
    case IPRADIO_NET_DISCONNECTED:
        word = "нет сети"; color = COL_RED;      break;
    default:
        /* «Не настроен» - это не поломка, а несделанная настройка.
         * Красным её метить нельзя: красный означает, что что-то
         * сломалось, и человек пойдёт искать неисправность. */
        word = "Wi-Fi не задан"; color = COL_TEXT_FAINT; break;
    }

    lv_obj_set_style_bg_color(s_dot, color, 0);
    lv_obj_set_style_text_color(s_text, color, 0);
    lv_label_set_text(s_text, word);
}
