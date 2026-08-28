/*
 * ipradio_ui_theme.c — реализация общих помощников. Пояснения
 * в ipradio_ui_theme.h.
 */

#include "ipradio_ui_theme.h"

const char *ipradio_band_label(ipradio_band_t band)
{
#if IPRADIO_ENABLE_OIRT
    return (band == IPRADIO_BAND_OIRT) ? "УКВ" : "FM";
#else
    (void) band;
    return "FM";
#endif
}

lv_obj_t *ipradio_ui_label(lv_obj_t *parent, const lv_font_t *font,
                           lv_color_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text ? text : "");
    return l;
}

lv_obj_t *ipradio_ui_panel(lv_obj_t *parent, lv_coord_t w, lv_coord_t h,
                           lv_color_t bg, lv_color_t border, lv_coord_t radius)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_size(p, w, h);
    lv_obj_set_style_bg_color(p, bg, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(p, border, 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_radius(p, radius, 0);

    /* Прокрутка на подложках только мешает: содержимое рассчитано
     * так, чтобы помещаться, а случайный свайп по касанию не должен
     * уводить вёрстку. */
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}
