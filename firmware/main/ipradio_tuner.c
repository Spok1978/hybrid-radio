/*
 * ipradio_tuner.c — драйвер RDA5807M.
 *
 * Особенности чипа, из-за которых драйвер выглядит именно так
 * (даташит RDA5807M, разбор — docs/02-fm-frontend.md и docs/23-volume-control.md):
 *
 *   - У чипа ДВА адреса на шине. 0x10 — последовательный доступ:
 *     запись всегда начинается с регистра 02H и идёт подряд.
 *     0x11 — произвольный доступ к одному регистру. Мы пользуемся
 *     последовательным для записи и произвольным для чтения статуса:
 *     так меньше обменов.
 *   - Бит DMUTE ИНВЕРСНЫЙ: 0 это mute, 1 это нормальная работа.
 *     На этом легко ошибиться, поэтому вся работа с ним идёт через
 *     одну функцию.
 *   - При VOLUME=0000 выход уходит в высокоимпедансное состояние,
 *     а не просто становится тихим. Для пассивной суммы это в плюс.
 *   - Шкала громкости логарифмическая, и ступеней всего шестнадцать.
 *
 * Регистры чипа шестнадцатибитные, порядок байтов старшим вперёд.
 */

#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "board_pins.h"
#include "ipradio_tuner.h"

static const char *TAG = "tuner";

/* ------------------------------------------------------------------ *
 *  Регистры
 * ------------------------------------------------------------------ */

#define REG_02H   0x02   /* управление: питание, mute, диапазон, seek */
#define REG_03H   0x03   /* частота, диапазон, шаг сетки              */
#define REG_04H   0x04   /* прочее                                     */
#define REG_05H   0x05   /* громкость, порог автопоиска                */
#define REG_0AH   0x0A   /* статус: seek, RDS, уровень                 */
#define REG_0BH   0x0B   /* уровень сигнала                            */

/* Регистр 02H */
#define R02_DHIZ      (1u << 15)  /* 1 = выход включён, 0 = Hi-Z        */
#define R02_DMUTE     (1u << 14)  /* ИНВЕРСНЫЙ: 1 = звук, 0 = mute      */
#define R02_MONO      (1u << 13)
#define R02_BASS      (1u << 12)
#define R02_SEEKUP    (1u << 9)
#define R02_SEEK      (1u << 8)
#define R02_SKMODE    (1u << 7)   /* 1 = не переходить через край       */
#define R02_RDS_EN    (1u << 3)
#define R02_NEW       (1u << 2)   /* «новый метод», нужен RDA5807M      */
#define R02_SOFT_RST  (1u << 1)
#define R02_ENABLE    (1u << 0)

/* Регистр 03H */
#define R03_TUNE      (1u << 4)
#define R03_BAND_SHIFT  2
#define R03_SPACE_SHIFT 0

/* Регистр 05H */
#define R05_VOLUME_MASK   0x000F
#define R05_SEEKTH_SHIFT  8
#define R05_INT_MODE   (1u << 15)
#define R05_LNA_PORT   (3u << 6)   /* вход антенны: LNAP                */

/* Регистр 0AH */
#define R0A_RDSR      (1u << 15)  /* группа RDS готова                  */
#define R0A_STC       (1u << 14)  /* seek или tune завершены            */
#define R0A_SF        (1u << 13)  /* seek не нашёл станцию              */
#define R0A_RDSS      (1u << 12)  /* декодер RDS синхронизирован        */
#define R0A_ST        (1u << 10)  /* принимается стерео                 */

/* ------------------------------------------------------------------ *
 *  Состояние драйвера
 * ------------------------------------------------------------------ */

static i2c_master_dev_handle_t s_dev_seq;     /* 0x10 */
static i2c_master_dev_handle_t s_dev_random;  /* 0x11 */
static bool           s_present;

static uint16_t       s_r02;    /* теневые копии: чип не даёт их прочесть */
static uint16_t       s_r03;
static uint16_t       s_r05;

static ipradio_band_t s_band = IPRADIO_BAND_CCIR;
static uint32_t       s_freq_khz = TUNER_CCIR_MIN_KHZ;
static uint8_t        s_signal;

/* ------------------------------------------------------------------ *
 *  Низкий уровень
 * ------------------------------------------------------------------ */

/* Последовательная запись: чип сам начинает с 02H и увеличивает адрес.
 * Пишем сразу четыре регистра — 02H, 03H, 04H, 05H, — потому что
 * отдельной записи в 03H без 02H у этого чипа нет. */
static esp_err_t write_regs(void)
{
    uint8_t buf[8] = {
        (uint8_t) (s_r02 >> 8), (uint8_t) s_r02,
        (uint8_t) (s_r03 >> 8), (uint8_t) s_r03,
        0x00, 0x00,                       /* 04H: значения по умолчанию */
        (uint8_t) (s_r05 >> 8), (uint8_t) s_r05,
    };
    return i2c_master_transmit(s_dev_seq, buf, sizeof(buf), 100);
}

