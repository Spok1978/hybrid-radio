/*
 * ipradio_netradio.c — конвейер интернет-радио на ESP-ADF.
 *
 * Схема конвейера:
 *
 *     http_stream  →  automatic decoder  →  i2s_stream  →  PCM5102A
 *
 * Три решения, которые стоит держать в голове.
 *
 * 1. Декодер выбирается ПО ТИПУ ПОТОКА, а не задаётся заранее.
 *    Станции приходят в mp3, aac, ogg — фиксированный декодер
 *    означал бы, что часть станций молчит без объяснения.
 *
 * 2. Конвейер поднимается один раз и не разбирается при остановке.
 *    Требование §5.2: задача плеера не выгружается при уходе в эфир,
 *    иначе возврат в интернет стоил бы лишних секунд.
 *
 * 3. Обрыв сети не считается ошибкой станции. Сначала пробуем
 *    переподключиться молча, и только исчерпав попытки, показываем
 *    «станция не отвечает» — иначе человек пойдёт чинить исправный
 *    Wi-Fi (docs/26-firmware-spec.md, §5.2).
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "http_stream.h"
#include "i2s_stream.h"
#include "esp_decoder.h"
#include "ringbuf.h"

#include "board_pins.h"
#include "ipradio_netradio.h"
#include "ipradio_state.h"

static const char *TAG = "netradio";

#define RETRY_MAX          3
#define RETRY_DELAY_MS  2000

static audio_pipeline_handle_t s_pipeline;
static audio_element_handle_t  s_http;
static audio_element_handle_t  s_decoder;
static audio_element_handle_t  s_i2s;
static audio_event_iface_handle_t s_events;

static bool  s_active;
static char  s_url[256];
static int   s_retries;

/* ------------------------------------------------------------------ *
 *  Обработчик событий потока
 * ------------------------------------------------------------------ */

/* ADF сообщает о смене формата и об ошибках через шину событий.
 * Отдельная задача слушает её и переводит в события автомата —
 * так остальная прошивка ничего не знает про устройство ADF. */
/* Насколько наполнен буфер потока, в процентах.
 *
 * Смотрим кольцо НА ВЫХОДЕ HTTP-элемента: именно оно скрывает дрожание
 * сети, и именно его опустошение слышно как заикание. Кольцо после
 * декодера меряет другое - успевает ли декодер, а он всегда успевает.
 *
 * Показатель нужен не сам по себе. Когда поток заикается, человек
 * должен видеть разницу между «сеть не тянет» (буфер пуст) и «что-то
 * не так с прибором» (буфер полон, а звука нет). */
static void report_buffer_fill(void)
{
    static uint8_t last = 255;

    if (!s_http || !s_active) {
        return;
    }

    ringbuf_handle_t rb = audio_element_get_output_ringbuf(s_http);
    if (!rb) {
        return;
    }

    int size = rb_get_size(rb);
    if (size <= 0) {
        return;
    }

    int filled = rb_bytes_filled(rb);
    if (filled < 0) {
        filled = 0;
    }

    uint8_t pct = (uint8_t) ((int64_t) filled * 100 / size);

    /* Событие шлём только на заметное изменение: иначе автомат
     * будил бы всех подписчиков по два раза в секунду ради цифры,
     * которая колеблется на единицу. */
    if (last != 255 && pct > last - 5 && pct < last + 5) {
        return;
    }
    last = pct;

    ipradio_post_simple(IPRADIO_EV_BUFFER_FILL, pct);
}

static void events_task(void *arg)
{
    (void) arg;
    audio_event_iface_msg_t msg;

    for (;;) {
        /* Ждём событие не бесконечно: заполнение буфера надо
         * показывать и тогда, когда ничего не происходит, - а именно
         * тогда человек на него и смотрит. */
        if (audio_event_iface_listen(s_events, &msg,
                                     pdMS_TO_TICKS(500)) != ESP_OK) {
            report_buffer_fill();
            continue;
        }

        report_buffer_fill();

        /* Декодер сообщил параметры потока — подстраиваем вывод.
         * Частота дискретизации у станций разная, и менять её надо
         * при смене станции, а не при старте. */
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT &&
            msg.source == (void *) s_decoder &&
            msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {

            audio_element_info_t info = { 0 };
            audio_element_getinfo(s_decoder, &info);

            ESP_LOGI(TAG, "поток: %d Гц, %d бит, %d канал(ов)",
                     info.sample_rates, info.bits, info.channels);

            i2s_stream_set_clk(s_i2s, info.sample_rates,
                               info.bits, info.channels);

            ipradio_post_simple(IPRADIO_EV_PLAY_STATE, IPRADIO_PLAY_PLAYING);
            s_retries = 0;      /* заиграло — счётчик попыток сбрасываем */
            continue;
        }

        /* Ошибка или конец потока. У радиостанции «конец» означает
         * обрыв: поток бесконечный по своей природе. */
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT &&
            msg.cmd == AEL_MSG_CMD_REPORT_STATUS) {

            audio_element_status_t st = (audio_element_status_t) (intptr_t) msg.data;
            if (st != AEL_STATUS_STATE_FINISHED &&
                st != AEL_STATUS_ERROR_OPEN &&
                st != AEL_STATUS_ERROR_PROCESS) {
                continue;
            }

            if (!s_active) {
                continue;       /* мы сами остановили — это не ошибка */
            }

            if (s_retries < RETRY_MAX) {
                s_retries++;
                ESP_LOGW(TAG, "поток прервался, попытка %d из %d",
                         s_retries, RETRY_MAX);
                ipradio_post_simple(IPRADIO_EV_PLAY_STATE,
                                    IPRADIO_PLAY_BUFFERING);

                vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));

                audio_pipeline_stop(s_pipeline);
                audio_pipeline_wait_for_stop(s_pipeline);
                audio_pipeline_reset_ringbuffer(s_pipeline);
                audio_pipeline_reset_elements(s_pipeline);
                audio_element_set_uri(s_http, s_url);
                audio_pipeline_run(s_pipeline);
            } else {
                /* Попытки исчерпаны. Только теперь это «станция
                 * не отвечает», а не «нет сети»: Wi-Fi при этом
                 * может быть совершенно исправен. */
                ESP_LOGE(TAG, "станция не отвечает после %d попыток",
                         RETRY_MAX);
                s_active = false;
                ipradio_post_simple(IPRADIO_EV_PLAY_STATE,
                                    IPRADIO_PLAY_ERROR);
            }
        }
    }
}

