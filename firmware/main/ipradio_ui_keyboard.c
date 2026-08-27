/*
 * ipradio_ui_keyboard.c — экранная клавиатура, экран 07.
 *
 * Замысел — в ipradio_ui_keyboard.h.
 */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "ipradio_fonts.h"
#include "ipradio_ui_keyboard.h"
#include "ipradio_ui_theme.h"

static const char *TAG = "ui.kb";

/* Порог двойного нажатия. 400 мс — обычная для интерфейсов величина;
 * меньше уже требует сноровки, больше начинает срабатывать на два
 * осмысленно раздельных нажатия. */
#define DOUBLE_PRESS_MS   400

/* Служебные клавиши. Обозначены строками, которых нет среди букв,
 * поэтому спутать их с вводимым знаком нельзя. */
#define KEY_LAYOUT   "\x01"   /* смена раскладки  */
#define KEY_SHIFT    "\x02"   /* регистр          */
#define KEY_BACK     "\x03"   /* стереть знак     */
#define KEY_SPACE    "\x04"   /* пробел           */
#define KEY_SAVE     "\x05"   /* сохранить        */

/* Что на них написано. Отдельно от кода клавиши: подпись меняется
 * (у смены раскладки — на название следующей), а код нет. */
#define LBL_BACK     "\u2190"      /* стрелка влево */
#define LBL_SPACE    "пробел"
#define LBL_SAVE     "Сохранить"

typedef enum {
    LAYOUT_RU = 0,
    LAYOUT_EN,
    LAYOUT_SYM,
    LAYOUT_COUNT,
} layout_t;

/* ------------------------------------------------------------------ *
 *  Раскладки
 * ------------------------------------------------------------------ */

/* Нижний ряд одинаков у всех раскладок и потому вынесен в макрос:
 * служебные клавиши обязаны стоять на одном месте, иначе смена
 * раскладки уводила бы «Сохранить» из-под пальца. */
#define BOTTOM_ROW \
    KEY_LAYOUT, KEY_SHIFT, KEY_SPACE, KEY_BACK, KEY_SAVE, ""

static const char *const MAP_RU_LO[] = {
    "й","ц","у","к","е","н","г","ш","щ","з","х","ъ","\n",
    "ф","ы","в","а","п","р","о","л","д","ж","э","\n",
    "я","ч","с","м","и","т","ь","б","ю","ё","\n",
    BOTTOM_ROW
};

static const char *const MAP_RU_UP[] = {
    "Й","Ц","У","К","Е","Н","Г","Ш","Щ","З","Х","Ъ","\n",
    "Ф","Ы","В","А","П","Р","О","Л","Д","Ж","Э","\n",
    "Я","Ч","С","М","И","Т","Ь","Б","Ю","Ё","\n",
    BOTTOM_ROW
};

static const char *const MAP_EN_LO[] = {
    "q","w","e","r","t","y","u","i","o","p","\n",
    "a","s","d","f","g","h","j","k","l","\n",
    "z","x","c","v","b","n","m","\n",
    BOTTOM_ROW
};

static const char *const MAP_EN_UP[] = {
    "Q","W","E","R","T","Y","U","I","O","P","\n",
    "A","S","D","F","G","H","J","K","L","\n",
    "Z","X","C","V","B","N","M","\n",
    BOTTOM_ROW
};

/* Знаки — те, что встречаются в паролях Wi-Fi и в названиях станций.
 * Верхний ряд цифрами, дальше — то, что обычно на клавиатуре
 * над цифрами и рядом с ними. */
static const char *const MAP_SYM[] = {
    "1","2","3","4","5","6","7","8","9","0","\n",
    "!","@","#","$","%","^","&","*","(",")","\n",
    "-","_","=","+","[","]","{","}",";",":","\n",
    "'","\"","\\","/","|","<",">",",",".","?","\n",
    BOTTOM_ROW
};

/* ------------------------------------------------------------------ *
 *  Состояние
 * ------------------------------------------------------------------ */

static lv_obj_t *s_screen;
static lv_obj_t *s_title;
static lv_obj_t *s_field;
static lv_obj_t *s_matrix;
static lv_obj_t *s_hints;

static layout_t s_layout;
static bool     s_upper;
static bool     s_password;
static uint32_t s_selected;
static bool     s_visible;

