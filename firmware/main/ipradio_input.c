/*
 * ipradio_input.c — опрос энкодеров и кнопок.
 *
 * Почему опрос, а не прерывания на каждую линию. Кнопок много, и все
 * они медленные: человек не нажимает чаще нескольких раз в секунду.
 * Опрос раз в 5 мс даёт антидребезг бесплатно — состояние считается
 * установившимся, когда оно совпало DEBOUNCE_SAMPLES раз подряд.
 * Энкодеры опрашиваются в том же цикле: EC11 с двадцатью импульсами
 * на оборот при быстром вращении даёт от силы двести переходов
 * в секунду, а мы читаем двести раз в секунду каждый канал.
 *
 * Декодер энкодера — таблица переходов по коду Грея. Она отбрасывает
 * дребезг контактов сама: недопустимый переход просто не даёт шага.
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "board_pins.h"
#include "ipradio_input.h"
#include "ipradio_watchdog.h"
#include "ipradio_state.h"

static const char *TAG = "input";

#define POLL_PERIOD_MS      5
#define DEBOUNCE_SAMPLES    3      /* 3 × 5 мс = 15 мс устойчивости */
#define INPUT_TASK_STACK    3072
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
    gpio_num_t     pin;
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
        .name = "энкодер 1",
        /* Коротко — ОК и играть, длинно — вход в меню. Разведено
         * специально: в черновике ТЗ обе роли висели на одном нажатии. */
        .act  = { .short_ev  = IPRADIO_EV_SELECT,
                  .double_ev = -1,
                  .long_ev   = IPRADIO_EV_MENU },
    },
    {
        .pin  = PIN_ENC2_BTN,
        .name = "энкодер 2",
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
#if !IPRADIO_USE_IO_EXPANDER
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

/* ------------------------------------------------------------------ *
 *  Энкодеры
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
      .name = "энкодер 1", .event = IPRADIO_EV_TUNE_DELTA },
    { .pin_a = PIN_ENC2_A, .pin_b = PIN_ENC2_B,
      .name = "энкодер 2", .event = IPRADIO_EV_VOLUME_DELTA },
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

static volatile int64_t s_last_activity_us;

static void mark_activity(void)
{
    s_last_activity_us = esp_timer_get_time();
}

uint32_t ipradio_input_idle_ms(void)
{
    int64_t dt = esp_timer_get_time() - s_last_activity_us;
    return (uint32_t) (dt / 1000);
}

static void fire(int event, int32_t arg)
{
    if (event < 0) {
        return;
    }
    mark_activity();
    ipradio_post_simple((ipradio_event_type_t) event, arg);
}

/* ------------------------------------------------------------------ *
 *  Опрос
 * ------------------------------------------------------------------ */

static void poll_button(button_t *b, int64_t now_us)
{
    /* Кнопки замыкают вывод на землю, подтяжка внутренняя:
     * низкий уровень означает «нажата». */
    bool raw = (gpio_get_level(b->pin) == BUTTON_ACTIVE_LEVEL);

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

    ESP_LOGI(TAG, "опрос запущен: %d кнопок, %d энкодера, период %d мс",
             (int) BUTTON_COUNT, (int) ENCODER_COUNT, POLL_PERIOD_MS);

    int wdt = ipradio_watchdog_register("input", 1000, 0);

    for (;;) {
        ipradio_watchdog_feed(wdt);
        int64_t now = esp_timer_get_time();

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

esp_err_t ipradio_input_init(void)
{
    for (size_t i = 0; i < BUTTON_COUNT; i++) {
        ESP_ERROR_CHECK(configure_input_pin(s_buttons[i].pin));
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
