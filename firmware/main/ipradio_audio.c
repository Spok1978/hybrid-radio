/*
 * ipradio_audio.c — I²S на PCM5102A, громкость и приглушение.
 *
 * Разделение обязанностей, которое стоит держать в голове:
 *
 *   автомат   — хранит ОДНО логическое значение громкости 0…100
 *               и флаг mute, ничего не знает про железо;
 *   этот файл — переводит их в действия: для эфира это регистр
 *               тюнера, для интернета — масштабирование отсчётов;
 *   железо    — пассивный сумматор на резисторах, про который
 *               прошивка не знает вовсе.
 *
 * Из-за пассивного сумматора неактивный источник обязан быть заглушён
 * ПО-НАСТОЯЩЕМУ, иначе его шум слышен всегда (docs/04-audio-path.md).
 */

#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"

#include "board_pins.h"
#include "ipradio_audio.h"
#include "ipradio_tuner.h"

static const char *TAG = "audio";

#define DEFAULT_RATE_HZ   44100
#define DMA_FRAME_NUM     512
#define DMA_DESC_NUM      6

static i2s_chan_handle_t s_tx;
static uint32_t          s_rate = DEFAULT_RATE_HZ;

/* Множитель громкости в формате Q15: 0 — тишина, 32768 — единица.
 * Читается из задачи потока на каждый блок, пишется из задачи
 * автомата, поэтому volatile. Атомарности хватает: значение
 * помещается в машинное слово, и промежуточных состояний нет. */
static volatile int32_t  s_gain_q15 = 0;

/* ------------------------------------------------------------------ *
 *  Громкость
 * ------------------------------------------------------------------ */

/* Логическая шкала 0…100 в множитель.
 *
 * Шкала логарифмическая: линейный множитель на слух ведёт себя
 * неправильно — половина хода ручки даёт едва заметное падение.
 * Берём приближение к −40 дБ на нуле шкалы, что примерно
 * соответствует шестнадцати ступеням тюнера и делает ощущение
 * от ручки одинаковым в обоих режимах (docs/23-volume-control.md).
 *
 * Таблица вместо вычисления: логарифм на каждый блок отсчётов
 * считать незачем, а сто значений занимают двести байт.
 */
static const uint16_t s_gain_table[21] = {
    /* шаг 5 единиц шкалы, от 0 до 100 */
        0,   103,   146,   206,   291,   412,   582,
      823,  1163,  1644,  2325,  3286,  4645,  6567,
     9283, 13123, 18552, 26227, 32768, 32768, 32768,
};

static int32_t logical_to_gain(uint8_t logical)
{
    if (logical == 0) {
        return 0;
    }
    if (logical > 100) {
        logical = 100;
    }

    /* Линейная интерполяция между узлами таблицы: ручка крутится
     * по одной единице, а таблица идёт через пять. */
    uint8_t idx  = logical / 5;
    uint8_t frac = logical % 5;

    int32_t a = s_gain_table[idx];
    int32_t b = s_gain_table[idx + 1];
    return a + ((b - a) * frac) / 5;
}

void ipradio_audio_scale(int16_t *samples, size_t count)
{
    int32_t g = s_gain_q15;

    if (g >= 32768) {
        return;                 /* полная громкость — не трогаем */
    }
    if (g == 0) {
        memset(samples, 0, count * sizeof(int16_t));
        return;
    }

    for (size_t i = 0; i < count; i++) {
        samples[i] = (int16_t) ((samples[i] * g) >> 15);
    }
}

/* ------------------------------------------------------------------ *
 *  Применение состояния
 * ------------------------------------------------------------------ */