static int64_t  s_last_press_us;

/* Вставило ли предыдущее нажатие знак в поле. Нужно из-за двойного
 * нажатия: первое нажатие успевает ввести букву, и если второе
 * означало «сохранить», эту букву надо забрать обратно. Иначе каждое
 * сохранение добавляло бы к тексту лишний знак.
 *
 * Другой путь — придержать одиночное нажатие на время порога и решить
 * потом, — сделал бы набор заметно вялым: буква появлялась бы через
 * 400 мс после нажатия. Быстрый набор важнее, чем аккуратность
 * в редком случае. */
static bool s_last_was_insert;

/* Сколько клавиш в текущей раскладке. Считается при её смене:
 * пересчитывать на каждый щелчок энкодера значило бы обходить всю
 * карту по десять раз в секунду. */
static uint32_t s_button_count;

static ipradio_kb_cb_t s_cb;
static void           *s_ctx;

/* Копия текущей карты с подставленными подписями служебных клавиш.
 * Сама раскладка const, а подпись у смены раскладки меняется — значит
 * отдавать виджету надо копию, а не константу. */
static const char *s_map[80];

/* ------------------------------------------------------------------ *
 *  Карта
 * ------------------------------------------------------------------ */

static const char *const *raw_map(void)
{
    switch (s_layout) {
    case LAYOUT_EN:  return s_upper ? MAP_EN_UP : MAP_EN_LO;
    case LAYOUT_SYM: return MAP_SYM;
    case LAYOUT_RU:
    default:         return s_upper ? MAP_RU_UP : MAP_RU_LO;
    }
}

/* Название СЛЕДУЮЩЕЙ раскладки, а не текущей: на клавише переключения
 * надо писать, куда она приведёт. Написать текущую — обычная ошибка,
 * после которой человек жмёт клавишу дважды. */
static const char *next_layout_label(void)
{
    switch (s_layout) {
    case LAYOUT_RU:  return "ABC";
    case LAYOUT_EN:  return "!#1";
    case LAYOUT_SYM:
    default:         return "АБВ";
    }
}

static uint32_t rebuild_map(void)
{
    const char *const *src = raw_map();
    uint32_t buttons = 0;
    size_t   n = 0;

    for (size_t i = 0; src[i] && n < (sizeof(s_map) / sizeof(s_map[0])) - 1; i++) {
        const char *k = src[i];

        if      (strcmp(k, KEY_LAYOUT) == 0) k = next_layout_label();
        else if (strcmp(k, KEY_SHIFT)  == 0) k = s_upper ? "абв" : "АБВ";
        else if (strcmp(k, KEY_BACK)   == 0) k = LBL_BACK;
        else if (strcmp(k, KEY_SPACE)  == 0) k = LBL_SPACE;
        else if (strcmp(k, KEY_SAVE)   == 0) k = LBL_SAVE;

        s_map[n++] = k;

        if (strcmp(src[i], "\n") != 0) {
            buttons++;
        }
    }
    s_map[n] = "";   /* признак конца карты для LVGL */

    return buttons;
}

/* Обратный разбор: по номеру кнопки — что это за клавиша в ИСХОДНОЙ
 * карте. Идём по ней же, потому что подписи в s_map уже подменены
 * и по ним служебную клавишу не опознать. */
static const char *key_at(uint32_t id)
{
    const char *const *src = raw_map();
    uint32_t n = 0;

    for (size_t i = 0; src[i] && src[i][0]; i++) {
        if (strcmp(src[i], "\n") == 0) {
            continue;
        }
        if (n == id) {
            return src[i];
        }
        n++;
    }
    return NULL;
}

static void apply_layout(void)
{
    s_button_count = rebuild_map();

    lv_buttonmatrix_set_map(s_matrix, s_map);

    if (s_selected >= s_button_count) {
        s_selected = s_button_count ? s_button_count - 1 : 0;
    }
    lv_buttonmatrix_set_selected_button(s_matrix, s_selected);
}

/* ------------------------------------------------------------------ *
 *  Ввод
 * ------------------------------------------------------------------ */

