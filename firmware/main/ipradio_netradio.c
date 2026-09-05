/*
 * ipradio_netradio.c — конвейер интернет-радио на ESP-ADF.
 *
 * Схема конвейера:
 *
 *     http_stream  →  decoder  →  громкость (ALC)  →  i2s_stream  →  ЦАП
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

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/idf_additions.h"

#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "http_stream.h"
#include "i2s_stream.h"
#include "esp_decoder.h"
#include "audio_alc.h"
#include "ringbuf.h"

#include "board_pins.h"
#include "ipradio_board_codec.h"
#include "ipradio_netradio.h"
#include "ipradio_state.h"
#include "ipradio_watchdog.h"

static const char *TAG = "netradio";

#define RETRY_MAX          3
#define RETRY_DELAY_MS  2000

static audio_pipeline_handle_t s_pipeline;
static audio_element_handle_t  s_http;
static audio_element_handle_t  s_decoder;
static audio_element_handle_t  s_alc;
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

/* ВРЕМЕННО: раз в пять секунд печатать, сколько байт ушло в I2S.
 *
 * ЦАП не подключён, услышать нечего, а знать, идёт ли звук на выход,
 * надо. Счётчик byte_pos у элемента вывода растёт ровно тогда, когда
 * данные действительно уходят в шину. УБРАТЬ, когда подключим ЦАП. */
static void report_i2s_flow(void)
{
    static int64_t  prev;
    static int64_t  prev_us;

    if (!s_i2s || !s_active) {
        prev = 0;
        prev_us = 0;
        return;
    }

    int64_t now_us = esp_timer_get_time();
    if (prev_us && now_us - prev_us < 5000000) {
        return;
    }

    audio_element_info_t info = { 0 };
    audio_element_getinfo(s_i2s, &info);

    int64_t delta = info.byte_pos - prev;
    if (prev_us) {
        ESP_LOGW(TAG, "ЗАМЕР: в I2S ушло %lld байт, за 5 с прибавилось %lld "
                      "(%lld байт/с)",
                 (long long) info.byte_pos, (long long) delta,
                 (long long) (delta * 1000000 / (now_us - prev_us)));
    }
    prev = info.byte_pos;
    prev_us = now_us;
}

