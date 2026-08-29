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
#include "ipradio_watchdog.h"

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

/* Регистры блоков RDS.
 *
 * ВНИМАНИЕ, НЕ ПРОВЕРЕНО. Даташита RDA5807M в проекте нет, и эти
 * четыре адреса взяты по общим сведениям о семействе, а не прочитаны
 * из документа. Всё остальное в этом файле опирается на разбор
 * даташита, записанный в docs/26-firmware-spec.md, §5.1.
 *
 * Это ПЕРВОЕ, что надо сверить на живой плате: если имена станций
 * не появляются или приходят мусором, проверять надо здесь, а не
 * в декодере ниже. Признак того, что адреса верны: при настройке
 * на вещающую станцию блок B почти всегда даёт осмысленный код
 * группы (старшие четыре бита), а не 0x0000 и не 0xFFFF. */
#define REG_0CH   0x0C   /* блок A: код станции (PI)                   */
#define REG_0DH   0x0D   /* блок B: тип группы и служебные поля        */
#define REG_0EH   0x0E   /* блок C                                      */
#define REG_0FH   0x0F   /* блок D: для группы 0 - две буквы имени     */

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

/* ------------------------------------------------------------------ *
 *  Состояние декодера RDS
 * ------------------------------------------------------------------ */

/* Имя станции по RDS - восемь знаков, приходящих по две штуки
 * в четырёх сообщениях. Собираем их здесь.
 *
 * s_ps_draft   - то, что пришло последним
 * s_ps_stable  - то, что подтвердилось повторным приёмом
 * s_ps_sent    - то, что уже отдано автомату
 *
 * Три буфера, а не один, из-за помех. Эфир шумит, и одна принятая
 * группа запросто содержит искажённый знак. Поэтому знак принимается
 * только тогда, когда пришёл дважды подряд одинаковым - приём
 * ошибается случайно, а вещатель передаёт одно и то же по кругу.
 * Без этой проверки на экране плясали бы опечатки. */
#define PS_LEN 8

static char    s_ps_draft[PS_LEN + 1];
static char    s_ps_stable[PS_LEN + 1];
static char    s_ps_sent[PS_LEN + 1];
static uint8_t s_ps_confirmed;      /* битовая маска подтверждённых мест */

static void rds_reset(void);   /* зовётся из перестройки, а живёт ниже */

/* Идёт ли проход по диапазону. Задача опроса состояния при этом
 * не трогает флаг завершения поиска: иначе две задачи снимали бы
 * одну и ту же команду, и проход спотыкался бы через раз. */
static volatile bool s_scanning;
static volatile bool s_scan_abort;
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
    rds_reset();

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

/* ------------------------------------------------------------------ *
 *  Проход по диапазону
 * ------------------------------------------------------------------ */

/* Сколько ждать одного шага поиска. Чип проходит диапазон не мгновенно:
 * на каждой частоте он оценивает уровень и качество. Три секунды -
 * с большим запасом; если не уложился, значит что-то не так с чипом,
 * и продолжать проход бессмысленно. */
/* Гистерезис уровня сигнала, в единицах шкалы 0…100. Два - это
 * примерно шум одного разряда RSSI после пересчёта. */
#define SIGNAL_HYST   2

/* Сколько подряд неудачных чтений считать отказом чипа. Двадцать пять
 * при периоде опроса 200 мс - это пять секунд молчания шины. Одиночные
 * ошибки бывают от занятости шины и тревоги не стоят. */
#define TUNER_FAIL_LIMIT  25

#define SEEK_STEP_TIMEOUT_MS  3000
#define SEEK_POLL_MS            50

/* Предел числа шагов. На 87,5-108 МГц с шагом 100 кГц каналов 206,
 * и столько станций не бывает даже теоретически. Предел нужен
 * не для этого: он защищает от чипа, который по какой-то причине
 * перестал двигаться вперёд и отвечает одной и той же частотой. */
#define SEEK_MAX_STEPS         250

static ipradio_tuner_scan_cb_t s_scan_cb;
static void                   *s_scan_ctx;

