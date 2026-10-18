/*
 * ipradio_input.c — опрос регуляторов и кнопок.
 *
 * Почему опрос, а не прерывания на каждую линию. Кнопок много, и все
 * они медленные: человек не нажимает чаще нескольких раз в секунду.
 * Опрос раз в 5 мс даёт антидребезг бесплатно — состояние считается
 * установившимся, когда оно совпало DEBOUNCE_SAMPLES раз подряд.
 * Регуляторы опрашиваются в том же цикле: EC11 с двадцатью импульсами
 * на оборот при быстром вращении даёт от силы двести переходов
 * в секунду, а мы читаем двести раз в секунду каждый канал.
 *
 * Декодер регулятора — таблица переходов по коду Грея. Она отбрасывает
 * дребезг контактов сама: недопустимый переход просто не даёт шага.
 */

#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "board_pins.h"
#include "ipradio_storage.h"
#include "ipradio_input.h"
#include "ipradio_watchdog.h"
#include "ipradio_state.h"

static const char *TAG = "input";

#define POLL_PERIOD_MS      5
#define DEBOUNCE_SAMPLES    3      /* 3 × 5 мс = 15 мс устойчивости */
#define INPUT_TASK_STACK    4096   /* +1 КБ: в цикле теперь обмен по I²C */
#define INPUT_TASK_PRIO     7      /* выше автомата: ввод не должен ждать */

/* ------------------------------------------------------------------ *
 *  Кнопки
 * ------------------------------------------------------------------ */

typedef enum {
    BTN_IDLE = 0,
    BTN_DOWN,          /* нажата, ждём отпускания илидолгого удержания */
    BTN_WAIT_DOUBLE,   /* отпущена коротко, ждём второго нажатия       */
    BTN_LONG_FIRED,    /* длинное уже отработало, ждём отпускания      */
} btn_phase_t;

/* Что делать при каждом виде нажатия. Событие -1 означает «нет». */
typedef struct {
    int short_ev;
    int double_ev;
    int long_ev;
    int32_t arg;
} btn_actions_t;

typedef struct {
    gpio_num_t     pin;          /* вывод ESP32, если не на расширителе    */
    bool           on_expander;  /* true - линия MCP23017, pin не нужен    */
    uint8_t        exp_line;     /* 0…15: GPAn = n, GPBn = 8 + n           */
    const char    *name;
    btn_actions_t  act;

    /* Состояние опроса */
    bool           stable;        /* установившийся уровень: true = нажата */
    bool           last_raw;
    uint8_t        same_count;
    btn_phase_t    phase;
    int64_t        t_down_us;
    int64_t        t_up_us;
} button_t;

/* Таблица кнопок. Номера выводов — из board_pins.h, назначение —
 * из docs/26-firmware-spec.md, §6.1. */
