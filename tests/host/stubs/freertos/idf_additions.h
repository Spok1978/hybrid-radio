/* Создание задачи с выбором памяти под стек: тоже пустышка. */
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static inline BaseType_t xTaskCreateWithCaps(TaskFunction_t f, const char *n,
                                             uint32_t stack, void *arg,
                                             UBaseType_t prio,
                                             TaskHandle_t *out, uint32_t caps)
{
    (void) caps;
    return xTaskCreate(f, n, stack, arg, prio, out);
}

static inline void vTaskDeleteWithCaps(TaskHandle_t t) { (void) t; }
