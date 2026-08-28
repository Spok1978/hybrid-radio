/*
 * ipradio_ui_diag.c — состояние прибора.
 *
 * Замысел — в ipradio_ui_diag.h.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "ipradio_fonts.h"
#include "ipradio_net.h"
#include "ipradio_storage.h"
#include "ipradio_tuner.h"
#include "ipradio_ui_diag.h"
#include "ipradio_ui_theme.h"

static const char *TAG = "ui.diag";

#define SIDE_PAD  48
#define ROW_H     56

/* Порядок строк — порядок, в котором их спрашивают, когда прибор
 * ведёт себя не так. Сеть первой: с ней связано больше всего
 * непонятного поведения. */
enum {
    ROW_NET = 0,
    ROW_IP,
    ROW_CARD,
    ROW_TUNER,
    ROW_MEM,
    ROW_PSRAM,
    ROW_CHIP,
    ROW_FIRMWARE,
    ROW_UPTIME,
    ROW_COUNT,
};

static const char *const LABELS[ROW_COUNT] = {
    [ROW_NET]      = "Сеть",
    [ROW_IP]       = "Адрес",
    [ROW_CARD]     = "Карта памяти",
    [ROW_TUNER]    = "Тюнер",
    [ROW_MEM]      = "Свободная память",
    [ROW_PSRAM]    = "PSRAM",
    [ROW_CHIP]     = "Кристалл",
    [ROW_FIRMWARE] = "Прошивка",
    [ROW_UPTIME]   = "Время работы",
};

static lv_obj_t *s_screen;
static lv_obj_t *s_value[ROW_COUNT];
static bool      s_visible;

static void (*s_on_close)(void *ctx);
static void  *s_ctx;

/* ------------------------------------------------------------------ *
 *  Сборка
 * ------------------------------------------------------------------ */

