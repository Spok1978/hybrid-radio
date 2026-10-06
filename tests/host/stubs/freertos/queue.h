/* Очередь: кольцевой буфер фиксированного размера.
 *
 * Ровно то, чем очередь и является, за вычетом блокировок
 * и ожидания. Ждать в тесте некого: события кладёт и забирает
 * один и тот же поток. */
#pragma once

#include "freertos/FreeRTOS.h"

#include <stdlib.h>

typedef struct {
    unsigned char *buf;
    size_t item;
    size_t cap;
    size_t head;
    size_t count;
} fake_queue_t;

typedef fake_queue_t *QueueHandle_t;

static inline QueueHandle_t xQueueCreate(size_t len, size_t item_size)
{
    fake_queue_t *q = calloc(1, sizeof(*q));
    if (!q) {
        return NULL;
    }
    q->buf  = calloc(len, item_size);
    q->item = item_size;
    q->cap  = len;
    return q;
}

static inline BaseType_t xQueueSend(QueueHandle_t q, const void *src,
                                    TickType_t wait)
{
    (void) wait;
    if (!q || q->count >= q->cap) {
        return pdFALSE;          /* переполнение - как на плате */
    }
    size_t tail = (q->head + q->count) % q->cap;
    memcpy(q->buf + tail * q->item, src, q->item);
    q->count++;
    return pdTRUE;
}

static inline BaseType_t xQueueReceive(QueueHandle_t q, void *dst,
                                       TickType_t wait)
{
    (void) wait;
    if (!q || q->count == 0) {
        return pdFALSE;
    }
    memcpy(dst, q->buf + q->head * q->item, q->item);
    q->head = (q->head + 1) % q->cap;
    q->count--;
    return pdTRUE;
}