/* Чтение одного регистра по произвольному адресу. */
static esp_err_t read_reg(uint8_t reg, uint16_t *out)
{
    uint8_t addr = reg;
    uint8_t rx[2] = { 0, 0 };
    esp_err_t err = i2c_master_transmit_receive(s_dev_random,
                                               &addr, 1, rx, 2, 100);
    if (err == ESP_OK) {
        *out = (uint16_t) ((rx[0] << 8) | rx[1]);
    }
    return err;
}

/* ------------------------------------------------------------------ *
 *  Диапазоны и сетка
 * ------------------------------------------------------------------ */

static uint32_t band_min(ipradio_band_t b)
{
    return (b == IPRADIO_BAND_OIRT) ? TUNER_OIRT_MIN_KHZ : TUNER_CCIR_MIN_KHZ;
}

static uint32_t band_max(ipradio_band_t b)
{
    return (b == IPRADIO_BAND_OIRT) ? TUNER_OIRT_MAX_KHZ : TUNER_CCIR_MAX_KHZ;
}

static uint32_t band_step(ipradio_band_t b)
{
    return (b == IPRADIO_BAND_OIRT) ? TUNER_STEP_OIRT_KHZ : TUNER_STEP_CCIR_KHZ;
}

/* Нижняя граница шкалы, от которой чип считает номер канала.
 * Для УКВ это 65 МГц, а не 65,8: у чипа диапазон BAND=3 начинается
 * именно оттуда, и вещательная граница лежит внутри него. */
static uint32_t band_base(ipradio_band_t b)
{
    return (b == IPRADIO_BAND_OIRT) ? 65000 : 87000;
}

/* Код диапазона и шага для регистра 03H. */
static void band_bits(ipradio_band_t b, uint8_t *band_code, uint8_t *space_code)
{
    if (b == IPRADIO_BAND_OIRT) {
        *band_code  = 3;   /* 65–76 МГц */
        *space_code = 1;   /* 50 кГц    */
    } else {
        *band_code  = 0;   /* 87–108 МГц */
        *space_code = 0;   /* 100 кГц    */
    }
}

static uint32_t clamp_freq(uint32_t khz, ipradio_band_t b)
{
    if (khz < band_min(b)) return band_min(b);
    if (khz > band_max(b)) return band_max(b);
    return khz;
}

/* ------------------------------------------------------------------ *
 *  Публичные операции
 * ------------------------------------------------------------------ */

esp_err_t ipradio_tuner_set_freq(uint32_t khz)
{
    if (!s_present) {
        return ESP_ERR_INVALID_STATE;
    }

    khz = clamp_freq(khz, s_band);

    uint32_t base = band_base(s_band);
    uint32_t step = band_step(s_band);
    uint32_t chan = (khz - base) / step;

    /* Округляем к ближайшему узлу сетки: пользователь крутит энкодер,
     * а не вводит частоту с точностью до килогерца. */
    s_freq_khz = base + chan * step;

    uint8_t band_code, space_code;
    band_bits(s_band, &band_code, &space_code);

    s_r03 = (uint16_t) ((chan << 6) | R03_TUNE |
                        (band_code << R03_BAND_SHIFT) |
                        (space_code << R03_SPACE_SHIFT));

    return write_regs();
}

esp_err_t ipradio_tuner_set_band(ipradio_band_t band)
{
    s_band = band;
    return ipradio_tuner_set_freq(clamp_freq(s_freq_khz, band));
}

esp_err_t ipradio_tuner_step(int steps)
{
    int64_t f = (int64_t) s_freq_khz + (int64_t) steps * band_step(s_band);
    if (f < 0) {
        f = 0;
    }
    return ipradio_tuner_set_freq((uint32_t) f);
}

esp_err_t ipradio_tuner_seek(bool up)
{
    if (!s_present) {
        return ESP_ERR_INVALID_STATE;
    }

    /* SKMODE=1: дойдя до края диапазона, остановиться, а не перескочить
     * на другой конец. Иначе автопоиск ходит по кругу вечно. */
    s_r02 |= R02_SEEK | R02_SKMODE;
    if (up) {
        s_r02 |= R02_SEEKUP;
    } else {
        s_r02 &= ~R02_SEEKUP;
    }
    return write_regs();
}

/* Логическая шкала 0…100 в четырёхбитное поле.
 * Ступеней всего шестнадцать, и шкала чипа логарифмическая, поэтому
 * простое деление здесь уместно: логарифм уже «внутри» чипа. */
esp_err_t ipradio_tuner_set_volume(uint8_t logical)
{
    if (!s_present) {
        return ESP_ERR_INVALID_STATE;
    }
    if (logical > 100) {
        logical = 100;
    }

    uint16_t vol = (uint16_t) ((logical * 15 + 50) / 100);

    s_r05 = (s_r05 & ~R05_VOLUME_MASK) | vol;
    return write_regs();
}

