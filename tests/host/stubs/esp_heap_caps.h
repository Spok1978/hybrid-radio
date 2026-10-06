/* Куча: обычные malloc и free, признаки памяти игнорируем.
 *
 * На плате они выбирают между внутренней памятью и PSRAM. На машине
 * память одна, и различать нечего. Проверить нехватку внутренней
 * памяти этим нельзя - только на плате. */
#pragma once

#include <stdlib.h>

#define MALLOC_CAP_8BIT      (1 << 2)
#define MALLOC_CAP_INTERNAL  (1 << 11)
#define MALLOC_CAP_SPIRAM    (1 << 10)
#define MALLOC_CAP_DMA       (1 << 3)
#define MALLOC_CAP_DEFAULT   (1 << 12)

static inline void *heap_caps_malloc(size_t n, uint32_t caps)
{
    (void) caps;
    return malloc(n);
}

static inline void *heap_caps_calloc(size_t n, size_t sz, uint32_t caps)
{
    (void) caps;
    return calloc(n, sz);
}

static inline void heap_caps_free(void *p) { free(p); }

static inline size_t heap_caps_get_free_size(uint32_t caps)
{
    (void) caps;
    return 1024 * 1024;
}
