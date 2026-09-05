/*
 * ipradio_adf_stack.c — стеки задач ESP-ADF в PSRAM без патчей к ESP-IDF.
 *
 * ЗАЧЕМ
 *
 * Конвейер звука ADF поднимает четыре задачи: http_stream, декодер,
 * регулятор громкости и вывод I2S. Их стеки — 6144 + 3072 + 2048 + 3584,
 * почти пятнадцать килобайт. На этой плате внутренней памяти 445 КБ
 * всего, и в момент подключения к станции свободными оставались
 * единицы килобайт: рукопожатие TLS падало на `alloc(2389 bytes) failed`,
 * а драйвер SDIO — на нехватке буферов приёма.
 *
 * PSRAM при этом почти пуста: 30 МБ, занято меньше седьмой части.
 * Стеки этих задач могут лежать там: ни одна из них не работает
 * из обработчика прерывания, а при CONFIG_SPIRAM_XIP_FROM_PSRAM
 * на ESP32-P4 кеш не отключается даже на время записи флеша.
 *
 * ПОЧЕМУ ЭТО НЕ РАБОТАЛО САМО
 *
 * ADF умеет размещать стек во внешней памяти — у элемента есть поле
 * `stack_in_ext`. Но задачу с готовым буфером стека он создаёт функцией
 * `xTaskCreateRestrictedPinnedToCore`, а её в обычном ESP-IDF нет:
 * она появляется только вместе с патчами из `$ADF_PATH/idf_patches`.
 * Без них в журнале было:
 *
 *     E AUDIO_THREAD: Not found right xTaskCreateRestrictedPinnedToCore.
 *     E AUDIO_ELEMENT: [http] audio_thread_create failed
 *
 * — и ни один элемент не запускался.
 *
 * В самом ADF заглушка объявлена слабой (`__attribute__((weak))`,
 * audio_thread.c:43), так что достаточно определить функцию у себя:
 * компоновщик возьмёт нашу, патчи к ESP-IDF не нужны.
 *
 * ПОЧЕМУ ЗДЕСЬ ПУЛ, А НЕ ПРОСТО xTaskCreatePinnedToCoreWithCaps
 *
 * Напрашивается решение в три строки: освободить буфер, который выделил
 * ADF, и позвать штатный `xTaskCreatePinnedToCoreWithCaps`. Оно
 * НЕВЕРНО, и вот почему.
 *
 * Задачи, созданные через *WithCaps, положено удалять парной функцией
 * `vTaskDeleteWithCaps`. ADF же зовёт обычный `vTaskDelete(NULL)`
 * (audio_thread.c:110), а `audio_thread_cleanup()` у него — пустышка
 * с комментарием «TODO nothing» (audio_thread.c:102). Внутри ESP-IDF
 * про этот случай сказано прямо, в idf_additions.c:
 *
 *     the idle task will not free the task TCB and stack memories we
 *     created statically during xTaskCreateWithCaps()... Therefore,
 *     it will leak memory.
 *
 * А задачи элементов пересоздаются НЕ единожды: ADF поднимает их
 * на каждый audio_pipeline_run, то есть на каждое нажатие станции.
 * Замер по журналу платы: три запуска — три создания каждой из четырёх
 * задач. Значит, простой вариант терял бы пятнадцать килобайт PSRAM
 * и около килобайта внутренней памяти при каждом переключении
 * станции. Через полсотни нажатий прибор бы слёг — ровно от той
 * болезни, которую мы лечим.
 *
 * Поэтому память тут не выделяется и не освобождается вовсе. Четыре
 * элемента ADF имеют постоянные имена и постоянные размеры стеков,
 * так что каждому отводится свой слот, который переиспользуется при
 * каждом следующем запуске. После первого проигрывания расход
 * перестаёт расти совсем.
 *
 * ЧТО ОСТАЁТСЯ ВО ВНУТРЕННЕЙ ПАМЯТИ
 *
 * Управляющий блок задачи (TCB): планировщик трогает его в том числе
 * при выключенном кеше. Это около двухсот байт на задачу против
 * нескольких килобайт стека.
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "adf.stack";

/* Элементов конвейера четыре; шестой слот - запас на случай, если
 * в конвейер добавится ещё элемент. Больше не понадобится: слоты
 * не расходуются, а переиспользуются по имени задачи. */
