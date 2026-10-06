/* Мьютексы: пустышки.
 *
 * Тест идёт в один поток, блокироваться не от кого. Проверить
 * правильность блокировок этим НЕЛЬЗЯ - для этого нужна плата
 * и настоящая одновременность. */
#pragma once

#include "freertos/FreeRTOS.h"

typedef void *SemaphoreHandle_t;

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    static int one;
    return &one;               /* не NULL, больше от него ничего не надо */
}

static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t)
{
    (void) s; (void) t;
    return pdTRUE;
}

static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
    (void) s;
    return pdTRUE;
}