/* ------------------------------------------------------------------ *
 *  Сборка конвейера
 * ------------------------------------------------------------------ */

esp_err_t ipradio_netradio_init(void)
{
    audio_pipeline_cfg_t pipe_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    s_pipeline = audio_pipeline_init(&pipe_cfg);
    if (!s_pipeline) {
        return ESP_ERR_NO_MEM;
    }

    /* Источник: HTTP. Буфер побольше — интернет-радио живёт на нём,
     * и он же скрывает дрожание сети. Память под него берётся
     * из PSRAM, её у платы 32 МБ. */
    http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_cfg.type = AUDIO_STREAM_READER;
    http_cfg.out_rb_size = 64 * 1024;
    s_http = http_stream_init(&http_cfg);

    /* Декодер: набор кодеков, выбор по типу потока.
     * Порядок в списке — порядок проверки. */
    audio_decoder_t decoders[] = {
        DEFAULT_ESP_MP3_DECODER_CONFIG(),
        DEFAULT_ESP_AAC_DECODER_CONFIG(),
        DEFAULT_ESP_OGG_DECODER_CONFIG(),
        DEFAULT_ESP_WAV_DECODER_CONFIG(),
    };
    esp_decoder_cfg_t dec_cfg = DEFAULT_ESP_DECODER_CONFIG();
    dec_cfg.out_rb_size = 32 * 1024;
    s_decoder = esp_decoder_init(&dec_cfg, decoders,
                                 sizeof(decoders) / sizeof(decoders[0]));

    /* Выход: I²S на наш PCM5102A. Выводы те же, что у модуля audio;
     * MCLK не используется — у ЦАП вывод тактирования на земле. */
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_cfg.std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    i2s_cfg.std_cfg.gpio_cfg.bclk = PIN_I2S_BCK;
    i2s_cfg.std_cfg.gpio_cfg.ws   = PIN_I2S_LRCK;
    i2s_cfg.std_cfg.gpio_cfg.dout = PIN_I2S_DIN;
    i2s_cfg.std_cfg.gpio_cfg.din  = I2S_GPIO_UNUSED;
    s_i2s = i2s_stream_init(&i2s_cfg);

    if (!s_http || !s_decoder || !s_i2s) {
        ESP_LOGE(TAG, "не собрались элементы конвейера");
        return ESP_ERR_NO_MEM;
    }

    audio_pipeline_register(s_pipeline, s_http,    "http");
    audio_pipeline_register(s_pipeline, s_decoder, "dec");
    audio_pipeline_register(s_pipeline, s_i2s,     "i2s");

    const char *order[] = { "http", "dec", "i2s" };
    audio_pipeline_link(s_pipeline, &order[0], 3);

    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    s_events = audio_event_iface_init(&evt_cfg);
    audio_pipeline_set_listener(s_pipeline, s_events);

    xTaskCreate(events_task, "netradio_evt", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "конвейер собран: http -> декодер -> I²S");
    return ESP_OK;
}

/* ------------------------------------------------------------------ *
 *  Управление
 * ------------------------------------------------------------------ */

esp_err_t ipradio_netradio_play(const char *url, const char *station_name)
{
    if (!s_pipeline || !url || !url[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_active) {
        ipradio_netradio_stop();
    }

    strncpy(s_url, url, sizeof(s_url) - 1);
    s_url[sizeof(s_url) - 1] = '\0';
    s_retries = 0;
    s_active  = true;

    ESP_LOGI(TAG, "запуск: %s", station_name ? station_name : url);

    /* Пока идёт подключение и наполняется буфер, на экране должно быть
     * написано, что происходит, а не молчание. */
    ipradio_post_simple(IPRADIO_EV_PLAY_STATE, IPRADIO_PLAY_BUFFERING);

    audio_pipeline_reset_ringbuffer(s_pipeline);
    audio_pipeline_reset_elements(s_pipeline);
    audio_element_set_uri(s_http, s_url);

    return audio_pipeline_run(s_pipeline);
}

esp_err_t ipradio_netradio_stop(void)
{
    if (!s_pipeline || !s_active) {
        return ESP_OK;
    }

    s_active = false;

    audio_pipeline_stop(s_pipeline);
    audio_pipeline_wait_for_stop(s_pipeline);
    audio_pipeline_terminate(s_pipeline);

    ipradio_post_simple(IPRADIO_EV_PLAY_STATE, IPRADIO_PLAY_IDLE);
    return ESP_OK;
}

bool ipradio_netradio_active(void)
{
    return s_active;
}