esp_err_t ipradio_diag_ui_init(lv_obj_t *parent)
{
    s_screen = lv_obj_create(parent);
    lv_obj_set_size(s_screen, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_screen, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_radius(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *head = ipradio_ui_label(s_screen, ipradio_font_28,
                                      COL_TEXT, "Состояние прибора");
    lv_obj_align(head, LV_ALIGN_TOP_LEFT, SIDE_PAD, 30);

    lv_obj_t *list = lv_obj_create(s_screen);
    lv_obj_set_size(list, LV_PCT(100), 720 - 150 - 60);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 100);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_hor(list, SIDE_PAD, 0);
    lv_obj_set_style_pad_ver(list, 0, 0);
    lv_obj_set_style_pad_row(list, 2, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    for (int i = 0; i < ROW_COUNT; i++) {
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_set_size(row, LV_PCT(100), ROW_H);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(row, COL_BORDER, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *name = ipradio_ui_label(row, ipradio_font_16,
                                          COL_TEXT_FAINT, LABELS[i]);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);

        /* Значение справа и моноширинным не делаем: цифры тут читают
         * глазами, а не сравнивают по столбцам. */
        s_value[i] = ipradio_ui_label(row, ipradio_font_20, COL_TEXT, "—");
        lv_obj_set_width(s_value[i], LV_PCT(70));
        lv_label_set_long_mode(s_value[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(s_value[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(s_value[i], LV_ALIGN_RIGHT_MID, 0, 0);
    }

    lv_obj_t *hints = ipradio_ui_label(s_screen, ipradio_font_14,
        COL_TEXT_FAINT, "Нажатие энкодера 2 — назад");
    lv_obj_align(hints, LV_ALIGN_BOTTOM_MID, 0, -28);

    lv_obj_add_flag(s_screen, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "экран состояния готов");
    return ESP_OK;
}

/* ------------------------------------------------------------------ *
 *  Содержимое
 * ------------------------------------------------------------------ */

static void set_row(int row, lv_color_t color, const char *fmt, ...)
{
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    lv_label_set_text(s_value[row], buf);
    lv_obj_set_style_text_color(s_value[row], color, 0);
}

void ipradio_diag_ui_update(const ipradio_snapshot_t *s)
{
    if (!s_screen || !s_visible || !s) {
        return;
    }

    /* --- сеть --- */
    switch (s->net) {
    case IPRADIO_NET_CONNECTED:
        set_row(ROW_NET, COL_GREEN, "%s", ipradio_net_ssid());
        break;
    case IPRADIO_NET_CONNECTING:
        set_row(ROW_NET, COL_AMBER, "подключение…");
        break;
    case IPRADIO_NET_DISCONNECTED:
        set_row(ROW_NET, COL_RED, "нет связи");
        break;
    default:
        set_row(ROW_NET, COL_TEXT_FAINT, "не настроена");
        break;
    }

    /* --- адрес и уровень --- */
    if (s->net == IPRADIO_NET_CONNECTED) {
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip = { 0 };

        wifi_ap_record_t ap;
        int rssi = 0;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            rssi = ap.rssi;
        }

        if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK &&
            ip.ip.addr != 0) {
            /* Адрес и уровень в одной строке: порознь они занимают
             * две строки экрана, а спрашивают их всегда вместе. */
            set_row(ROW_IP, COL_TEXT, IPSTR "   •   %d дБм",
                    IP2STR(&ip.ip), rssi);
        } else {
            set_row(ROW_IP, COL_AMBER, "адрес не получен");
        }
    } else {
        set_row(ROW_IP, COL_TEXT_FAINT, "—");
    }

    /* --- карта --- */
    if (ipradio_storage_ready()) {
        set_row(ROW_CARD, COL_GREEN, "читается");
    } else {
        /* Не ошибка, но важное следствие: без карты настройки
         * не переживут выключения. Так и пишем. */
        set_row(ROW_CARD, COL_AMBER, "нет — настройки не сохраняются");
    }

    /* --- тюнер --- */
    if (ipradio_tuner_present()) {
        set_row(ROW_TUNER, COL_GREEN, "отвечает   •   сигнал %u %%",
                (unsigned) s->signal_level);
    } else {
        set_row(ROW_TUNER, COL_RED, "не отвечает — эфира не будет");
    }

    /* --- память --- */
    size_t heap = esp_get_free_heap_size();
    size_t low  = esp_get_minimum_free_heap_size();

    /* Показываем и текущее, и минимум за всё время работы. Текущее
     * само по себе мало что говорит: важно, насколько близко прибор
     * подходил к нулю. */
    set_row(ROW_MEM, (low < 20 * 1024) ? COL_RED : COL_TEXT,
            "%u КБ   •   минимум %u КБ",
            (unsigned) (heap / 1024), (unsigned) (low / 1024));

#if CONFIG_SPIRAM
    if (esp_psram_is_initialized()) {
        size_t total = esp_psram_get_size();
        size_t free_ = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        set_row(ROW_PSRAM, COL_TEXT, "%u МБ, свободно %u МБ",
                (unsigned) (total / (1024 * 1024)),
                (unsigned) (free_ / (1024 * 1024)));
    } else {
        set_row(ROW_PSRAM, COL_RED, "не поднялась");
    }
#else
    set_row(ROW_PSRAM, COL_TEXT_FAINT, "выключена в сборке");
#endif

    /* --- кристалл --- */
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    /* В ESP-IDF 5.x поле revision кодируется как major * 100 + minor. */
    set_row(ROW_CHIP, COL_TEXT, "ESP32-P4 rev %d.%d",
            chip.revision / 100, chip.revision % 100);

    /* --- прошивка --- */
    const esp_app_desc_t *app = esp_app_get_description();
    if (app) {
        set_row(ROW_FIRMWARE, COL_TEXT, "%s   •   %s",
                app->version, app->date);
    }

    /* --- время работы --- */
    int64_t us = esp_timer_get_time();
    int     sec = (int) (us / 1000000);
    if (sec < 3600) {
        set_row(ROW_UPTIME, COL_TEXT, "%d мин %d с", sec / 60, sec % 60);
    } else {
        set_row(ROW_UPTIME, COL_TEXT, "%d ч %d мин",
                sec / 3600, (sec % 3600) / 60);
    }
}

/* ------------------------------------------------------------------ *
 *  Открытие и закрытие
 * ------------------------------------------------------------------ */

void ipradio_diag_ui_open(void (*on_close)(void *ctx), void *ctx)
{
    if (!s_screen) {
        return;
    }

    s_on_close = on_close;
    s_ctx      = ctx;
    s_visible  = true;

    ipradio_snapshot_t snap;
    ipradio_get(&snap);
    ipradio_diag_ui_update(&snap);

    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_screen);
}

void ipradio_diag_ui_close(void)
{
    if (!s_screen || !s_visible) {
        return;
    }

    void (*cb)(void *) = s_on_close;
    void *ctx = s_ctx;

    s_visible  = false;
    s_on_close = NULL;
    s_ctx      = NULL;

    lv_obj_add_flag(s_screen, LV_OBJ_FLAG_HIDDEN);

    if (cb) {
        cb(ctx);
    }
}

bool ipradio_diag_ui_visible(void)
{
    return s_visible;
}

void ipradio_diag_ui_back(void)
{
    ipradio_diag_ui_close();
}