static void events_task(void *arg)
{
    (void) arg;
    audio_event_iface_msg_t msg;

    /* Лимит 10 с, а не 3, как было предложено изначально.
     * Причина в пути повтора ниже: там законная пауза 2 с плюс
     * audio_pipeline_wait_for_stop(), который ждёт остановки всех
     * элементов. Элемент HTTP в этот момент может сидеть в чтении
     * сокета на умершей сети, и остановка занимает секунды.
     *
     * С лимитом 3 с сторож перезагружал бы прибор ровно в той
     * ситуации, ради которой повтор и написан: при дрожащей сети.
     * Это хуже, чем отсутствие сторожа. */
    int wdt = ipradio_watchdog_register("netradio", 10000, 0);

    for (;;) {
        ipradio_watchdog_feed(wdt);
        /* Ждём событие не бесконечно: заполнение буфера надо
         * показывать и тогда, когда ничего не происходит, - а именно
         * тогда человек на него и смотрит. */
        if (audio_event_iface_listen(s_events, &msg,
                                     pdMS_TO_TICKS(500)) != ESP_OK) {
            report_buffer_fill();
            report_i2s_flow();
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

            ESP_LOGI(TAG, "поток: %d Гц, %d бит, %d канал(ов), %d бит/с",
                     info.sample_rates, info.bits, info.channels, info.bps);

            i2s_stream_set_clk(s_i2s, info.sample_rates,
                               info.bits, info.channels);

#if IPRADIO_USE_BOARD_CODEC
            /* Кодеку платы нужен тот же формат, что и шине: иначе
             * он будет тактоваться от своего и звук поедет по темпу. */
            ipradio_board_codec_set_format(info.sample_rates,
                                           info.bits, info.channels);
#endif

            /* Регулятору тоже надо знать про каналы: моно и стерео
             * он обрабатывает по-разному, а станции бывают и такие,
             * и такие. */
            if (s_alc && info.channels > 0) {
                alc_volume_setup_set_channel(s_alc, info.channels);
            }

            /* Битрейт декодер отдаёт в битах в секунду; на экране
             * его показываем в килобитах, как принято у станций. */
            if (info.bps > 0) {
                ipradio_post_simple(IPRADIO_EV_BITRATE, info.bps / 1000);
            }

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

                /* Пауза кончилась - отмечаемся, чтобы остаток
                 * повтора считался от этого момента, а не от начала
                 * витка. */
                ipradio_watchdog_feed(wdt);

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

                /* Погасить конвейер обязательно, а не просто снять
                 * признак.
                 *
                 * Раньше здесь стояло одно `s_active = false`,
                 * и элементы оставались запущенными. Следующее
                 * нажатие уходило в ipradio_netradio_play(), тот
                 * видел s_active == false, останавливать было
                 * «нечего», и звал audio_pipeline_run() поверх уже
                 * работающего конвейера. В журнале это выглядело как
                 * «Pipeline already started, state:3», после чего
                 * задача i2s начинала крутиться вхолостую, забирала
                 * ядро 0 у простоя, и сторож принимался срабатывать
                 * раз в пять секунд без конца. Снаружи это выглядело
                 * зависанием с мигающим экраном. */
                audio_pipeline_stop(s_pipeline);
                audio_pipeline_wait_for_stop(s_pipeline);
                audio_pipeline_terminate(s_pipeline);
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

    /* Набор корневых сертификатов: без него ни одна станция на https
     * не заиграет, а каталог отдаёт их изрядную долю. Такие станции
     * записывались бы в ячейку и молча уходили в «не отвечает».
     * Цена - около 70 КБ образа, место есть. */
    http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    http_cfg.out_rb_size = 64 * 1024;

    /* Стек ЗАДАЧИ элемента - в PSRAM.
     *
     * ADF умеет это сам, но создаёт такие задачи функцией
     * xTaskCreateRestrictedPinnedToCore, которой в обычном ESP-IDF
     * нет: она приходит с патчами из каталога idf_patches, а их мы
     * намеренно не применяли. Без неё элементы вообще не запускались
     * («Error creating RestrictedPinnedToCore»), и поток вечно висел
     * на «Подключение…».
     *
     * Некоторое время обходились обратным: просили внутреннюю память.
     * На плате это упёрлось в стену - в момент подключения свободной
     * внутренней оставалось 2 КБ, и рукопожатие TLS падало на
     * alloc(2389 bytes) failed.
     *
     * Теперь эту функцию мы определяем сами, в ipradio_adf_stack.c:
     * в ADF она объявлена слабой, так что патчи к ESP-IDF не нужны.
     * Четыре стека, почти 15 КБ, уезжают в PSRAM. */
    http_cfg.stack_in_ext = true;
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
    dec_cfg.stack_in_ext = true;    /* см. пояснение выше */
    s_decoder = esp_decoder_init(&dec_cfg, decoders,
                                 sizeof(decoders) / sizeof(decoders[0]));

    /* Регулятор громкости. Отдельным элементом, а не умножением
     * отсчётов на выходе декодера: буферами владеет ADF, и лезть
     * в них своим кодом значит спорить с ним за одни и те же данные
     * в двух задачах сразу.
     *
     * Стоит ПОСЛЕ декодера и ДО вывода: до декодера регулировать
     * нечего - там сжатый поток. */
    alc_volume_setup_cfg_t alc_cfg = DEFAULT_ALC_VOLUME_SETUP_CONFIG();
    alc_cfg.channel = 2;
    alc_cfg.stack_in_ext = true;    /* см. пояснение выше */
    s_alc = alc_volume_setup_init(&alc_cfg);

    /* Выход: I²S на наш PCM5102A. Порт нулевой - им владеет ТОЛЬКО
     * этот конвейер. Выводы те же, что описаны в board_pins.h;
     * MCLK не используется - у ЦАП вывод тактирования на земле. */
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_cfg.stack_in_ext = true;    /* см. пояснение выше */

    /* Ядро 1: на нулевом рисует LVGL. У этой задачи приоритет 23,
     * и на общем ядре она не оставляла отрисовке ничего - см.
     * пояснение в ipradio_ui.c рядом с task_core_id. */
    i2s_cfg.task_core = 1;
#if IPRADIO_USE_BOARD_CODEC
    /* ВРЕМЕННО: выход на штатный кодек платы ES8311, чтобы можно было
     * послушать до подключения своего ЦАП. Выводы — из BSP платы
     * и из поддержки этой же платы в ESP-ADF, они совпадают.
     * MCLK здесь обязателен: ES8311 без него не тактируется. */
    i2s_cfg.std_cfg.gpio_cfg.mclk = GPIO_NUM_13;
    i2s_cfg.std_cfg.gpio_cfg.bclk = GPIO_NUM_12;
    i2s_cfg.std_cfg.gpio_cfg.ws   = GPIO_NUM_10;
    i2s_cfg.std_cfg.gpio_cfg.dout = GPIO_NUM_9;
#else
    i2s_cfg.std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    i2s_cfg.std_cfg.gpio_cfg.bclk = PIN_I2S_BCK;
    i2s_cfg.std_cfg.gpio_cfg.ws   = PIN_I2S_LRCK;
    i2s_cfg.std_cfg.gpio_cfg.dout = PIN_I2S_DIN;
#endif
    i2s_cfg.std_cfg.gpio_cfg.din  = I2S_GPIO_UNUSED;
    s_i2s = i2s_stream_init(&i2s_cfg);

    if (!s_http || !s_decoder || !s_alc || !s_i2s) {
        ESP_LOGE(TAG, "не собрались элементы конвейера");
        return ESP_ERR_NO_MEM;
    }

    audio_pipeline_register(s_pipeline, s_http,    "http");
    audio_pipeline_register(s_pipeline, s_decoder, "dec");
    audio_pipeline_register(s_pipeline, s_alc,     "alc");
    audio_pipeline_register(s_pipeline, s_i2s,     "i2s");

    const char *order[] = { "http", "dec", "alc", "i2s" };
    audio_pipeline_link(s_pipeline, &order[0], 4);

    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    s_events = audio_event_iface_init(&evt_cfg);
    audio_pipeline_set_listener(s_pipeline, s_events);

    /* Стек в PSRAM: внутренняя нужна конвейеру и сети. */
    xTaskCreateWithCaps(events_task, "netradio_evt", 4096, NULL, 5, NULL,
                        MALLOC_CAP_SPIRAM);

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

    /* Гасим безусловно. Признак s_active говорит лишь о том, считаем
     * ли МЫ станцию играющей; конвейер может оставаться поднятым и
     * при снятом признаке - например, после исчерпания попыток. */
    ipradio_netradio_stop();
    audio_pipeline_terminate(s_pipeline);

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

/* ------------------------------------------------------------------ *
 *  Громкость
 * ------------------------------------------------------------------ */

void ipradio_netradio_set_volume(uint8_t logical, bool mute)
{
    if (!s_alc) {
        return;
    }

    /* Шкала ALC - децибелы, от -64 до +63, где 0 означает «как есть».
     * Это уже логарифм, поэтому таблица множителей, которая была
     * в звуковом модуле, здесь не нужна: достаточно линейно разложить
     * ход ручки по децибелам.
     *
     * Берём диапазон 40 дБ. Это примерно четыре «вдвое тише» подряд -
     * привычный ход для приёмника. Растянуть шире значит сделать
     * нижнюю половину ручки бесполезной: разницу между -55 и -60 дБ
     * на слух уже не отличить.
     *
     * Ноль шкалы и mute - не «очень тихо», а тишина: -64 дБ, при
     * котором ALC отдаёт молчание. */
    int db;

    if (mute || logical == 0) {
        db = -64;
    } else {
        if (logical > 100) {
            logical = 100;
        }
        db = -40 + ((int) logical * 40) / 100;
    }

    /* ВРЕМЕННО: печатаем, что реально ушло в регулятор. Проверяем,
     * не в уровнях ли дело при отсутствии звука. */
    ESP_LOGW(TAG, "ЗАМЕР: громкость %u%%, mute=%d -> ALC %d дБ",
             (unsigned) logical, (int) mute, db);
    alc_volume_setup_set_volume(s_alc, db);
}