static void finish(bool save)
{
    ipradio_kb_cb_t cb = s_cb;
    void *ctx = s_ctx;

    char text[IPRADIO_KB_TEXT_MAX];
    text[0] = '\0';
    if (save) {
        const char *cur = lv_textarea_get_text(s_field);
        if (cur) {
            strncpy(text, cur, sizeof(text) - 1);
            text[sizeof(text) - 1] = '\0';
        }
    }

    s_visible = false;
    s_cb      = NULL;
    s_ctx     = NULL;
    lv_obj_add_flag(s_screen, LV_OBJ_FLAG_HIDDEN);

    if (cb) {
        cb(save ? text : NULL, ctx);
    }
}

/* Возвращает true, если в поле добавился знак. */
static bool press_key(const char *key)
{
    if (!key) {
        return false;
    }

    if (strcmp(key, KEY_LAYOUT) == 0) {
        s_layout = (layout_t) ((s_layout + 1) % LAYOUT_COUNT);
        apply_layout();
        return false;
    }

    if (strcmp(key, KEY_SHIFT) == 0) {
        /* В раскладке знаков регистра нет — там клавиша просто
         * ничего не делает, и это честнее, чем прятать её: клавиши,
         * появляющиеся и исчезающие, сбивают счёт при выборе
         * крутилкой. */
        s_upper = !s_upper;
        apply_layout();
        return false;
    }

    if (strcmp(key, KEY_BACK) == 0) {
        lv_textarea_delete_char(s_field);
        return false;
    }

    if (strcmp(key, KEY_SAVE) == 0) {
        finish(true);
        return false;
    }

    if (strcmp(key, KEY_SPACE) == 0) {
        lv_textarea_add_text(s_field, " ");
        return true;
    }

    /* Обычная буква. Длину проверяем сами: lv_textarea умеет
     * ограничивать, но молча, а нам надо не пустить лишнее
     * в буфер фиксированного размера у вызывающего. */
    const char *cur = lv_textarea_get_text(s_field);
    if (cur && strlen(cur) + strlen(key) < IPRADIO_KB_TEXT_MAX) {
        lv_textarea_add_text(s_field, key);
        return true;
    }
    return false;
}

static void on_matrix_click(lv_event_t *e)
{
    uint32_t id = lv_buttonmatrix_get_selected_button(s_matrix);
    (void) e;
    s_selected = id;

    /* Касание экрана — не двойное нажатие энкодера. Сбрасываем отсчёт,
     * иначе касание сразу после нажатия кнопки посчиталось бы вторым
     * нажатием и сохранило бы ввод посреди набора. */
    s_last_press_us   = 0;
    s_last_was_insert = false;

    press_key(key_at(id));
}

/* ------------------------------------------------------------------ *
 *  Сборка
 * ------------------------------------------------------------------ */

