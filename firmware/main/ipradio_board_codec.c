/*
 * ipradio_board_codec.c — ВРЕМЕННО: звук через штатный кодек платы.
 *
 * Замысел и причины — в ipradio_board_codec.h.
 */

#include "esp_log.h"

#include "audio_codec_ctrl_if.h"
#include "audio_codec_gpio_if.h"
#include "es8311_codec.h"
#include "esp_codec_dev_defaults.h"
#include "esp_codec_dev_types.h"

#include "ipradio_board_codec.h"

static const char *TAG = "board.codec";

/* Выводы штатного тракта платы. Сверено с BSP Waveshare
 * (esp32_p4_wifi6_touch_lcd_5.h: BSP_I2S_*, BSP_POWER_AMP_IO) и
 * с поддержкой этой же платы в ESP-ADF
 * (audio_board/esp32_p4_function_ev_board/board_pins_config.c:54-58,
 * board_def.h:60) — совпадают полностью. */
#define BOARD_PA_GPIO   53

static const audio_codec_if_t *s_codec;

esp_err_t ipradio_board_codec_init(i2c_master_bus_handle_t bus)
{
    if (!bus) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_codec) {
        return ESP_OK;
    }

    /* Управление кодеком - по той же шине I²C, что тюнер и касание.
     * Адрес 0x30 - восьмибитный, то есть 0x18 со сдвигом. */
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port       = 0,
        .addr       = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = bus,
    };
    const audio_codec_ctrl_if_t *ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!ctrl) {
        ESP_LOGE(TAG, "не поднялось управление кодеком по I²C");
        return ESP_FAIL;
    }

    const audio_codec_gpio_if_t *gpio = audio_codec_new_gpio();

    es8311_codec_cfg_t cfg = {
        .ctrl_if     = ctrl,
        .gpio_if     = gpio,
        /* Только воспроизведение: микрофоны платы нам не нужны. */
        .codec_mode  = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin      = BOARD_PA_GPIO,
        .pa_reverted = false,
        /* Ведущий по I²S - процессор: тактирование задаёт конвейер,
         * кодек только принимает. */
        .master_mode = false,
        /* MCLK у ES8311 обязателен, вывод 13. */
        .use_mclk    = true,
    };

    s_codec = es8311_codec_new(&cfg);
    if (!s_codec) {
        ESP_LOGE(TAG, "кодек ES8311 не отозвался");
        return ESP_FAIL;
    }

    s_codec->enable(s_codec, true);

    /* Громкость держим на нуле децибел: регулировкой занимается
     * элемент ALC в конвейере, и два регулятора подряд только
     * запутали бы шкалу. */
    if (s_codec->set_vol) {
        s_codec->set_vol(s_codec, 0);
    }
    if (s_codec->mute) {
        s_codec->mute(s_codec, false);
    }

    ESP_LOGW(TAG, "ВРЕМЕННО: звук идёт в штатный кодек платы, "
                  "динамик в разъём GH1.25");
    return ESP_OK;
}

esp_err_t ipradio_board_codec_set_format(int sample_rate, int bits, int channels)
{
    if (!s_codec || !s_codec->set_fs) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = (uint8_t) bits,
        .channel         = (uint8_t) channels,
        .sample_rate     = (uint32_t) sample_rate,
    };
    int ret = s_codec->set_fs(s_codec, &fs);
    if (ret != 0) {
        ESP_LOGE(TAG, "кодек не принял формат: %d", ret);
        return ESP_FAIL;
    }

    /* Формат меняет тактирование кодека, а вместе с ним и состояние
     * тракта. Перечитываем включение и уровень после настройки:
     * порядок «сначала включить, потом задать частоту» у ES8311
     * не гарантирует, что выход останется открытым. */
    s_codec->enable(s_codec, true);
    if (s_codec->set_vol) {
        s_codec->set_vol(s_codec, 0);
    }
    if (s_codec->mute) {
        s_codec->mute(s_codec, false);
    }

    /* ВРЕМЕННО: полный дамп регистров. Без него не отличить
     * «кодек молчит» от «до кодека не доходит». */
    if (s_codec->dump_reg) {
        ESP_LOGW(TAG, "ЗАМЕР: регистры ES8311 после настройки формата");
        s_codec->dump_reg(s_codec);
    }
    return ESP_OK;
}
