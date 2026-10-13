/*
 * ipradio_ui_search.c — поиск станций в каталоге.
 *
 * Замысел — в ipradio_ui_search.h.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"

#include "esp_attr.h"
#include "esp_heap_caps.h"

#include "esp_log.h"

#include "ipradio_fonts.h"
#include "ipradio_net.h"
#include "ipradio_preset_edit.h"
#include "ipradio_state.h"
#include "ipradio_storage.h"
#include "ipradio_ui_keyboard.h"
#include "ipradio_ui_search.h"
#include "ipradio_ui_theme.h"

static const char *TAG = "ui.search";

#define ROW_H     78
#define SIDE_PAD  48

static lv_obj_t *s_screen;
static lv_obj_t *s_head;
static lv_obj_t *s_status;
static lv_obj_t *s_list;
static lv_obj_t *s_row[IPRADIO_SEARCH_MAX];
static lv_obj_t *s_row_name[IPRADIO_SEARCH_MAX];
static lv_obj_t *s_row_info[IPRADIO_SEARCH_MAX];

/* Двадцать станций по 314 байт - самый крупный статический объект
 * во всей прошивке. Внутренней памяти он не нужен: сюда пишет
 * задача поиска, читает задача интерфейса, из прерывания никто. */
EXT_RAM_BSS_ATTR static ipradio_station_t s_found[IPRADIO_SEARCH_MAX];
static int  s_found_count;
static int  s_focus;
static bool s_visible;
static bool s_busy;

static char s_query[64];

/* Куда записать найденное: 0 - обычный поиск (туда, где станция уже
 * есть, иначе в первую свободную), 1…N - замена из меню ячейки. */
static int s_target;

/* Итог запроса из задачи поиска в задачу интерфейса. */
static SemaphoreHandle_t s_lock;
EXT_RAM_BSS_ATTR static ipradio_station_t s_pending[IPRADIO_SEARCH_MAX];
static int               s_pending_count = -1;
static bool              s_pending_failed;  /* каталог не ответил вовсе */

static void (*s_on_close)(void *ctx);
static void  *s_ctx;

/* ------------------------------------------------------------------ *
 *  Список
 * ------------------------------------------------------------------ */

static void paint_focus(void)
{
    for (int i = 0; i < IPRADIO_SEARCH_MAX; i++) {
        bool on = (i == s_focus);
        lv_obj_set_style_bg_color(s_row[i], on ? lv_color_hex(0x0f2529)
                                               : COL_SURFACE, 0);
        lv_obj_set_style_border_color(s_row[i], on ? COL_CYAN : COL_BORDER, 0);
        lv_obj_set_style_border_width(s_row[i], on ? 2 : 1, 0);
        lv_obj_set_style_text_color(s_row_name[i],
                                    on ? COL_TEXT : COL_TEXT_DIM, 0);
    }

    if (s_found_count > 0 && s_focus < s_found_count) {
        lv_obj_scroll_to_view(s_row[s_focus], LV_ANIM_ON);
    }
}

static void render_list(void)
{
    for (int i = 0; i < IPRADIO_SEARCH_MAX; i++) {
        if (i >= s_found_count) {
            lv_obj_add_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_row_name[i], s_found[i].name);

        /* Кодек и битрейт показываем не из любви к цифрам: по ним
         * человек отличает станцию, которая заиграет хорошо, от той,
         * что будет булькать. Битрейт 0 каталог отдаёт, когда сам
         * его не знает, — тогда и не пишем. */
        char info[128];
        if (s_found[i].bitrate > 0) {
            snprintf(info, sizeof(info), "%s   •   %s   •   %u кбит/с",
                     s_found[i].countrycode[0] ? s_found[i].countrycode : "—",
                     s_found[i].codec[0] ? s_found[i].codec : "?",
                     (unsigned) s_found[i].bitrate);
        } else {
            snprintf(info, sizeof(info), "%s   •   %s",
                     s_found[i].countrycode[0] ? s_found[i].countrycode : "—",
                     s_found[i].codec[0] ? s_found[i].codec : "?");
        }
        lv_label_set_text(s_row_info[i], info);
    }

    paint_focus();
}

