/*
 * test_json_hooks.c — формат хранилища при своих функциях выделения
 * в cJSON.
 *
 * На плате с 2026-10-06 cJSON выделяет память своими функциями: узлы
 * уходят в PSRAM (ipradio_mem.c). Со своими функциями cJSON перестаёт
 * пользоваться realloc и растит буфер печати иначе: выделить новый,
 * скопировать, освободить старый (cJSON_InitHooks в cJSON.c). Это
 * другой путь кода, и самопроверка формата должна пройти и по нему.
 *
 * Здесь функции те же malloc и free, только со счётчиком: место
 * хранения на машине разработчика проверить нечем, это делает плата
 * (строка «узлы JSON в PSRAM» при загрузке).
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "cJSON.h"
#include "ipradio_selftest.h"

static unsigned s_allocs;

static void *count_malloc(size_t n)
{
    s_allocs++;
    return malloc(n);
}

static void count_free(void *p)
{
    free(p);
}

bool test_json_hooks_run(void)
{
    printf("W (hooks) === формат хранилища со своими выделениями cJSON ===\n");

    cJSON_Hooks hooks = { count_malloc, count_free };
    cJSON_InitHooks(&hooks);

    bool ok = ipradio_selftest_run();

    /* Вернуть как было: следующие проверки идут без подмены. */
    cJSON_InitHooks(NULL);

    printf("I (hooks) выделений через свои функции: %u\n", s_allocs);
    if (s_allocs == 0) {
        printf("E (hooks) ОТКАЗ: свои функции не вызывались, проверка пустая\n");
        return false;
    }
    return ok;
}