static button_t s_buttons[] = {
    {
        .pin  = PIN_ENC1_BTN,
        .name = "регулятор 1",
        /* Коротко — ОК и играть, длинно — вход в меню. Разведено
         * специально: в черновике ТЗ обе роли висели на одном нажатии. */
        .act  = { .short_ev  = IPRADIO_EV_SELECT,
                  .double_ev = -1,
                  .long_ev   = IPRADIO_EV_MENU },
    },
    {
        .pin  = PIN_ENC2_BTN,
        .name = "регулятор 2",
        /* Mute вкл/выкл, одинаково в обоих режимах. */
        .act  = { .short_ev  = IPRADIO_EV_MUTE_TOGGLE,
                  .double_ev = -1,
                  .long_ev   = -1 },
    },
    {
        .pin  = PIN_POWER_BUTTON,
        .name = "питание",
        /* Только длинное: короткое нажатие кнопки питания на радио —
         * почти всегда случайность. */
        .act  = { .short_ev  = -1,
                  .double_ev = -1,
                  .long_ev   = IPRADIO_EV_POWER },
    },
#if IPRADIO_USE_IO_EXPANDER
    /* Кнопки на расширителе MCP23017 (линии - board_pins.h). Действия
     * те же, что у вариантов на выводах: коротко - вызвать пресет,
     * длинно - записать в него то, что играет. */
    { .on_expander = true, .exp_line = EXP_LINE_MODE, .name = "MODE",
      .act = { IPRADIO_EV_MODE_TOGGLE, -1, -1, 0 } },
#define EXP_PRESET(n, line) \
    { .on_expander = true, .exp_line = (line), .name = "пресет " #n, \
      .act = { IPRADIO_EV_PRESET_PRESSED, -1, IPRADIO_EV_PRESET_HOLD, (n) } }
    EXP_PRESET(1, EXP_LINE_PRESET_BASE + 0),
    EXP_PRESET(2, EXP_LINE_PRESET_BASE + 1),
    EXP_PRESET(3, EXP_LINE_PRESET_BASE + 2),
    EXP_PRESET(4, EXP_LINE_PRESET_BASE + 3),
    EXP_PRESET(5, EXP_LINE_PRESET_BASE + 4),
    EXP_PRESET(6, EXP_LINE_PRESET_BASE + 5),
    EXP_PRESET(7, EXP_LINE_PRESET_BASE + 6),
    EXP_PRESET(8, EXP_LINE_PRESET_8),
#undef EXP_PRESET
#else
    {
        .pin  = PIN_MODE_BUTTON,
        .name = "MODE",
        .act  = { .short_ev  = IPRADIO_EV_MODE_TOGGLE,
                  .double_ev = -1,
                  .long_ev   = -1 },
    },
    /* Кнопки пресетов: коротко — вызвать, длинно — записать текущую
     * станцию. Скрытых вторых функций быть не должно (R4.1), поэтому
     * двойное нажатие здесь намеренно не используется. */
    { .pin = PIN_PRESET_1, .name = "пресет 1",
      .act = { IPRADIO_EV_PRESET_PRESSED, -1, IPRADIO_EV_PRESET_HOLD, 1 } },
    { .pin = PIN_PRESET_2, .name = "пресет 2",
      .act = { IPRADIO_EV_PRESET_PRESSED, -1, IPRADIO_EV_PRESET_HOLD, 2 } },
    { .pin = PIN_PRESET_3, .name = "пресет 3",
      .act = { IPRADIO_EV_PRESET_PRESSED, -1, IPRADIO_EV_PRESET_HOLD, 3 } },
    { .pin = PIN_PRESET_4, .name = "пресет 4",
      .act = { IPRADIO_EV_PRESET_PRESSED, -1, IPRADIO_EV_PRESET_HOLD, 4 } },
    { .pin = PIN_PRESET_5, .name = "пресет 5",
      .act = { IPRADIO_EV_PRESET_PRESSED, -1, IPRADIO_EV_PRESET_HOLD, 5 } },
#endif
};

#define BUTTON_COUNT (sizeof(s_buttons) / sizeof(s_buttons[0]))

/* Кнопка не может звать ячейку, которой нет в банке. Проверка
 * статическая: оба числа - константы времени сборки, и ошибка
 * должна ловиться компилятором, а не на плате. */
_Static_assert(IPRADIO_PRESET_BUTTONS <= IPRADIO_PRESET_MAX,
               "кнопок пресетов больше, чем ячеек в хранилище");

/* ------------------------------------------------------------------ *
 *  Регуляторы
 * ------------------------------------------------------------------ */

typedef struct {
    gpio_num_t pin_a;
    gpio_num_t pin_b;
    const char *name;
    int event;              /* какое событие слать на каждый шаг */

    uint8_t    prev;        /* прошлое состояние пары AB */
    int8_t     accum;       /* накопитель четвертьшагов  */
} encoder_t;

static encoder_t s_encoders[] = {
    { .pin_a = PIN_ENC1_A, .pin_b = PIN_ENC1_B,
      .name = "регулятор 1", .event = IPRADIO_EV_TUNE_DELTA },
    { .pin_a = PIN_ENC2_A, .pin_b = PIN_ENC2_B,
      .name = "регулятор 2", .event = IPRADIO_EV_VOLUME_DELTA },
};

#define ENCODER_COUNT (sizeof(s_encoders) / sizeof(s_encoders[0]))

