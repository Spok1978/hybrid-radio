/* Задачи: не создаются вовсе.
 *
 * Это главное отличие теста от платы. На плате автомат крутится
 * в своей задаче и сам разбирает очередь; в тесте мы вызываем
 * разбор вручную (ipradio_state_pump), чтобы после каждого события
 * проверить состояние. */
#pragma once

#include "freertos/FreeRTOS.h"

typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

static inline BaseType_t xTaskCreate(TaskFunction_t f, const char *n,
                                     uint32_t stack, void *arg,
                                     UBaseType_t prio, TaskHandle_t *out)
{
    (void) f; (void) n; (void) stack; (void) arg; (void) prio;
    if (out) {
        *out = NULL;
    }
    return pdPASS;
}

static inline void vTaskDelay(TickType_t t) { (void) t; }
static inline void vTaskDelete(TaskHandle_t t) { (void) t; }
