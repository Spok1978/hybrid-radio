/*
 * ipradio_ui_clock.c — часы.
 *
 * Замысел — в ipradio_ui_clock.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"

#include "ipradio_fonts.h"
#include "ipradio_storage.h"
#include "ipradio_ui_clock.h"
#include "ipradio_ui_theme.h"

static const char *TAG = "ui.clock";

#define SIDE_PAD  48
#define ROW_H     72

/* Часовые пояса России. Смещение в строке POSIX TZ ОБРАТНОЕ привычному:
 * «UTC-3» означает UTC+3. Это не опечатка, а стандарт, и ровно поэтому
 * человеку такую строку показывать нельзя — он прочтёт её наоборот.
 *
 * Переходов на летнее время в России нет с 2014 года, поэтому правила
 * перехода в строках не нужны. */
static const struct {
    const char *title;
    const char *tz;
} ZONES[] = {
    { "Калининград   UTC+2",  "UTC-2"  },
    { "Москва   UTC+3",       "UTC-3"  },
    { "Самара   UTC+4",       "UTC-4"  },
    { "Екатеринбург   UTC+5", "UTC-5"  },
    { "Омск   UTC+6",         "UTC-6"  },
    { "Красноярск   UTC+7",   "UTC-7"  },
    { "Иркутск   UTC+8",      "UTC-8"  },
    { "Якутск   UTC+9",       "UTC-9"  },
    { "Владивосток   UTC+10", "UTC-10" },
    { "Магадан   UTC+11",     "UTC-11" },
    { "Камчатка   UTC+12",    "UTC-12" },
};
#define ZONE_COUNT (sizeof(ZONES) / sizeof(ZONES[0]))

enum {
    ROW_NOW = 0,     /* текущее время, только показ  */
    ROW_SOURCE,      /* NTP или вручную              */
    ROW_ZONE,
    ROW_FORMAT,
    ROW_SET,         /* установить вручную           */
    ROW_COUNT,
};

static const char *const LABELS[ROW_COUNT] = {
    [ROW_NOW]    = "Сейчас",
    [ROW_SOURCE] = "Источник времени",
    [ROW_ZONE]   = "Часовой пояс",
    [ROW_FORMAT] = "Формат",
    [ROW_SET]    = "Установить вручную",
};

static lv_obj_t *s_screen;
static lv_obj_t *s_row[ROW_COUNT];
static lv_obj_t *s_value[ROW_COUNT];
static lv_obj_t *s_hint;

static int  s_focus;
static bool s_visible;

/* Настройки, с которыми работаем. Пишем на карту при выходе, а не
 * на каждое нажатие: подряд меняют обычно несколько полей. */
static ipradio_settings_t s_set;
static int  s_zone;
static bool s_ntp = true;

/* Правка даты и времени. Поле за полем, регулятор меняет значение,
 * нажатие переходит к следующему; после последнего время
 * применяется. Отдельного экрана не нужно — правим прямо в строке. */
static bool      s_editing;
static int       s_field;      /* 0 ч, 1 мин, 2 день, 3 месяц, 4 год */
static struct tm s_edit;

#define FIELD_COUNT 5

static void (*s_on_close)(void *ctx);
static void  *s_ctx;

/* ------------------------------------------------------------------ *
 *  Отрисовка
 * ------------------------------------------------------------------ */

static void paint_focus(void)
{
    for (int i = 0; i < ROW_COUNT; i++) {
        bool on = (i == s_focus) && !s_editing;

        lv_obj_set_style_bg_color(s_row[i], on ? lv_color_hex(0x1e2228)
                                               : COL_SURFACE, 0);
        lv_obj_set_style_border_color(s_row[i], on ? COL_AMBER : COL_BORDER, 0);
        lv_obj_set_style_border_width(s_row[i], on ? 2 : 1, 0);
    }

    /* В режиме правки выделяем саму строку правки, а не выбор. */
    if (s_editing) {
        lv_obj_set_style_border_color(s_row[ROW_SET], COL_GREEN, 0);
        lv_obj_set_style_border_width(s_row[ROW_SET], 2, 0);
    }
}