/* Дождаться завершения одного шага поиска.
 * Возвращает регистр 0AH или ноль, если не дождались. */
static uint16_t seek_wait(void)
{
    for (int i = 0; i < SEEK_STEP_TIMEOUT_MS / SEEK_POLL_MS; i++) {
        vTaskDelay(pdMS_TO_TICKS(SEEK_POLL_MS));

        if (s_scan_abort) {
            return 0;
        }

        uint16_t r0a = 0;
        if (read_reg(REG_0AH, &r0a) == ESP_OK && (r0a & R0A_STC)) {
            return r0a;
        }
    }
    return 0;
}

static void scan_task(void *arg)
{
    (void) arg;

    const uint32_t hi = band_max(s_band);
    uint32_t last = 0;
    int found = 0;

    ESP_LOGI(TAG, "проход по диапазону начат");

    /* Становимся на нижнюю границу: искать надо весь диапазон,
     * а не от того места, где человек оставил приёмник. */
    ipradio_tuner_set_freq(band_min(s_band));
    vTaskDelay(pdMS_TO_TICKS(100));

    for (int step = 0; step < SEEK_MAX_STEPS && !s_scan_abort; step++) {

        /* SKMODE=1: дойдя до края, остановиться, а не перескочить
         * на другой конец. Без него проход не кончился бы никогда. */
        s_r02 |= R02_SEEK | R02_SKMODE | R02_SEEKUP;
        if (write_regs() != ESP_OK) {
            break;
        }

        uint16_t r0a = seek_wait();

        /* Команду снимаем в любом случае, даже не дождавшись: иначе
         * чип уйдёт искать снова, как только мы отвернёмся. */
        s_r02 &= ~R02_SEEK;
        write_regs();

        if (r0a == 0) {
            ESP_LOGW(TAG, "шаг поиска не завершился, проход прерван");
            break;
        }

        if (r0a & R0A_SF) {
            /* Край диапазона: станций дальше нет. Это нормальное
             * завершение, а не ошибка. */
            break;
        }

        uint32_t f = band_base(s_band) +
                     (uint32_t) (r0a & 0x03FF) * band_step(s_band);

        /* Чип не сдвинулся вперёд - дальше идти некуда. Проверка
         * не теоретическая: на краю диапазона SEEK может возвращать
         * одну и ту же частоту, не выставляя SF. */
        if (f <= last) {
            break;
        }

        last = f;
        s_freq_khz = f;
        rds_reset();
        found++;

        if (s_scan_cb) {
            s_scan_cb(f, false, s_scan_ctx);
        }

        if (f >= hi) {
            break;
        }
    }

    ESP_LOGI(TAG, "проход закончен, найдено станций: %d", found);

    s_scanning = false;
    if (s_scan_cb) {
        s_scan_cb(0, true, s_scan_ctx);
    }

    vTaskDelete(NULL);
}