/* ------------------------------------------------------------------ *
 *  Запрос к каталогу
 * ------------------------------------------------------------------ */

/* Задача одноразовая: запрос уходит, ответ кладётся, задача умирает.
 * Держать её постоянно незачем — поиск бывает раз в несколько дней,
 * а стек под HTTP и разбор JSON нужен немаленький. */
static void search_task(void *arg)
{
    (void) arg;

    ipradio_station_t found[IPRADIO_SEARCH_MAX];
    ESP_LOGW(TAG, "ЗАМЕР: ухожу в каталог за «%s»", s_query);
    int n = ipradio_net_search(s_query, found, IPRADIO_SEARCH_MAX);
    ESP_LOGW(TAG, "ЗАМЕР: каталог вернул %d", n);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (n > 0) {
        memcpy(s_pending, found, sizeof(found[0]) * n);
    }
    s_pending_count  = (n < 0) ? 0 : n;
    s_pending_failed = (n < 0);
    xSemaphoreGive(s_lock);

    vTaskDeleteWithCaps(NULL);
}

static void on_query(const char *text, void *ctx)
{
    (void) ctx;

    ESP_LOGW(TAG, "ЗАМЕР: запрос от клавиатуры = «%s»",
             text ? text : "(отказ)");

    if (!text || !text[0]) {
        /* Отказались от ввода. Экран поиска при этом закрываем:
         * оставлять его пустым незачем — искать всё равно нечего. */
        ipradio_search_ui_close();
        return;
    }

    snprintf(s_query, sizeof(s_query), "%s", text);

    s_busy          = true;
    s_found_count   = 0;
    s_focus         = 0;
    s_pending_count = -1;

    for (int i = 0; i < IPRADIO_SEARCH_MAX; i++) {
        lv_obj_add_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);
    }

    char buf[96];
    snprintf(buf, sizeof(buf), "Ищем «%s»…", s_query);
    lv_label_set_text(s_status, buf);

    /* Стек ЗАДАЧИ - В PSRAM, и это не роскошь.
     *
     * Обычный xTaskCreate берёт стек из внутренней памяти, а её
     * на этой плате почти не осталось: LVGL с двенадцатью экранами
     * делает тысячи мелких выделений, и все они по настройке
     * SPIRAM_MALLOC_ALWAYSINTERNAL уходят внутрь. На плате
     * 2026-09-05 создание этой задачи просто не удавалось - поиск
     * молчал, а человек видел «ничего не нашлось».
     *
     * Задача разовая, живёт секунды, ничего не делает при
     * запрещённом кэше - для внешнего стека случай подходящий.
     * Удаляться она обязана парной функцией, иначе память стека
     * не вернётся. */
    if (xTaskCreateWithCaps(search_task, "cat_search", 12288, NULL, 4,
                            NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "задача поиска не создалась");
        lv_label_set_text(s_status, "Не хватило памяти для поиска");
        s_busy = false;
    }
}

void ipradio_search_ui_poll(void)
{
    if (!s_visible) {
        return;
    }

    int  n = -1;
    bool failed = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_pending_count >= 0) {
        n = s_pending_count;
        failed = s_pending_failed;
        if (n > 0) {
            memcpy(s_found, s_pending, sizeof(s_found[0]) * n);
        }
        s_pending_count = -1;
    }
    xSemaphoreGive(s_lock);

    if (n < 0) {
        return;
    }

    s_found_count = n;
    s_busy        = false;
    s_focus       = 0;

    if (failed) {
        /* Не «ничего не нашлось»: с ним человек меняет запрос, а дело
         * в связи. Нажатие на пустом экране спросит запрос заново. */
        lv_label_set_text(s_status,
            "Каталог не ответил - проверьте сеть. Нажмите, чтобы "
            "попробовать ещё раз");
    } else if (n == 0) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "По запросу «%s» ничего не нашлось", s_query);
        lv_label_set_text(s_status, buf);
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "Найдено: %d   •   выберите станцию", n);
        lv_label_set_text(s_status, buf);
    }

    render_list();
}

