/*
 * ipradio_ui_wifi.c — выбор сети Wi-Fi.
 *
 * Замысел — в ipradio_ui_wifi.h.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"

#include "bsp/display.h"

#include "ipradio_fonts.h"
#include "ipradio_net.h"
#include "ipradio_state.h"
#include "ipradio_ui_keyboard.h"
#include "ipradio_ui_theme.h"
#include "ipradio_ui_wifi.h"

static const char *TAG = "ui.wifi";

#define ROW_H       78
#define SIDE_PAD    48

/* Строк на экране на одну больше, чем сетей: последняя — «Искать
 * заново». Повторный поиск нужен чаще, чем кажется: человек включает
 * роутер, возвращается к прибору, а список собран до этого. */
#define ROWS        (IPRADIO_SCAN_MAX + 1)
#define ROW_RESCAN  IPRADIO_SCAN_MAX

static lv_obj_t *s_screen;
static lv_obj_t *s_status;
static lv_obj_t *s_list;
static lv_obj_t *s_row[ROWS];
static lv_obj_t *s_row_name[ROWS];
static lv_obj_t *s_row_info[ROWS];

static ipradio_ap_t s_ap[IPRADIO_SCAN_MAX];
static int          s_ap_count;
static int          s_focus;
static bool         s_visible;
static bool         s_scanning;

/* Итог поиска, переданный из задачи сканирования в задачу интерфейса.
 * Сама задача виджеты не трогает: рисовать можно только там, где
 * крутится LVGL. */
static SemaphoreHandle_t s_lock;
static ipradio_ap_t      s_pending[IPRADIO_SCAN_MAX];
static int               s_pending_count = -1;   /* -1 = нечего забирать */

static void (*s_on_close)(void *ctx);
static void  *s_ctx;

/* ------------------------------------------------------------------ *
 *  Уровень сигнала словами
 * ------------------------------------------------------------------ */

/* Границы взяты по обиходной шкале Wi-Fi: около −60 дБм связь уверенная,
 * около −75 начинаются потери, ниже −85 работать почти невозможно.
 * Точные числа тут не важны — важно, чтобы человек отличил свою сеть
 * в соседней комнате от чужой через две стены. */
static const char *signal_word(int8_t rssi)
{
    if (rssi >= -60) return "отличный";
    if (rssi >= -70) return "хороший";
    if (rssi >= -80) return "слабый";
    return "очень слабый";
}

/* ------------------------------------------------------------------ *
 *  Отрисовка списка
 * ------------------------------------------------------------------ */