/* Таблица переходов кода Грея: индекс — (прошлое << 2) | текущее.
 * Ноль означает недопустимый переход, то есть дребезг: шага не будет. */
static const int8_t s_gray_table[16] = {
     0, -1, +1,  0,
    +1,  0,  0, -1,
    -1,  0,  0, +1,
     0, +1, -1,  0,
};

/* У EC11 один щелчок даёт четыре четвертьшага. Копим и выдаём шаг
 * только на полный щелчок, иначе громкость менялась бы вчетверо
 * быстрее ожидаемого. */
#define QUARTERS_PER_DETENT  4

/* ------------------------------------------------------------------ *
 *  Общее
 * ------------------------------------------------------------------ */

/* Метка последней активности - в МИЛЛИСЕКУНДАХ и 32 разряда.
 *
 * Раньше хранились микросекунды в int64. На 32-разрядном кристалле
 * это два отдельных чтения, и читатель из другой задачи мог застать
 * половину старого значения и половину нового - отсюда редкий ложный
 * уход в ждущий режим или пропуск его.
 *
 * Тридцати двух разрядов миллисекунд хватает на 49 суток; разность
 * беззнаковых считается верно и через переполнение. */
static volatile uint32_t s_last_activity_ms;

static void mark_activity(void)
{
    s_last_activity_ms = (uint32_t) (esp_timer_get_time() / 1000);
}

void ipradio_input_note_activity(void)
{
    mark_activity();
}

uint32_t ipradio_input_idle_ms(void)
{
    /* Возвращаем МИЛЛИСЕКУНДЫ, как и обещано именем.
     *
     * Здесь стояло лишнее деление на тысячу, и функция отдавала
     * секунды. Единственный потребитель - ipradio_ui.c - сравнивает
     * результат с IPRADIO_IDLE_TIMEOUT_MS, то есть со 180 000.
     * Получалось «180 000 секунд», пятьдесят часов: ждущий экран
     * не включался никогда. */
    return (uint32_t) (esp_timer_get_time() / 1000) - s_last_activity_ms;
}

static void fire(int event, int32_t arg)
{
    if (event < 0) {
        return;
    }
    mark_activity();
#if IPRADIO_DEBUG_INPUT
    ESP_LOGI(TAG, "событие %d, аргумент %d", event, (int) arg);
#endif
    ipradio_post_simple((ipradio_event_type_t) event, arg);
}

/* ------------------------------------------------------------------ *
 *  Расширитель MCP23017
 * ------------------------------------------------------------------ *
 *
 * Опрос, а не прерывание: порты читаются одним запросом в том же
 * цикле, что и остальные кнопки, раз в POLL_PERIOD_MS. Линии INTA/INTB
 * не нужны вовсе. Два байта на 100 кГц - около 0,4 мс шины раз в 5 мс.
 *
 * Регистры - даташит Microchip DS20001952D, таблица адресов при
 * IOCON.BANK = 0 (значение после сброса): IODIRA 00h, IODIRB 01h,
 * GPPUA 0Ch, GPPUB 0Dh, GPIOA 12h, GPIOB 13h, OLATA 14h, OLATB 15h.
 * IOCON.SEQOP после сброса 0 - адрес растёт сам, поэтому пары A/B
 * пишутся и читаются одним обменом.
 *
 * GPA7 и GPB7 у MCP23017 - только выходы (там же, с. 1). Делаем их
 * выходами с нулём: если на них по ошибке повесят кнопку на землю,
 * нажатие ничего не замкнёт.
 */

#define MCP_IODIRA   0x00
#define MCP_GPPUA    0x0C
#define MCP_GPIOA    0x12
#define MCP_OLATA    0x14
#define MCP_INPUTS   0x7F        /* линии 0…6 порта - входы, 7 - выход */

static i2c_master_dev_handle_t s_exp;      /* NULL - расширителя нет      */
static uint16_t s_exp_in = 0xFFFF;         /* все единицы = всё отпущено  */
static uint32_t s_exp_errors;
static uint32_t s_exp_resets;              /* сколько раз настройка слетала */
static uint32_t s_exp_glitches;            /* отбракованных чтений         */