/* ------------------------------------------------------------------ *
 *  Сохранение выбранной
 * ------------------------------------------------------------------ */

/* Записать станцию в банк и включить её.
 *
 * Обычный поиск кладёт станцию туда, где она уже есть, иначе в первую
 * свободную ячейку. Поиск из меню ячейки («Заменить») - в ту ячейку,
 * с которой его открыли (s_target).
 *
 * Свободной ячейки может не быть. Молча ничего не делать нельзя,
 * подменять чужую ячейку без спроса - тем более: человек её сам туда
 * положил. Поэтому говорим, в чём дело, и как это поправить.
 *
 * Правка банка - в ipradio_preset_edit.c. До 2026-10-06 она была
 * написана здесь же, и при полном банке функция возвращалась, не
 * отпустив замок хранилища: следующее обращение к карте из любой
 * задачи вставало навсегда. */
static void save_and_play(int idx)
{
    int cell = 0;
    ipradio_pedit_t r = ipradio_preset_put_net(s_target,
                                               s_found[idx].name,
                                               s_found[idx].url,
                                               s_found[idx].uuid, &cell);
    switch (r) {
    case IPRADIO_PEDIT_OK:
        break;
    case IPRADIO_PEDIT_FULL:
        lv_label_set_text(s_status,
            "Все ячейки заняты. Долгое нажатие на ячейку главного "
            "экрана - заменить или удалить станцию.");
        return;
    case IPRADIO_PEDIT_NO_CARD:
        lv_label_set_text(s_status, "Карты памяти нет - записать некуда");
        return;
    case IPRADIO_PEDIT_BAD_ARG:
        lv_label_set_text(s_status,
            "Адрес потока этой станции не помещается в память прибора");
        return;
    case IPRADIO_PEDIT_WRITE_FAILED:
    default:
        lv_label_set_text(s_status, "Не удалось записать на карту");
        return;
    }

    ESP_LOGI(TAG, "станция «%s» в ячейке %d", s_found[idx].name, cell);

    /* Отметить запуск в каталоге — вежливость, по ней там строится
     * рейтинг (§4). Не наша обязанность, но нам это ничего не стоит. */
    ipradio_net_report_click_async(s_found[idx].uuid);

    /* Включаем ИНТЕРНЕТНУЮ ячейку из любого режима. Обычное нажатие
     * пресета для этого не годится с разделения банков: оно берёт
     * ячейку текущего режима, и из эфира включилась бы эфирная
     * станция с тем же номером. */
    ipradio_post_simple(IPRADIO_EV_NET_PRESET_PRESSED, cell);

    ipradio_search_ui_close();
}

/* ------------------------------------------------------------------ *
 *  Сборка
 * ------------------------------------------------------------------ */

static void on_row_click(lv_event_t *e)
{
    int idx = (int) (intptr_t) lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_found_count) {
        return;
    }
    s_focus = idx;
    paint_focus();
    save_and_play(idx);
}