static void render(void)
{
    char buf[96];

    /* --- текущее время --- */
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);

    if (s_set.clock_24h) {
        strftime(buf, sizeof(buf), "%H:%M:%S   %d.%m.%Y", &tmv);
    } else {
        strftime(buf, sizeof(buf), "%I:%M:%S %p   %d.%m.%Y", &tmv);
    }
    lv_label_set_text(s_value[ROW_NOW], buf);

    /* Год до 1971-го означает, что часы не выставлены: система
     * стартует с начала эпохи. Пишем это прямо, иначе человек
     * решит, что прибор показывает чушь. */
    bool unset = (tmv.tm_year + 1900) < 1971;
    lv_obj_set_style_text_color(s_value[ROW_NOW],
                                unset ? COL_RED : COL_TEXT, 0);
    if (unset) {
        lv_label_set_text(s_value[ROW_NOW], "не выставлены");
    }

    lv_label_set_text(s_value[ROW_SOURCE],
                      s_ntp ? "из сети (NTP)" : "вручную");

    lv_label_set_text(s_value[ROW_ZONE], ZONES[s_zone].title);

    lv_label_set_text(s_value[ROW_FORMAT],
                      s_set.clock_24h ? "24 часа" : "AM / PM");

    /* --- строка правки --- */
    if (!s_editing) {
        lv_label_set_text(s_value[ROW_SET],
                          s_ntp ? "не нужно — время из сети" : "нажмите");
        lv_obj_set_style_text_color(s_value[ROW_SET],
                                    s_ntp ? COL_TEXT_FAINT : COL_AMBER, 0);
    } else {
        /* Поле, которое сейчас правится, помечено скобками: цветом
         * одну цифру из пяти не выделить так, чтобы это читалось
         * через комнату. */
        const char *o[FIELD_COUNT] = { "", "", "", "", "" };
        const char *c[FIELD_COUNT] = { "", "", "", "", "" };
        o[s_field] = "[";
        c[s_field] = "]";

        snprintf(buf, sizeof(buf),
                 "%s%02d%s:%s%02d%s   %s%02d%s.%s%02d%s.%s%04d%s",
                 o[0], s_edit.tm_hour, c[0],
                 o[1], s_edit.tm_min,  c[1],
                 o[2], s_edit.tm_mday, c[2],
                 o[3], s_edit.tm_mon + 1, c[3],
                 o[4], s_edit.tm_year + 1900, c[4]);
        lv_label_set_text(s_value[ROW_SET], buf);
        lv_obj_set_style_text_color(s_value[ROW_SET], COL_GREEN, 0);
    }

    paint_focus();
}

void ipradio_clock_ui_poll(void)
{
    if (s_visible) {
        render();
    }
}

/* ------------------------------------------------------------------ *
 *  Правка времени
 * ------------------------------------------------------------------ */

/* Сколько дней в месяце. Нужно, чтобы 31 февраля выставить было
 * нельзя: mktime такую дату молча исправит, и человек увидит не то,
 * что вводил. */