esp_err_t ipradio_tuner_set_mute(bool mute)
{
    if (!s_present) {
        return ESP_ERR_INVALID_STATE;
    }
    /* DMUTE инверсный: единица означает звук. Единственное место
     * в драйвере, где этот бит трогается. */
    if (mute) {
        s_r02 &= ~R02_DMUTE;
    } else {
        s_r02 |= R02_DMUTE;
    }
    return write_regs();
}

esp_err_t ipradio_tuner_set_hiz(bool hiz)
{
    if (!s_present) {
        return ESP_ERR_INVALID_STATE;
    }
    if (hiz) {
        s_r02 &= ~R02_DHIZ;
    } else {
        s_r02 |= R02_DHIZ;
    }
    return write_regs();
}

uint8_t ipradio_tuner_signal(void)
{
    return s_signal;
}

bool ipradio_tuner_present(void)
{
    return s_present;
}

/* ------------------------------------------------------------------ *
 *  Опрос статуса
 * ------------------------------------------------------------------ */

/* Отдельная задача: чип надо периодически спрашивать про уровень
 * сигнала, завершение поиска и готовность RDS. Раз в 200 мс —
 * достаточно для шкалы уровня и не грузит шину, которую делим
 * с тачем и кодеками. */
static void tuner_task(void *arg)
{
    (void) arg;
    bool seeking = false;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(200));

        uint16_t r0a = 0, r0b = 0;
        if (read_reg(REG_0AH, &r0a) != ESP_OK) {
            continue;
        }
        read_reg(REG_0BH, &r0b);

        /* Уровень: старшие семь бит регистра 0BH, приводим к 0…100. */
        uint8_t rssi = (uint8_t) ((r0b >> 9) & 0x7F);
        s_signal = (uint8_t) ((rssi * 100) / 127);
        ipradio_post_simple(IPRADIO_EV_SIGNAL_LEVEL, s_signal);

        /* Завершение автопоиска. Флаг STC держится до следующей команды,
         * поэтому реагируем на его появление, а не на уровень. */
        bool stc = (r0a & R0A_STC) != 0;
        if (stc && !seeking) {
            seeking = true;
            if (r0a & R0A_SF) {
                ESP_LOGI(TAG, "автопоиск: станция не найдена");
            } else {
                /* Прочитать фактическую частоту: чип мог уйти дальше
                 * той, что мы просили. */
                uint16_t chan = r0a & 0x03FF;
                s_freq_khz = band_base(s_band) + chan * band_step(s_band);
                ESP_LOGI(TAG, "настроен на %" PRIu32 " кГц", s_freq_khz);
            }
            /* Снять команду поиска, иначе чип уйдёт искать снова. */
            s_r02 &= ~R02_SEEK;
            write_regs();
        } else if (!stc) {
            seeking = false;
        }

        /* RDS появится вместе с декодером групп: пока только
         * отмечаем синхронизацию. */
        if (r0a & R0A_RDSS) {
            /* декодер синхронизирован */
        }
    }
}

/* ------------------------------------------------------------------ *
 *  Инициализация
 * ------------------------------------------------------------------ */

esp_err_t ipradio_tuner_init(i2c_master_bus_handle_t bus)
{
    if (!bus) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .scl_speed_hz    = 100000,   /* чип не любит быстрее */
    };

    cfg.device_address = I2C_ADDR_TUNER_SEQ;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &cfg, &s_dev_seq));

    cfg.device_address = I2C_ADDR_TUNER_RANDOM;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &cfg, &s_dev_random));

    if (i2c_master_probe(bus, I2C_ADDR_TUNER_SEQ, 100) != ESP_OK) {
        ESP_LOGW(TAG, "тюнер не отвечает на 0x%02X — эфир недоступен",
                 I2C_ADDR_TUNER_SEQ);
        s_present = false;
        return ESP_ERR_NOT_FOUND;
    }
    s_present = true;

    /* Сброс, затем включение. Между ними пауза: чипу нужно время
     * на запуск внутреннего генератора. */
    s_r02 = R02_SOFT_RST | R02_ENABLE;
    write_regs();
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Рабочее состояние: выход включён, звук не заглушён, «новый метод»,
     * RDS включён. Начинаем приглушёнными — громкость выставит автомат. */
    s_r02 = R02_DHIZ | R02_DMUTE | R02_NEW | R02_RDS_EN | R02_ENABLE;
    s_r05 = R05_INT_MODE | R05_LNA_PORT | (8u << R05_SEEKTH_SHIFT) | 0;
    ipradio_tuner_set_freq(s_freq_khz);

    ESP_LOGI(TAG, "тюнер поднят, диапазон %s, %" PRIu32 " кГц",
             (s_band == IPRADIO_BAND_OIRT) ? "УКВ" : "FM", s_freq_khz);

    xTaskCreate(tuner_task, "ipradio_tuner", 3072, NULL, 5, NULL);
    return ESP_OK;
}