#define SLOT_MAX  6
#define NAME_MAX  16

typedef struct {
    char          name[NAME_MAX];
    StackType_t  *stack;        /* PSRAM  */
    size_t        stack_bytes;
    StaticTask_t *tcb;          /* внутренняя память */
    bool          reused;       /* слот уже отдавали хотя бы раз */
} slot_t;

static slot_t s_slots[SLOT_MAX];

/* Найти слот по имени задачи или занять свободный. */
static slot_t *slot_for(const char *name)
{
    for (int i = 0; i < SLOT_MAX; i++) {
        if (s_slots[i].name[0] && strcmp(s_slots[i].name, name) == 0) {
            return &s_slots[i];
        }
    }
    for (int i = 0; i < SLOT_MAX; i++) {
        if (!s_slots[i].name[0]) {
            snprintf(s_slots[i].name, NAME_MAX, "%s", name);
            return &s_slots[i];
        }
    }
    return NULL;
}

BaseType_t xTaskCreateRestrictedPinnedToCore(
        const TaskParameters_t *const pxTaskDefinition,
        TaskHandle_t                 *pxCreatedTask,
        const BaseType_t              xCoreID)
{
    if (!pxTaskDefinition || !pxTaskDefinition->pcName) {
        return pdFAIL;
    }

    /* Буфер, выделенный ADF, нам не нужен: своим мы владеем сами
     * и переиспользуем. Освобождаем сразу, иначе он утечёт - ADF
     * его не освобождает никогда (audio_thread_cleanup - пустышка). */
    if (pxTaskDefinition->puxStackBuffer) {
        heap_caps_free(pxTaskDefinition->puxStackBuffer);
    }

    const size_t want = (size_t) pxTaskDefinition->usStackDepth;

    slot_t *sl = slot_for(pxTaskDefinition->pcName);
    if (!sl) {
        ESP_LOGE(TAG, "слоты кончились, задача %s",
                 pxTaskDefinition->pcName);
        return pdFAIL;
    }

    /* Стек - в PSRAM. Перевыделяем, только если запросили больше,
     * чем в прошлый раз; обычно размер один и тот же. */
    if (!sl->stack || sl->stack_bytes < want) {
        heap_caps_free(sl->stack);
        sl->stack = heap_caps_malloc(want, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        sl->stack_bytes = sl->stack ? want : 0;
    }
    if (!sl->tcb) {
        sl->tcb = heap_caps_malloc(sizeof(StaticTask_t),
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!sl->stack || !sl->tcb) {
        ESP_LOGE(TAG, "нет памяти под задачу %s", sl->name);
        return pdFAIL;
    }

    /* Предыдущая задача из этого слота уже сказала vTaskDelete(NULL),
     * но убирает её за собой задача простоя, и до этого момента её
     * TCB ещё числится в списке ожидающих удаления. Переиспользовать
     * ту же память раньше - значит порвать этот список. Двух тактов
     * простою хватает с запасом; ждём только при повторном заходе. */
    if (sl->reused) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    sl->reused = true;

    /* Бит привилегии относится к сборкам с MPU и к приоритету
     * отношения не имеет - снимаем. */
    UBaseType_t prio = pxTaskDefinition->uxPriority & ~portPRIVILEGE_BIT;

    /* Глубина стека здесь в байтах: столько же ADF выделял под буфер
     * (audio_calloc(1, stack)), и столько же ждёт ESP-IDF - в его
     * редакции FreeRTOS размер стека задаётся в байтах, не в словах. */
    TaskHandle_t h = xTaskCreateStaticPinnedToCore(
                         pxTaskDefinition->pvTaskCode,
                         sl->name,
                         want,
                         pxTaskDefinition->pvParameters,
                         prio,
                         sl->stack,
                         sl->tcb,
                         xCoreID);
    if (!h) {
        ESP_LOGE(TAG, "не удалось создать задачу %s", sl->name);
        return pdFAIL;
    }

    if (pxCreatedTask) {
        *pxCreatedTask = h;
    }

    ESP_LOGI(TAG, "%s: стек %u байт в PSRAM", sl->name, (unsigned) want);
    return pdPASS;
}