static int days_in_month(int mon, int year)
{
    static const int d[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (mon == 1) {
        bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        return leap ? 29 : 28;
    }
    return d[mon];
}

static void edit_field(int delta)
{
    switch (s_field) {
    case 0:
        s_edit.tm_hour = (s_edit.tm_hour + delta + 24) % 24;
        break;
    case 1:
        s_edit.tm_min = (s_edit.tm_min + delta + 60) % 60;
        break;
    case 2: {
        int max = days_in_month(s_edit.tm_mon, s_edit.tm_year + 1900);
        s_edit.tm_mday = ((s_edit.tm_mday - 1 + delta + max) % max) + 1;
        break;
    }
    case 3:
        s_edit.tm_mon = (s_edit.tm_mon + delta + 12) % 12;
        /* День мог оказаться за краем нового месяца — подтягиваем. */
        {
            int max = days_in_month(s_edit.tm_mon, s_edit.tm_year + 1900);
            if (s_edit.tm_mday > max) {
                s_edit.tm_mday = max;
            }
        }
        break;
    case 4: {
        /* Разумные пределы: ниже 2020 прибора не существовало,
         * выше 2099 незачем. */
        int y = s_edit.tm_year + 1900 + delta;
        if (y < 2020) y = 2020;
        if (y > 2099) y = 2099;
        s_edit.tm_year = y - 1900;
        break;
    }
    default:
        break;
    }
    render();
}

static void apply_manual_time(void)
{
    s_edit.tm_sec  = 0;
    s_edit.tm_isdst = 0;

    time_t t = mktime(&s_edit);
    if (t == (time_t) -1) {
        ESP_LOGW(TAG, "дату применить не удалось");
        return;
    }

    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    settimeofday(&tv, NULL);

    ESP_LOGI(TAG, "время выставлено вручную");
}

static void start_edit(void)
{
    time_t now = time(NULL);
    localtime_r(&now, &s_edit);

    /* Если часы не выставлены, начинаем не с 1970-го: крутить
     * полвека регулятором человек не станет. */
    if (s_edit.tm_year + 1900 < 2020) {
        s_edit.tm_year = 2026 - 1900;
        s_edit.tm_mon  = 0;
        s_edit.tm_mday = 1;
        s_edit.tm_hour = 12;
        s_edit.tm_min  = 0;
    }

    s_editing = true;
    s_field   = 0;
    lv_label_set_text(s_hint,
        "Регулятор 1 — значение   •   нажатие — следующее поле");
    render();
}

/* ------------------------------------------------------------------ *
 *  Сборка
 * ------------------------------------------------------------------ */

/* Нажатие по строке с экрана.
 *
 * Экран задумывался под регуляторы: выбор строки поворотом, вход
 * нажатием. Регуляторов пока нет, а настроить часы надо - поэтому
 * то же самое доступно пальцем. Нажатие делает две вещи разом:
 * переводит выбор на строку и сразу входит в неё, иначе пришлось бы
 * тыкать дважды. */
static void on_row_click(lv_event_t *e)
{
    int row = (int) (intptr_t) lv_event_get_user_data(e);
    if (row < 0 || row >= ROW_COUNT || row == ROW_NOW) {
        return;
    }
    s_focus = row;
    ipradio_clock_ui_select();
}

esp_err_t ipradio_clock_ui_init(lv_obj_t *parent)
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
                                      COL_TEXT, "Часы");
    lv_obj_align(head, LV_ALIGN_TOP_LEFT, SIDE_PAD, 30);

    lv_obj_t *list = lv_obj_create(s_screen);
    lv_obj_set_size(list, LV_PCT(100), ROW_H * ROW_COUNT + 40);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 100);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_hor(list, SIDE_PAD, 0);
    lv_obj_set_style_pad_ver(list, 0, 0);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < ROW_COUNT; i++) {
        s_row[i] = ipradio_ui_panel(list, LV_PCT(100), ROW_H,
                                    COL_SURFACE, COL_BORDER, 12);
        lv_obj_set_style_pad_hor(s_row[i], 22, 0);

        lv_obj_t *name = ipradio_ui_label(s_row[i], ipradio_font_22,
                                          COL_TEXT_FAINT, LABELS[i]);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);

        s_value[i] = ipradio_ui_label(s_row[i], ipradio_font_28,
                                      COL_TEXT, "");
        lv_obj_align(s_value[i], LV_ALIGN_RIGHT_MID, 0, 0);

        if (i != ROW_NOW) {
            lv_obj_add_flag(s_row[i], LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(s_row[i], on_row_click, LV_EVENT_CLICKED,
                                (void *) (intptr_t) i);
        }
    }

    /* Строка «Сейчас» ничего не делает — это не настройка, а показ.
     * Гасим у неё рамку, чтобы она не выглядела нажимаемой. */
    lv_obj_set_style_bg_opa(s_row[ROW_NOW], LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_row[ROW_NOW], 0, 0);

    s_hint = ipradio_ui_label(s_screen, ipradio_font_22, COL_TEXT, "");
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -28);

    lv_obj_add_flag(s_screen, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "экран часов готов");
    return ESP_OK;
}

/* ------------------------------------------------------------------ *
 *  Открытие и закрытие
 * ------------------------------------------------------------------ */

void ipradio_clock_ui_open(void (*on_close)(void *ctx), void *ctx)
{
    if (!s_screen) {
        return;
    }

    s_on_close = on_close;
    s_ctx      = ctx;
    s_visible  = true;
    s_editing  = false;
    s_focus    = ROW_SOURCE;   /* на первой настройке, а не на показе */

    /* Настройки берём отдельным вызовом, а не целым хранилищем.
     *
     * Раньше здесь стоял ipradio_store_t на стеке - 2548 байт. Из
     * обработчика касания этот код исполняется в задаче lvgl, и её
     * стека на такое не хватает: панель уходила в перезагрузку
     * ровно на нажатии «назад», а часовой пояс не сохранялся.
     * Та же грабля уже была с ячейками станций. */
    ipradio_storage_get_settings(&s_set);

    /* Ищем сохранённый пояс в списке. Не нашли — Москва: это
     * и умолчание хранилища. */
    s_zone = 1;
    for (size_t i = 0; i < ZONE_COUNT; i++) {
        if (strcmp(s_set.tz, ZONES[i].tz) == 0) {
            s_zone = (int) i;
            break;
        }
    }

    lv_label_set_text(s_hint,
        "Регулятор 1 — выбор   •   нажатие — изменить   •   "
        "нажатие регулятора 2 — назад");

    render();

    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_screen);
}