esp_err_t ipradio_keyboard_init(lv_obj_t *parent)
{
    s_screen = lv_obj_create(parent);
    lv_obj_set_size(s_screen, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_screen, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_radius(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    s_title = ipradio_ui_label(s_screen, ipradio_font_22, COL_TEXT_DIM, "");
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 30);

    s_field = lv_textarea_create(s_screen);
    lv_obj_set_size(s_field, LV_PCT(84), 76);
    lv_obj_align(s_field, LV_ALIGN_TOP_MID, 0, 76);
    lv_textarea_set_one_line(s_field, true);
    lv_obj_set_style_text_font(s_field, ipradio_font_28, 0);
    lv_obj_set_style_text_color(s_field, COL_TEXT, 0);
    lv_obj_set_style_bg_color(s_field, COL_SURFACE, 0);
    lv_obj_set_style_border_color(s_field, COL_AMBER, 0);
    lv_obj_set_style_border_width(s_field, 2, 0);
    lv_obj_set_style_radius(s_field, 10, 0);

    s_matrix = lv_buttonmatrix_create(s_screen);
    lv_obj_set_size(s_matrix, LV_PCT(96), 420);
    lv_obj_align(s_matrix, LV_ALIGN_BOTTOM_MID, 0, -64);
    lv_obj_set_style_bg_opa(s_matrix, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_matrix, 0, 0);
    lv_obj_set_style_pad_all(s_matrix, 4, 0);

    lv_obj_set_style_text_font(s_matrix, ipradio_font_22, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_matrix, COL_SURFACE, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(s_matrix, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_matrix, COL_TEXT_DIM, LV_PART_ITEMS);
    lv_obj_set_style_border_color(s_matrix, COL_BORDER, LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_matrix, 1, LV_PART_ITEMS);
    lv_obj_set_style_radius(s_matrix, 8, LV_PART_ITEMS);

    /* Выбранная клавиша — заливкой, как выделение везде в приборе. */
    lv_obj_set_style_bg_color(s_matrix, COL_AMBER,
                              LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_text_color(s_matrix, COL_BG,
                                LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_color(s_matrix, COL_AMBER,
                              LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(s_matrix, COL_BG,
                                LV_PART_ITEMS | LV_STATE_PRESSED);

    lv_obj_add_event_cb(s_matrix, on_matrix_click, LV_EVENT_VALUE_CHANGED, NULL);

    s_hints = ipradio_ui_label(s_screen, ipradio_font_14, COL_TEXT_FAINT,
        "Энкодер 1 — выбор   •   нажатие — ввод   •   "
        "двойное — сохранить   •   энкодер 2 — отмена");
    lv_obj_align(s_hints, LV_ALIGN_BOTTOM_MID, 0, -24);

    apply_layout();
    lv_obj_add_flag(s_screen, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "клавиатура готова: ЙЦУКЕН, латиница, знаки");
    return ESP_OK;
}

/* ------------------------------------------------------------------ *
 *  Открытие и закрытие
 * ------------------------------------------------------------------ */

void ipradio_keyboard_open(const char *title, const char *initial,
                           bool password, ipradio_kb_cb_t cb, void *ctx)
{
    if (!s_screen) {
        return;
    }

    s_cb       = cb;
    s_ctx      = ctx;
    s_password = password;
    s_selected = 0;
    s_visible  = true;
    s_last_press_us   = 0;
    s_last_was_insert = false;

    /* Пароль набирается латиницей — с русской раскладки его вводить
     * некуда. Название станции чаще русское. Открываемся с той,
     * которая нужна, чтобы не переключать первым же действием. */
    s_layout = password ? LAYOUT_EN : LAYOUT_RU;
    s_upper  = false;

    lv_label_set_text(s_title, title ? title : "");
    lv_textarea_set_password_mode(s_field, password);
    lv_textarea_set_text(s_field, initial ? initial : "");

    apply_layout();

    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_screen);
}

void ipradio_keyboard_close(void)
{
    if (s_visible) {
        finish(false);
    }
}

bool ipradio_keyboard_visible(void)
{
    return s_visible;
}

/* ------------------------------------------------------------------ *
 *  Органы управления
 * ------------------------------------------------------------------ */

void ipradio_keyboard_move(int delta)
{
    if (!s_visible || delta == 0) {
        return;
    }

    if (s_button_count == 0) {
        return;
    }
    uint32_t count = s_button_count;

    /* По кругу: клавиш много, и упор в край заставлял бы отматывать
     * всю раскладку обратно. */
    int32_t next = (int32_t) s_selected + delta;
    next %= (int32_t) count;
    if (next < 0) {
        next += (int32_t) count;
    }

    s_selected = (uint32_t) next;
    lv_buttonmatrix_set_selected_button(s_matrix, s_selected);
}

void ipradio_keyboard_select(void)
{
    if (!s_visible) {
        return;
    }

    /* Двойное нажатие сохраняет (§5.3.1). Считаем сами, а не ловим
     * событие LVGL: нажатие приходит с физической кнопки энкодера,
     * а не с сенсорного экрана, и LVGL про него не знает. */
    int64_t now = esp_timer_get_time();
    bool    dbl = (s_last_press_us != 0) &&
                  ((now - s_last_press_us) < DOUBLE_PRESS_MS * 1000);
    s_last_press_us = now;

    if (dbl) {
        /* Первое из двух нажатий успело ввести знак — забираем его
         * обратно: человек имел в виду «сохранить», а не «ввести
         * букву и сохранить». */
        if (s_last_was_insert) {
            lv_textarea_delete_char(s_field);
        }
        finish(true);
        return;
    }

    s_last_was_insert = press_key(key_at(s_selected));
}

void ipradio_keyboard_back(void)
{
    ipradio_keyboard_close();
}
