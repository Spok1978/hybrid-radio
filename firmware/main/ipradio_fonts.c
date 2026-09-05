/*
 * ipradio_fonts.c — создание шрифтов из встроенных TTF.
 *
 * Замысел и обоснование — в ipradio_fonts.h.
 */

#include <stddef.h>

#include "esp_log.h"

#include "ipradio_fonts.h"

static const char *TAG = "fonts";

/* Файлы, вложенные в образ через EMBED_FILES в main/CMakeLists.txt.
 * Имена символов собирает система сборки ESP-IDF из имени файла:
 * точки и разделители пути превращаются в подчёркивания. */
extern const uint8_t dejavu_ru_ttf_start[]      asm("_binary_dejavu_ru_ttf_start");
extern const uint8_t dejavu_ru_ttf_end[]        asm("_binary_dejavu_ru_ttf_end");
extern const uint8_t dejavu_ru_bold_ttf_start[] asm("_binary_dejavu_ru_bold_ttf_start");
extern const uint8_t dejavu_ru_bold_ttf_end[]   asm("_binary_dejavu_ru_bold_ttf_end");

const lv_font_t *ipradio_font_14;
const lv_font_t *ipradio_font_16;
const lv_font_t *ipradio_font_20;
const lv_font_t *ipradio_font_22;
const lv_font_t *ipradio_font_28;
const lv_font_t *ipradio_font_48b;
const lv_font_t *ipradio_font_28b;
const lv_font_t *ipradio_font_40;
const lv_font_t *ipradio_font_56;
const lv_font_t *ipradio_font_64b;
const lv_font_t *ipradio_font_96b;

esp_err_t ipradio_fonts_init(void)
{
    const size_t reg_size  = (size_t) (dejavu_ru_ttf_end - dejavu_ru_ttf_start);
    const size_t bold_size = (size_t) (dejavu_ru_bold_ttf_end - dejavu_ru_bold_ttf_start);

    /* Каждый вызов даёт отдельный lv_font_t со своим кэшем глифов,
     * но разбор самого TTF идёт по одному и тому же куску образа:
     * пять кеглей не стоят пяти копий шрифта. */
    struct {
        const lv_font_t **dst;
        const void       *data;
        size_t            size;
        int32_t           px;
    } wanted[] = {
        { &ipradio_font_14,  dejavu_ru_ttf_start,      reg_size,  14 },
        { &ipradio_font_16,  dejavu_ru_ttf_start,      reg_size,  16 },
        { &ipradio_font_20,  dejavu_ru_ttf_start,      reg_size,  20 },
        { &ipradio_font_22,  dejavu_ru_ttf_start,      reg_size,  22 },
        { &ipradio_font_28,  dejavu_ru_ttf_start,      reg_size,  28 },
        { &ipradio_font_48b, dejavu_ru_bold_ttf_start, bold_size, 48 },
        { &ipradio_font_28b, dejavu_ru_bold_ttf_start, bold_size, 28 },
        { &ipradio_font_40,  dejavu_ru_ttf_start,      reg_size,  40 },
        { &ipradio_font_56,  dejavu_ru_ttf_start,      reg_size,  56 },
        { &ipradio_font_64b, dejavu_ru_bold_ttf_start, bold_size, 64 },
        { &ipradio_font_96b, dejavu_ru_bold_ttf_start, bold_size, 96 },
    };

    for (size_t i = 0; i < sizeof(wanted) / sizeof(wanted[0]); i++) {
        /* КЕРНИНГ ВЫКЛЮЧЕН, и это не про красоту.
         *
         * На живой плате 2026-08-30 подъём интерфейса вставал намертво
         * при построении клавиатуры: сторож ловил голодание, а адрес
         * из дампа разрешался в ttf_get_glyph_pair_kerning_width.
         * Наш урезанный шрифт сохранил таблицы GPOS, а разбор их
         * в stb_truetype, на котором стоит Tiny TTF, минимальный -
         * и уходит в долгий обход.
         *
         * Кернинг в этом интерфейсе не нужен вовсе: подписи короткие,
         * кегли крупные, разницы не видно. Кэш глифов оставляем
         * штатный. */
        lv_font_t *f = lv_tiny_ttf_create_data_ex(
                           wanted[i].data, wanted[i].size, wanted[i].px,
                           LV_FONT_KERNING_NONE,
                           CONFIG_LV_TINY_TTF_CACHE_GLYPH_CNT);
        if (!f) {
            /* Без шрифта интерфейс построить нельзя: LVGL уронит
             * первое же обращение к тексту. Лучше сказать об этом
             * внятно здесь, чем ловить исключение в отрисовке. */
            ESP_LOGE(TAG, "не создался шрифт %d px", (int) wanted[i].px);
            return ESP_ERR_NO_MEM;
        }
        *wanted[i].dst = f;
    }

    ESP_LOGI(TAG, "шрифты готовы: %u + %u байт в образе, одиннадцать кеглей",
             (unsigned) reg_size, (unsigned) bold_size);
    return ESP_OK;
}
