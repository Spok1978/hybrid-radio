/*
 * ipradio_mem.c — политика памяти и наблюдение за ней.
 *
 * Замысел — в ipradio_mem.h.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_rom_sys.h"

#include "ipradio_mem.h"

static const char *TAG = "mem";

static volatile uint32_t s_failed;
static volatile uint32_t s_json_allocs;

/* Неудачное выделение памяти.
 *
 * Зовётся из той задачи, где выделение не удалось, в том числе изнутри
 * драйверов. Поэтому печать ROM-функцией: обычный журнал сам может
 * попросить память, и отказ вызвал бы сам себя.
 *
 * Без этого отказы видны только по последствиям. 2026-10-05 рукопожатие
 * TLS падало с ошибкой 0x4290 - это нехватка памяти под длинную
 * арифметику, - а в журнале не было ни слова о памяти. */
static void on_alloc_failed(size_t size, uint32_t caps, const char *func)
{
    s_failed++;
    esp_rom_printf("ОТКАЗ ПАМЯТИ: %u байт, caps 0x%x, %s\n",
                   (unsigned) size, (unsigned) caps, func ? func : "?");
}

/* Узлы cJSON - в PSRAM.
 *
 * Каждый узел дерева и каждая строка в нём - отдельное выделение
 * меньше 256 байт, а такие по CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL идут
 * во внутреннюю память. Ответ каталога - это тысячи таких выделений
 * подряд, и они рассекают кучу как раз там, где её не хватает.
 *
 * К дереву обращаются только задача поиска и хранилище: ни прерываний,
 * ни DMA, так что PSRAM ему годится. Если PSRAM вдруг кончится, берём
 * обычной malloc - разбор не должен отказывать из-за места хранения.
 *
 * Освобождение обычной free(): в ESP-IDF она освобождает память
 * из любой кучи, в том числе выделенную heap_caps_malloc. Поэтому
 * строки, которые cJSON_PrintUnformatted отдаёт наружу и которые
 * хранилище освобождает free(), остаются правильными. */
static void *json_malloc(size_t size)
{
    s_json_allocs++;
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(size);
}

static void json_free(void *p)
{
    free(p);
}

void ipradio_mem_init(void)
{
    heap_caps_register_failed_alloc_callback(on_alloc_failed);

    cJSON_Hooks hooks = {
        .malloc_fn = json_malloc,
        .free_fn   = json_free,
    };
    cJSON_InitHooks(&hooks);

    /* Проверяем делом, а не верим настройке: разбираем крошечный
     * документ и смотрим, где оказался узел. */
    cJSON *probe = cJSON_Parse("[1]");
    bool in_psram = probe && esp_ptr_external_ram(probe);
    cJSON_Delete(probe);

    ESP_LOGI(TAG, "слежение за отказами памяти включено, "
                  "узлы JSON в PSRAM: %s", in_psram ? "да" : "НЕТ");
}

uint32_t ipradio_mem_failed_count(void)
{
    return s_failed;
}

uint32_t ipradio_mem_json_allocs(void)
{
    return s_json_allocs;
}