esp_err_t ipradio_search_ui_init(lv_obj_t *parent)
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

    s_head = ipradio_ui_label(s_screen, ipradio_font_28,
                              COL_TEXT, "Поиск станций");
    lv_obj_align(s_head, LV_ALIGN_TOP_LEFT, SIDE_PAD, 30);

    s_status = ipradio_ui_label(s_screen, ipradio_font_16, COL_TEXT_DIM, "");
    lv_obj_set_width(s_status, LV_PCT(88));
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_DOT);
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

    for (int i = 0; i < IPRADIO_SEARCH_MAX; i++) {
        s_row[i] = ipradio_ui_panel(s_list, LV_PCT(100), ROW_H,
                                    COL_SURFACE, COL_BORDER, 12);
        lv_obj_set_style_pad_hor(s_row[i], 22, 0);
        lv_obj_add_flag(s_row[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_row[i], on_row_click, LV_EVENT_CLICKED,
                            (void *) (intptr_t) i);

        s_row_name[i] = ipradio_ui_label(s_row[i], ipradio_font_22,
                                         COL_TEXT_DIM, "");
        lv_obj_set_width(s_row_name[i], LV_PCT(92));
        lv_label_set_long_mode(s_row_name[i], LV_LABEL_LONG_DOT);
        lv_obj_align(s_row_name[i], LV_ALIGN_LEFT_MID, 0, -12);

        s_row_info[i] = ipradio_ui_label(s_row[i], ipradio_font_14,
                                         COL_TEXT_FAINT, "");
        lv_obj_align(s_row_info[i], LV_ALIGN_LEFT_MID, 0, 16);

        lv_obj_add_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *hints = ipradio_ui_label(s_screen, ipradio_font_20,
        COL_TEXT,
        "Регулятор 1 — выбор   •   нажатие — сохранить и включить   •   "
        "нажатие регулятора 2 — назад");
    lv_obj_align(hints, LV_ALIGN_BOTTOM_MID, 0, -28);

    lv_obj_add_flag(s_screen, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "экран поиска станций готов");
    return ESP_OK;
}

/* ------------------------------------------------------------------ *
 *  Открытие и закрытие
 * ------------------------------------------------------------------ */

/* Вопрос на клавиатуре. При замене говорим, какую ячейку заменяем:
 * человек мог отвлечься между меню ячейки и вводом. */
static void ask_query(const char *initial)
{
    char title[96];
    if (s_target > 0) {
        snprintf(title, sizeof(title),
                 "Станция для ячейки П%d: название или жанр", s_target);
    } else {
        snprintf(title, sizeof(title), "Название станции или жанр");
    }
    ipradio_keyboard_open(title, initial, false, on_query, NULL);
}

void ipradio_search_ui_open_into(int cell, void (*on_close)(void *ctx),
                                 void *ctx)
{
    if (!s_screen) {
        return;
    }

    s_target      = (cell >= 1 && cell <= IPRADIO_PRESET_MAX) ? cell : 0;
    s_on_close    = on_close;
    s_ctx         = ctx;
    s_visible     = true;
    s_busy        = false;
    s_found_count = 0;
    s_focus       = 0;

    char head[48];
    if (s_target > 0) {
        snprintf(head, sizeof(head), "Замена станции в ячейке П%d", s_target);
    } else {
        snprintf(head, sizeof(head), "Поиск станций");
    }
    lv_label_set_text(s_head, head);

    for (int i = 0; i < IPRADIO_SEARCH_MAX; i++) {
        lv_obj_add_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(s_status, "");

    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_screen);

    /* Сразу спрашиваем, что искать: пустой экран со списком, которого
     * ещё нет, ничего не объясняет. */
    ask_query(NULL);
}

void ipradio_search_ui_open(void (*on_close)(void *ctx), void *ctx)
{
    ipradio_search_ui_open_into(0, on_close, ctx);
}

void ipradio_search_ui_close(void)
{
    if (!s_screen || !s_visible) {
        return;
    }

    void (*cb)(void *) = s_on_close;
    void *ctx = s_ctx;

    s_visible  = false;
    s_on_close = NULL;
    s_ctx      = NULL;
    s_target   = 0;

    lv_obj_add_flag(s_screen, LV_OBJ_FLAG_HIDDEN);

    if (cb) {
        cb(ctx);
    }
}

bool ipradio_search_ui_visible(void)
{
    return s_visible;
}

/* ------------------------------------------------------------------ *
 *  Органы управления
 * ------------------------------------------------------------------ */

void ipradio_search_ui_move(int delta)
{
    if (!s_visible || s_busy || s_found_count == 0 || delta == 0) {
        return;
    }

    s_focus = (s_focus + delta) % s_found_count;
    if (s_focus < 0) {
        s_focus += s_found_count;
    }
    paint_focus();
}

void ipradio_search_ui_select(void)
{
    if (!s_visible || s_busy) {
        return;
    }

    if (s_found_count == 0) {
        /* Ничего не нашлось — нажатие означает «спросить заново».
         * Иначе из этого состояния был бы только выход. */
        ask_query(s_query);
        return;
    }

    if (s_focus >= 0 && s_focus < s_found_count) {
        save_and_play(s_focus);
    }
}

void ipradio_search_ui_back(void)
{
    ipradio_search_ui_close();
}