esp_err_t ipradio_tuner_scan_start(ipradio_tuner_scan_cb_t cb, void *ctx)
{
    if (!s_present) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_scanning) {
        return ESP_ERR_INVALID_STATE;
    }

    s_scan_cb    = cb;
    s_scan_ctx   = ctx;
    s_scan_abort = false;
    s_scanning   = true;

    BaseType_t ok = xTaskCreate(scan_task, "tuner_scan", 4096, NULL, 4, NULL);
    if (ok != pdPASS) {
        s_scanning = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool ipradio_tuner_scanning(void)
{
    return s_scanning;
}

void ipradio_tuner_scan_abort(void)
{
    s_scan_abort = true;
}

/* ------------------------------------------------------------------ *
 *  RDS: имя станции
 * ------------------------------------------------------------------ */

/* Сброс при уходе с частоты. Обязателен: иначе на новой станции
 * несколько секунд висело бы имя предыдущей, и это хуже, чем пустота -
 * человек решит, что настроился не туда. */
static void rds_reset(void)
{
    memset(s_ps_draft,  0, sizeof(s_ps_draft));
    memset(s_ps_stable, 0, sizeof(s_ps_stable));
    memset(s_ps_sent,   0, sizeof(s_ps_sent));
    s_ps_confirmed = 0;

    ipradio_event_t ev = { .type = IPRADIO_EV_RDS_UPDATE, .text = "" };
    ipradio_post(&ev);
}

/* Годится ли знак для показа. В RDS передаются и управляющие коды,
 * и знаки из наборов, которых в нашем шрифте нет. Всё, что не входит
 * в печатную латиницу, заменяем пробелом: пустое место читается как
 * пустое место, а прямоугольник неизвестного знака - как поломка. */
static char ps_sanitize(uint8_t c)
{
    return (c >= 0x20 && c <= 0x7E) ? (char) c : ' ';
}

/* Разбор одной принятой группы. Нас интересует только группа 0:
 * именно она несёт имя станции. Всё прочее - время, радиотекст,
 * альтернативные частоты - пропускаем: на экране им места нет. */
static void rds_feed(uint16_t block_b, uint16_t block_d)
{
    /* Старшие четыре бита блока B - номер группы. */
    uint8_t group = (uint8_t) (block_b >> 12);
    if (group != 0) {
        return;
    }

    /* Младшие два бита - какая из четырёх пар знаков пришла. */
    uint8_t seg = (uint8_t) (block_b & 0x03);
    uint8_t pos = (uint8_t) (seg * 2);

    char c0 = ps_sanitize((uint8_t) (block_d >> 8));
    char c1 = ps_sanitize((uint8_t) (block_d & 0xFF));

    /* Подтверждение повторным приёмом: знак попадает в стабильное имя
     * только если он такой же, каким пришёл в прошлый раз. */
    bool same0 = (s_ps_draft[pos]     == c0);
    bool same1 = (s_ps_draft[pos + 1] == c1);

    s_ps_draft[pos]     = c0;
    s_ps_draft[pos + 1] = c1;

    if (same0 && same1) {
        s_ps_stable[pos]     = c0;
        s_ps_stable[pos + 1] = c1;
        s_ps_confirmed |= (uint8_t) (1u << seg);
    }

    /* Отдаём имя, только когда подтверждены все четыре пары. Показывать
     * половину - значит показать обрубок вроде "РАДИО   " и заставить
     * человека гадать, дочитается ли остальное. */
    if (s_ps_confirmed != 0x0F) {
        return;
    }

    char name[PS_LEN + 1];
    memcpy(name, s_ps_stable, PS_LEN);
    name[PS_LEN] = 0;

    /* Вещатели дополняют имя пробелами до восьми знаков. */
    for (int i = PS_LEN - 1; i >= 0 && name[i] == 0x20; i--) {
        name[i] = 0;
    }

    if (strcmp(name, s_ps_sent) == 0) {
        return;                      /* не изменилось - молчим */
    }

    snprintf(s_ps_sent, sizeof(s_ps_sent), "%s", name);

    ipradio_event_t ev = { .type = IPRADIO_EV_RDS_UPDATE, .text = name };
    ipradio_post(&ev);

    ESP_LOGI(TAG, "RDS: %s", name);
}

/* Отдельная задача: чип надо периодически спрашивать про уровень
 * сигнала, завершение поиска и готовность RDS. Раз в 200 мс —
 * достаточно для шкалы уровня и не грузит шину, которую делим
 * с тачем и кодеками. */
static void tuner_task(void *arg)
{
    (void) arg;
    bool seeking = false;

    /* Последний отправленный уровень и число подряд идущих ошибок
     * шины. Оба живут только внутри этой задачи. */
    uint8_t last_signal = 0xFF;      /* заведомо не равно первому  */
    int     read_fails  = 0;

    int wdt = ipradio_watchdog_register("tuner", 2000, 0);

    for (;;) {
        ipradio_watchdog_feed(wdt);
        vTaskDelay(pdMS_TO_TICKS(200));

        uint16_t r0a = 0, r0b = 0;
        if (read_reg(REG_0AH, &r0a) != ESP_OK) {
            /* Раньше просто ждали следующие 200 мс. При отвалившемся
             * чипе это значило, что прибор молчит об отказе, а на
             * экране застывает уровень сигнала - выглядит как
             * «станция слабая», а не как «тюнера нет».
             *
             * Считаем подряд идущие ошибки: одиночные бывают от
             * занятой шины, которую делим с тачем и кодеками,
             * и поднимать из-за них тревогу незачем. */
            if (++read_fails >= TUNER_FAIL_LIMIT) {
                ESP_LOGE(TAG, "тюнер не отвечает %d раз подряд, "
                              "считаем отсутствующим", read_fails);
                s_present = false;
                ipradio_post_simple(IPRADIO_EV_SIGNAL_LEVEL, 0);
                /* Задачу не останавливаем: чип может вернуться,
                 * например если дело было в питании. */
                read_fails = 0;
            }
            continue;
        }

        if (read_fails > 0 || !s_present) {
            if (!s_present) {
                ESP_LOGI(TAG, "тюнер снова отвечает, программируем заново");
                s_present = true;

                /* Просто поднять признак мало. Если чип пропадал
                 * по питанию, он вернулся с заводскими регистрами:
                 * диапазон, громкость, пороги - не наши. А теневые
                 * копии в прошивке утверждают обратное, и рассинхрон
                 * остался бы навсегда. Пишем всё заново. */
                write_regs();
                ipradio_tuner_set_band(s_band);
                ipradio_tuner_set_freq(s_freq_khz);
            }
            read_fails = 0;
        }

        read_reg(REG_0BH, &r0b);

        /* Уровень: старшие семь бит регистра 0BH, приводим к 0…100. */
        uint8_t rssi = (uint8_t) ((r0b >> 9) & 0x7F);
        s_signal = (uint8_t) ((rssi * 100) / 127);

        /* Событие шлём только на заметное изменение.
         *
         * Раньше слали безусловно, пять раз в секунду. Каждое такое
         * событие будит автомат, а тот обходит всех подписчиков:
         * мост лезет к железу, интерфейс перерисовывает экран. Ради
         * цифры, которая обычно стоит на месте.
         *
         * Гистерезис обязателен, а не желателен: показания RSSI
         * у RDA5807M шумят на единицу-другую, и без порога шторм
         * сменился бы дребезгом на границе значений. */
        if (s_signal > last_signal + SIGNAL_HYST ||
            s_signal + SIGNAL_HYST < last_signal) {
            last_signal = s_signal;
            ipradio_post_simple(IPRADIO_EV_SIGNAL_LEVEL, s_signal);
        }

        /* Завершение автопоиска. Флаг STC держится до следующей команды,
         * поэтому реагируем на его появление, а не на уровень. */
        /* Пока идёт проход по диапазону, флаг завершения принадлежит
         * задаче поиска. Снимать команду отсюда значило бы обрывать
         * ей каждый второй шаг. */
        bool stc = (r0a & R0A_STC) != 0 && !s_scanning;
        if (stc && !seeking) {
            seeking = true;
            if (r0a & R0A_SF) {
                ESP_LOGI(TAG, "автопоиск: станция не найдена");
            } else {
                /* Прочитать фактическую частоту: чип мог уйти дальше
                 * той, что мы просили. */
                uint16_t chan = r0a & 0x03FF;
                s_freq_khz = band_base(s_band) + chan * band_step(s_band);
                rds_reset();
                ESP_LOGI(TAG, "настроен на %" PRIu32 " кГц", s_freq_khz);
            }
            /* Снять команду поиска, иначе чип уйдёт искать снова. */
            s_r02 &= ~R02_SEEK;
            write_regs();
        } else if (!stc) {
            seeking = false;
        }

        /* RDS. Читаем только когда чип говорит, что группа готова
         * И декодер синхронизирован: без синхронизации в регистрах
         * лежит мусор, и подтверждение повторным приёмом его
         * не отсеет - мусор тоже бывает одинаковым дважды. */
        if ((r0a & R0A_RDSR) && (r0a & R0A_RDSS)) {
            uint16_t rb = 0, rd = 0;
            if (read_reg(REG_0DH, &rb) == ESP_OK &&
                read_reg(REG_0FH, &rd) == ESP_OK) {
                rds_feed(rb, rd);
            }
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