/* Линии с кнопками: 0…6 обоих портов. Биты 7 - выходы с нулём (см.
 * выше), они читаются нулём всегда и в счёт нажатий не идут. */
#define EXP_BUTTON_MASK      0x7F7F
/* Больше двух кнопок разом человек не жмёт. Такое чтение - помеха
 * или расширитель со слетевшими подтяжками: входы висят в воздухе
 * и ловят наводки с проводов кнопок. 2026-10-19 так, судя по всему,
 * пресеты сами перескакивали, а удержания записали одну частоту во
 * все эфирные ячейки. */
#define EXP_MAX_PRESSED      2
/* Раз в столько циклов опроса (5 мс) - проверка, что настройка на месте. */
#define EXP_CHECK_CYCLES     200           /* ≈ 1 с */

static esp_err_t exp_write_pair(uint8_t reg, uint8_t a, uint8_t b)
{
    uint8_t buf[3] = { reg, a, b };
    return i2c_master_transmit(s_exp, buf, sizeof(buf), 50);
}

/* Настройка портов. Сначала защёлки выходов в ноль, потом подтяжки,
 * потом направление: GPA7/GPB7 становятся выходами уже с нулём. */
static esp_err_t exp_configure(void)
{
    esp_err_t err = exp_write_pair(MCP_OLATA, 0x00, 0x00);
    if (err == ESP_OK) {
        err = exp_write_pair(MCP_GPPUA, MCP_INPUTS, MCP_INPUTS);
    }
    if (err == ESP_OK) {
        err = exp_write_pair(MCP_IODIRA, MCP_INPUTS, MCP_INPUTS);
    }
    return err;
}

/* Настройка на месте? После сброса MCP23017 (провал питания, помеха)
 * регистры возвращаются к заводским: IODIR = FFh, GPPU = 00h - то есть
 * подтяжки выключены. Это и проверяем. */
static bool exp_config_ok(void)
{
    uint8_t reg = MCP_IODIRA, dir[2] = { 0 }, pu[2] = { 0 };
    if (i2c_master_transmit_receive(s_exp, &reg, 1, dir, 2, 20) != ESP_OK) {
        return true;                /* не прочли - судить не о чем */
    }
    reg = MCP_GPPUA;
    if (i2c_master_transmit_receive(s_exp, &reg, 1, pu, 2, 20) != ESP_OK) {
        return true;
    }
    return dir[0] == MCP_INPUTS && dir[1] == MCP_INPUTS &&
           pu[0] == MCP_INPUTS && pu[1] == MCP_INPUTS;
}

static esp_err_t expander_init(i2c_master_bus_handle_t bus)
{
#if IPRADIO_USE_IO_EXPANDER
    if (!bus) {
        ESP_LOGW(TAG, "шины I2C нет - кнопки на расширителе не работают");
        return ESP_ERR_INVALID_ARG;
    }
    if (i2c_master_probe(bus, I2C_ADDR_EXPANDER, 50) != ESP_OK) {
        ESP_LOGW(TAG, "MCP23017 не отвечает на 0x%02X - кнопки пресетов "
                      "и MODE не работают, остаётся экран", I2C_ADDR_EXPANDER);
        return ESP_ERR_NOT_FOUND;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = I2C_ADDR_EXPANDER,
        .scl_speed_hz    = 100000,     /* как у тюнера на той же шине */
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_exp);
    if (err != ESP_OK) {
        s_exp = NULL;
        return err;
    }

    err = exp_configure();

    uint8_t reg = MCP_IODIRA, dir[2] = { 0 };
    if (err == ESP_OK) {
        err = i2c_master_transmit_receive(s_exp, &reg, 1, dir, 2, 50);
    }
    if (err != ESP_OK || dir[0] != MCP_INPUTS || dir[1] != MCP_INPUTS) {
        ESP_LOGE(TAG, "MCP23017 не настроился (%s, IODIR %02X %02X)",
                 esp_err_to_name(err), dir[0], dir[1]);
        i2c_master_bus_rm_device(s_exp);
        s_exp = NULL;
        return (err != ESP_OK) ? err : ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "MCP23017 на 0x%02X: GPA0-6 и GPB0-6 - входы с подтяжкой",
             I2C_ADDR_EXPANDER);
    return ESP_OK;
#else
    (void) bus;
    return ESP_OK;
#endif
}