esp_err_t ipradio_audio_apply(const ipradio_snapshot_t *snap)
{
    if (!snap) {
        return ESP_ERR_INVALID_ARG;
    }

    bool fm  = (snap->mode == IPRADIO_MODE_FM);
    bool net = !fm;

    /* Эфир. Приглушение — отдельным битом, а не громкостью в ноль.
     * Когда эфир неактивен, дополнительно уводим выход в Hi-Z:
     * так тюнер почти отключается от сумматора и не шумит в него. */
    if (ipradio_tuner_present()) {
        ipradio_tuner_set_mute(!fm || snap->muted);
        ipradio_tuner_set_hiz(!fm);
        if (fm) {
            ipradio_tuner_set_volume(snap->volume);
        }
    }

    /* Интернет. Здесь глушить нечего физически: если поток не играет,
     * на ЦАП просто ничего не идёт. Достаточно обнулить множитель. */
    if (net && !snap->muted) {
        s_gain_q15 = logical_to_gain(snap->volume);
    } else {
        s_gain_q15 = 0;
    }

    /* Усилитель выключаем, только когда молчат оба источника: щелчок
     * при каждом переключении режима был бы слышнее, чем польза. */
    bool anything_audible = !snap->muted &&
                            (fm ? ipradio_tuner_present()
                                : snap->play == IPRADIO_PLAY_PLAYING);
    ipradio_audio_amp_enable(anything_audible);

    return ESP_OK;
}

/* ------------------------------------------------------------------ *
 *  Усилитель
 * ------------------------------------------------------------------ */

esp_err_t ipradio_audio_amp_enable(bool on)
{
    /* SD у TPA3118: низкий уровень переводит выходы в Hi-Z. */
    return gpio_set_level(PIN_AMP_SD, on ? 1 : 0);
}

/* ------------------------------------------------------------------ *
 *  I²S
 * ------------------------------------------------------------------ */

static i2s_std_config_t make_std_config(uint32_t rate)
{
    i2s_std_config_t cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            /* MCLK не нужен: PCM5102A получает тактирование из BCK
             * внутренней ФАПЧ, если SCK посажен на землю. Это экономит
             * линию, а их у нас впритык. */
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_I2S_BCK,
            .ws   = PIN_I2S_LRCK,
            .dout = PIN_I2S_DIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    return cfg;
}

esp_err_t ipradio_audio_set_rate(uint32_t hz)
{
    if (!s_tx || hz == s_rate) {
        return ESP_OK;
    }

    /* Канал надо остановить: менять тактирование на ходу нельзя. */
    ESP_ERROR_CHECK(i2s_channel_disable(s_tx));

    i2s_std_config_t cfg = make_std_config(hz);
    esp_err_t err = i2s_channel_reconfig_std_clock(s_tx, &cfg.clk_cfg);

    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));

    if (err == ESP_OK) {
        s_rate = hz;
        ESP_LOGI(TAG, "частота дискретизации: %" PRIu32 " Гц", hz);
    }
    return err;
}

esp_err_t ipradio_audio_write(const void *samples, size_t bytes,
                              size_t *written)
{
    if (!s_tx) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2s_channel_write(s_tx, samples, bytes, written, portMAX_DELAY);
}

esp_err_t ipradio_audio_init(void)
{
    /* Линия приглушения усилителя. Настраиваем и сразу выключаем:
     * усилитель не должен щёлкнуть при подаче питания. */
    gpio_config_t amp = {
        .pin_bit_mask = 1ULL << PIN_AMP_SD,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&amp));
    ipradio_audio_amp_enable(false);

    /* Штатный усилитель платы держим выключенным: звук идёт мимо него,
     * через наш TPA3118. Иначе получилось бы двойное усиление. */
    gpio_config_t board_pa = {
        .pin_bit_mask = 1ULL << PIN_BOARD_PA_CTRL,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&board_pa));
    gpio_set_level(PIN_BOARD_PA_CTRL, 0);

    /* Контроллер I²S. Нулевой занят кодеком платы, поэтому берём
     * любой свободный: у ESP32-P4 их три. */
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = DMA_DESC_NUM;
    chan_cfg.dma_frame_num = DMA_FRAME_NUM;
    chan_cfg.auto_clear    = true;   /* тишина вместо мусора при простое */

    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx, NULL));

    i2s_std_config_t std = make_std_config(s_rate);
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));

    ESP_LOGI(TAG, "I²S поднят: BCK=GPIO%d, LRCK=GPIO%d, DIN=GPIO%d, %" PRIu32 " Гц",
             PIN_I2S_BCK, PIN_I2S_LRCK, PIN_I2S_DIN, s_rate);
    ESP_LOGI(TAG, "  MCLK не используется: у PCM5102A SCK на земле");

    return ESP_OK;
}