static void paint_focus(void)
{
    for (int i = 0; i < ROWS; i++) {
        bool on = (i == s_focus);
        lv_obj_set_style_bg_color(s_row[i], on ? lv_color_hex(0x1e2228)
                                               : COL_SURFACE, 0);
        lv_obj_set_style_border_color(s_row[i], on ? COL_CYAN : COL_BORDER, 0);
        lv_obj_set_style_border_width(s_row[i], on ? 2 : 1, 0);
        lv_obj_set_style_text_color(s_row_name[i],
                                    on ? COL_TEXT : COL_TEXT_DIM, 0);
    }

    if (!lv_obj_has_flag(s_row[s_focus], LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_scroll_to_view(s_row[s_focus], LV_ANIM_ON);
    }
}

static void render_list(void)
{
    const char *current = ipradio_net_ssid();

    for (int i = 0; i < IPRADIO_SCAN_MAX; i++) {
        if (i >= s_ap_count) {
            lv_obj_add_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_row_name[i], s_ap[i].ssid);

        char info[96];
        bool same = (current[0] && strcmp(current, s_ap[i].ssid) == 0);

        /* Про сохранённую сеть пишем прямо: иначе человек вводит
         * пароль заново там, где достаточно было выбрать. */
        snprintf(info, sizeof(info), "%s%s   •   сигнал %s",
                 same ? "сохранена   •   " : "",
                 s_ap[i].open ? "без пароля" : "с паролем",
                 signal_word(s_ap[i].rssi));
        lv_label_set_text(s_row_info[i], info);
    }

    /* Строка «Искать заново» стоит последней и всегда видна. */
    lv_obj_clear_flag(s_row[ROW_RESCAN], LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_row_name[ROW_RESCAN], "Искать заново");
    lv_label_set_text(s_row_info[ROW_RESCAN],
                      "если нужной сети нет в списке");

    if (s_focus >= ROWS) {
        s_focus = ROWS - 1;
    }
    paint_focus();
}

/* ------------------------------------------------------------------ *
 *  Поиск сетей
 * ------------------------------------------------------------------ */

/* Задача одноразовая: запустилась, отсканировала, положила результат
 * и завершилась. Держать её постоянно незачем — поиск бывает раз
 * в несколько месяцев. */
static void scan_task(void *arg)
{
    (void) arg;

    ipradio_ap_t found[IPRADIO_SCAN_MAX];
    int n = ipradio_net_scan(found, IPRADIO_SCAN_MAX);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (n > 0) {
        memcpy(s_pending, found, sizeof(found[0]) * n);
    }
    s_pending_count = (n < 0) ? 0 : n;
    xSemaphoreGive(s_lock);

    vTaskDelete(NULL);
}

static void start_scan(void)
{
    if (s_scanning) {
        return;
    }
    s_scanning = true;

    lv_label_set_text(s_status, "Идёт поиск сетей…");

    for (int i = 0; i < IPRADIO_SCAN_MAX; i++) {
        lv_obj_add_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_ap_count = 0;
    s_focus    = ROW_RESCAN;
    paint_focus();

    xTaskCreate(scan_task, "wifi_scan", 4096, NULL, 4, NULL);
}

/* Забрать результат, если он приехал. Зовётся из задачи интерфейса
 * вместе с остальной перерисовкой. */
static void collect_scan(void)
{
    int n = -1;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_pending_count >= 0) {
        n = s_pending_count;
        if (n > 0) {
            memcpy(s_ap, s_pending, sizeof(s_ap[0]) * n);
        }
        s_pending_count = -1;
    }
    xSemaphoreGive(s_lock);

    if (n < 0) {
        return;   /* ещё ищет */
    }

    s_ap_count = n;
    s_scanning = false;
    s_focus    = (n > 0) ? 0 : ROW_RESCAN;

    if (n == 0) {
        lv_label_set_text(s_status,
            "Сетей не найдено. Проверьте, включён ли роутер.");
    } else {
        lv_label_set_text(s_status, "Выберите сеть");
    }

    render_list();
}

/* ------------------------------------------------------------------ *
 *  Ввод пароля
 * ------------------------------------------------------------------ */

static int s_pending_ap = -1;   /* к какой сети подключаемся */

static void on_password(const char *text, void *ctx)
{
    (void) ctx;

    if (!text || s_pending_ap < 0 || s_pending_ap >= s_ap_count) {
        s_pending_ap = -1;
        return;    /* отказались от ввода */
    }

    ESP_LOGI(TAG, "подключение к «%s»", s_ap[s_pending_ap].ssid);
    lv_label_set_text(s_status, "Подключение…");

    ipradio_net_connect(s_ap[s_pending_ap].ssid, text);
    s_pending_ap = -1;
}

static void connect_to(int idx)
{
    s_pending_ap = idx;

    if (s_ap[idx].open) {
        /* Открытая сеть: пароля нет, спрашивать нечего. */
        on_password("", NULL);
        return;
    }

    char title[96];
    snprintf(title, sizeof(title), "Пароль сети «%s»", s_ap[idx].ssid);
    ipradio_keyboard_open(title, NULL, true, on_password, NULL);
}

/* ------------------------------------------------------------------ *
 *  Сборка
 * ------------------------------------------------------------------ */

static void on_row_click(lv_event_t *e)
{
    int idx = (int) (intptr_t) lv_event_get_user_data(e);
    if (idx < 0 || idx >= ROWS) {
        return;
    }
    s_focus = idx;
    paint_focus();
    ipradio_wifi_ui_select();
}

esp_err_t ipradio_wifi_ui_init(lv_obj_t *parent)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    s_screen = lv_obj_create(parent);
    lv_obj_set_size(s_screen, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_screen, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_radius(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *head = ipradio_ui_label(s_screen, ipradio_font_28,
                                      COL_TEXT, "Wi-Fi");
    lv_obj_align(head, LV_ALIGN_TOP_LEFT, SIDE_PAD, 30);

    s_status = ipradio_ui_label(s_screen, ipradio_font_16, COL_TEXT_DIM, "");
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, SIDE_PAD, 76);

    s_list = lv_obj_create(s_screen);
    lv_obj_set_size(s_list, LV_PCT(100), 720 - 190 - 60);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 120);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_hor(s_list, SIDE_PAD, 0);
    lv_obj_set_style_pad_ver(s_list, 0, 0);
    lv_obj_set_style_pad_row(s_list, 8, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);

    for (int i = 0; i < ROWS; i++) {
        s_row[i] = ipradio_ui_panel(s_list, LV_PCT(100), ROW_H,
                                    COL_SURFACE, COL_BORDER, 12);
        lv_obj_set_style_pad_hor(s_row[i], 22, 0);
        lv_obj_add_flag(s_row[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_row[i], on_row_click, LV_EVENT_CLICKED,
                            (void *) (intptr_t) i);

        s_row_name[i] = ipradio_ui_label(s_row[i], ipradio_font_22,
                                         COL_TEXT_DIM, "");
        lv_obj_align(s_row_name[i], LV_ALIGN_LEFT_MID, 0, -12);

        s_row_info[i] = ipradio_ui_label(s_row[i], ipradio_font_14,
                                         COL_TEXT_FAINT, "");
        lv_obj_align(s_row_info[i], LV_ALIGN_LEFT_MID, 0, 16);

        lv_obj_add_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *hints = ipradio_ui_label(s_screen, ipradio_font_14,
        COL_TEXT_FAINT,
        "Энкодер 1 — выбор   •   нажатие — подключиться   •   "
        "нажатие энкодера 2 — назад");
    lv_obj_align(hints, LV_ALIGN_BOTTOM_MID, 0, -28);

    lv_obj_add_flag(s_screen, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "экран выбора сети готов");
    return ESP_OK;
}

/* ------------------------------------------------------------------ *
 *  Открытие и закрытие
 * ------------------------------------------------------------------ */

void ipradio_wifi_ui_open(void (*on_close)(void *ctx), void *ctx)
{
    if (!s_screen) {
        return;
    }

    s_on_close = on_close;
    s_ctx      = ctx;
    s_visible  = true;

    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_screen);

    start_scan();
}

void ipradio_wifi_ui_close(void)
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

bool ipradio_wifi_ui_visible(void)
{
    return s_visible;
}

void ipradio_wifi_ui_update(const void *snap_v)
{
    const ipradio_snapshot_t *s = (const ipradio_snapshot_t *) snap_v;

    if (!s_screen || !s_visible) {
        return;
    }

    collect_scan();

    /* Пока идёт поиск, состояние подключения не пишем: строка одна,
     * и «идёт поиск» сейчас важнее. */
    if (s_scanning || !s) {
        return;
    }

    if (s->net == IPRADIO_NET_CONNECTED) {
        char buf[96];
        snprintf(buf, sizeof(buf), "Подключено к «%s»", ipradio_net_ssid());
        lv_label_set_text(s_status, buf);
    } else if (s->net == IPRADIO_NET_CONNECTING) {
        lv_label_set_text(s_status, "Подключение…");
    }
}

/* ------------------------------------------------------------------ *
 *  Органы управления
 * ------------------------------------------------------------------ */

void ipradio_wifi_ui_move(int delta)
{
    if (!s_visible || delta == 0 || s_scanning) {
        return;
    }

    /* Видимых строк — найденные сети плюс «Искать заново».
     *
     * Считать приходится в двух системах координат. Логическая: сети
     * идут подряд с нуля, «Искать заново» сразу за ними. Физическая:
     * строка «Искать заново» всегда последняя в массиве, потому что
     * сам массив рассчитан на предельное число сетей. Пересчёт туда
     * и обратно — здесь, чтобы больше нигде о нём не думать. */
    int count = s_ap_count + 1;

    int logical = (s_focus == ROW_RESCAN) ? s_ap_count : s_focus;

    logical += delta;
    logical %= count;
    if (logical < 0) {
        logical += count;
    }

    s_focus = (logical == s_ap_count) ? ROW_RESCAN : logical;

    paint_focus();
}

void ipradio_wifi_ui_select(void)
{
    if (!s_visible || s_scanning) {
        return;
    }

    if (s_focus == ROW_RESCAN) {
        start_scan();
        return;
    }

    if (s_focus >= 0 && s_focus < s_ap_count) {
        connect_to(s_focus);
    }
}

void ipradio_wifi_ui_back(void)
{
    ipradio_wifi_ui_close();
}