/* Один обмен - оба порта. Ошибка шины: кнопки расширителя считаем
 * отпущенными (ложное нажатие хуже пропущенного) и пишем в журнал
 * первую ошибку и дальше каждую тысячную, чтобы не утопить журнал. */
static void expander_read(void)
{
    if (!s_exp) {
        return;
    }
    static uint32_t cycle;
    if (++cycle % EXP_CHECK_CYCLES == 0 && !exp_config_ok()) {
        s_exp_resets++;
        esp_err_t e = exp_configure();
        ESP_LOGW(TAG, "MCP23017 потерял настройку (сброс №%" PRIu32 "), "
                      "настроен заново: %s", s_exp_resets, esp_err_to_name(e));
        s_exp_in = 0xFFFF;
        return;
    }

    uint8_t reg = MCP_GPIOA, v[2];
    esp_err_t err = i2c_master_transmit_receive(s_exp, &reg, 1, v, 2, 20);
    if (err == ESP_OK) {
        uint16_t in = (uint16_t) (v[0] | (v[1] << 8));
        int pressed = __builtin_popcount((~in) & EXP_BUTTON_MASK);
        if (pressed > EXP_MAX_PRESSED) {
            /* Отбраковка: считаем всё отпущенным. В журнал - первое
             * и дальше каждое сотое, со значением портов. */
            if ((s_exp_glitches++ % 100) == 0) {
                ESP_LOGW(TAG, "MCP23017: нажато %d кнопок разом (A=%02X B=%02X) - "
                              "помеха, отброшено (всего %" PRIu32 ")",
                         pressed, v[0], v[1], s_exp_glitches);
            }
            s_exp_in = 0xFFFF;
            return;
        }
        s_exp_in = in;
        return;
    }
    s_exp_in = 0xFFFF;
    if ((s_exp_errors++ % 1000) == 0) {
        ESP_LOGW(TAG, "чтение MCP23017: %s (ошибок %" PRIu32 ")",
                 esp_err_to_name(err), s_exp_errors);
    }
}

/* ------------------------------------------------------------------ *
 *  Опрос
 * ------------------------------------------------------------------ */

static void poll_button(button_t *b, int64_t now_us)
{
    /* Кнопки замыкают вывод на землю, подтяжка внутренняя:
     * низкий уровень означает «нажата». На расширителе - так же. */
    bool raw = b->on_expander
             ? (((s_exp_in >> b->exp_line) & 1u) == 0)
             : (gpio_get_level(b->pin) == BUTTON_ACTIVE_LEVEL);

    if (raw == b->last_raw) {
        if (b->same_count < DEBOUNCE_SAMPLES) {
            b->same_count++;
        }
    } else {
        b->last_raw   = raw;
        b->same_count = 1;
    }

    bool settled = (b->same_count >= DEBOUNCE_SAMPLES);
    bool changed = settled && (raw != b->stable);
    if (changed) {
        b->stable = raw;
    }

    switch (b->phase) {

    case BTN_IDLE:
        if (changed && b->stable) {
            b->phase     = BTN_DOWN;
            b->t_down_us = now_us;
        }
        break;

    case BTN_DOWN:
        if (changed && !b->stable) {
            /* Отпустили до порога длинного нажатия. */
            b->t_up_us = now_us;
            if (b->act.double_ev >= 0) {
                b->phase = BTN_WAIT_DOUBLE;   /* ждём второго касания */
            } else {
                fire(b->act.short_ev, b->act.arg);
                b->phase = BTN_IDLE;
            }
        } else if (b->stable &&
                   (now_us - b->t_down_us) >= BTN_LONG_MIN_MS * 1000) {
            /* Длинное срабатывает СРАЗУ при достижении порога, не дожидаясь
             * отпускания: человек должен почувствовать, что удержание
             * засчитано, пока держит. */
            fire(b->act.long_ev, b->act.arg);
            b->phase = BTN_LONG_FIRED;
        }
        break;

    case BTN_WAIT_DOUBLE:
        if (changed && b->stable) {
            fire(b->act.double_ev, b->act.arg);
            b->phase = BTN_LONG_FIRED;  /* дожидаемся отпускания молча */
        } else if ((now_us - b->t_up_us) >= BTN_DOUBLE_GAP_MAX_MS * 1000) {
            /* Второго нажатия не последовало — значит было короткое. */
            fire(b->act.short_ev, b->act.arg);
            b->phase = BTN_IDLE;
        }
        break;

    case BTN_LONG_FIRED:
        if (changed && !b->stable) {
            b->phase = BTN_IDLE;
        }
        break;
    }
}