void ipradio_clock_ui_close(void)
{
    if (!s_screen || !s_visible) {
        return;
    }

    /* Сохраняем на выходе: подряд меняют обычно несколько полей,
     * и писать карту на каждое нажатие незачем. */
    snprintf(s_set.tz, sizeof(s_set.tz), "%s", ZONES[s_zone].tz);

    /* Читать хранилище целиком тут было незачем и вдвойне вредно:
     * прочитанное всё равно затиралось нашим s_set, а 2548 байт
     * на стеке задачи lvgl роняли панель. */
    if (ipradio_storage_save_settings(&s_set) != ESP_OK) {
        ESP_LOGW(TAG, "настройки часов не сохранились");
    }

    /* Пояс применяем немедленно: иначе он вступил бы в силу только
     * после перезагрузки, и человек решил бы, что выбор не сработал. */
    setenv("TZ", s_set.tz, 1);
    tzset();

    void (*cb)(void *) = s_on_close;
    void *ctx = s_ctx;

    s_visible  = false;
    s_editing  = false;
    s_on_close = NULL;
    s_ctx      = NULL;

    lv_obj_add_flag(s_screen, LV_OBJ_FLAG_HIDDEN);

    if (cb) {
        cb(ctx);
    }
}

bool ipradio_clock_ui_visible(void)
{
    return s_visible;
}

/* ------------------------------------------------------------------ *
 *  Органы управления
 * ------------------------------------------------------------------ */

void ipradio_clock_ui_move(int delta)
{
    if (!s_visible || delta == 0) {
        return;
    }

    if (s_editing) {
        edit_field(delta);
        return;
    }

    /* Строку «Сейчас» пропускаем: выбирать в ней нечего. */
    do {
        s_focus += delta;
        if (s_focus >= ROW_COUNT) {
            s_focus = ROW_SOURCE;
        }
        if (s_focus < ROW_SOURCE) {
            s_focus = ROW_COUNT - 1;
        }
    } while (s_focus == ROW_NOW);

    paint_focus();
}

void ipradio_clock_ui_select(void)
{
    if (!s_visible) {
        return;
    }

    if (s_editing) {
        s_field++;
        if (s_field >= FIELD_COUNT) {
            apply_manual_time();
            s_editing = false;
            lv_label_set_text(s_hint,
                "Регулятор 1 — выбор   •   нажатие — изменить   •   "
                "нажатие регулятора 2 — назад");
        }
        render();
        return;
    }

    switch (s_focus) {
    case ROW_SOURCE:
        s_ntp = !s_ntp;
        /* Переключатель ничего не выключает у SNTP: он уже запущен
         * и подтянет время, когда сеть появится. Смысл ручного
         * режима не в том, чтобы запретить сеть, а в том, чтобы
         * дать выставить часы, пока её нет. */
        break;

    case ROW_ZONE:
        s_zone = (s_zone + 1) % (int) ZONE_COUNT;
        /* Применяем сразу: строка «Сейчас» тут же покажет новое
         * время, и выбор становится проверяемым. */
        setenv("TZ", ZONES[s_zone].tz, 1);
        tzset();
        break;

    case ROW_FORMAT:
        s_set.clock_24h = !s_set.clock_24h;
        break;

    case ROW_SET:
        if (!s_ntp) {
            start_edit();
            return;
        }
        break;

    default:
        break;
    }

    render();
}

void ipradio_clock_ui_back(void)
{
    if (s_editing) {
        /* Из правки выходим в список, а не с экрана: иначе кнопка
         * «назад» уводила бы сразу на два уровня. */
        s_editing = false;
        lv_label_set_text(s_hint,
            "Регулятор 1 — выбор   •   нажатие — изменить   •   "
            "нажатие регулятора 2 — назад");
        render();
        return;
    }
    ipradio_clock_ui_close();
}