static void poll_encoder(encoder_t *e)
{
    uint8_t cur = (uint8_t) ((gpio_get_level(e->pin_a) << 1) |
                              gpio_get_level(e->pin_b));
    if (cur == e->prev) {
        return;
    }

    int8_t step = s_gray_table[(e->prev << 2) | cur];
    e->prev = cur;
    if (step == 0) {
        return;                     /* дребезг, шага нет */
    }

    e->accum += step;

    if (e->accum >= QUARTERS_PER_DETENT) {
        e->accum -= QUARTERS_PER_DETENT;
        fire(e->event, +1);
    } else if (e->accum <= -QUARTERS_PER_DETENT) {
        e->accum += QUARTERS_PER_DETENT;
        fire(e->event, -1);
    }
}

static void input_task(void *arg)
{
    (void) arg;
    TickType_t last_wake = xTaskGetTickCount();

    ESP_LOGI(TAG, "опрос запущен: %d кнопок, %d регулятора, период %d мс",
             (int) BUTTON_COUNT, (int) ENCODER_COUNT, POLL_PERIOD_MS);

    int wdt = ipradio_watchdog_register("input", 1000, 0);

    for (;;) {
        ipradio_watchdog_feed(wdt);
        int64_t now = esp_timer_get_time();

        expander_read();
#if IPRADIO_DEBUG_INPUT
        static uint16_t s_exp_prev = 0xFFFF;
        if (s_exp_in != s_exp_prev) {
            ESP_LOGI(TAG, "MCP23017: A=%02X B=%02X (0 - нажата)",
                     s_exp_in & 0xFF, s_exp_in >> 8);
            s_exp_prev = s_exp_in;
        }
#endif
        for (size_t i = 0; i < BUTTON_COUNT; i++) {
            poll_button(&s_buttons[i], now);
        }
        for (size_t i = 0; i < ENCODER_COUNT; i++) {
            poll_encoder(&s_encoders[i]);
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(POLL_PERIOD_MS));
    }
}

/* ------------------------------------------------------------------ *
 *  Инициализация
 * ------------------------------------------------------------------ */

static esp_err_t configure_input_pin(gpio_num_t pin)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

esp_err_t ipradio_input_init(i2c_master_bus_handle_t bus)
{
    /* Расширитель не обязателен: без него работают экран, регуляторы
     * и кнопки на выводах ESP32. Поэтому ошибка - только в журнал. */
    (void) expander_init(bus);

    for (size_t i = 0; i < BUTTON_COUNT; i++) {
        if (!s_buttons[i].on_expander) {
            ESP_ERROR_CHECK(configure_input_pin(s_buttons[i].pin));
        }
        s_buttons[i].stable   = false;
        s_buttons[i].last_raw = false;
        s_buttons[i].phase    = BTN_IDLE;
    }

    for (size_t i = 0; i < ENCODER_COUNT; i++) {
        ESP_ERROR_CHECK(configure_input_pin(s_encoders[i].pin_a));
        ESP_ERROR_CHECK(configure_input_pin(s_encoders[i].pin_b));
        s_encoders[i].prev =
            (uint8_t) ((gpio_get_level(s_encoders[i].pin_a) << 1) |
                        gpio_get_level(s_encoders[i].pin_b));
        s_encoders[i].accum = 0;
    }

    mark_activity();

    BaseType_t ok = xTaskCreate(input_task, "ipradio_input",
                                INPUT_TASK_STACK, NULL,
                                INPUT_TASK_PRIO, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}
