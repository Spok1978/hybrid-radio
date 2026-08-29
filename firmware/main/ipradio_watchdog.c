/*
 * ipradio_watchdog.c — надзор за долгоживущими задачами прошивки.
 *
 * Устройство и причины принятых решений — в шапке ipradio_watchdog.h,
 * здесь только детали реализации.
 */

#include "ipradio_watchdog.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"

static const char *TAG = "ipradio-wdt";

/* ------------------------------------------------------------------ *
 *  Таблица наблюдаемых задач
 * ------------------------------------------------------------------ */

/* Сколько задач берём под надзор: сейчас их пять, запас — на будущее. */
#define WDT_MAX_TASKS     8

/* Как часто надзиратель перепроверяет таблицу. Интервал много меньше
 * любого из лимитов молчания, поэтому две подряд неудачные проверки —
 * это уже закономерность, а не случайно невовремя сделанный замер. */
#define WDT_CHECK_MS      250

/* Сколько подряд проваленных проверок признаёт задачу зависшей:
 * защита от замера, попавшего в середину законной обработки. */
#define WDT_STRIKES_MAX   2

typedef struct {
    bool          in_use;
    const char   *name;      /* литерал, не копируем                 */
    TaskHandle_t  handle;    /* чтобы видеть состояние задачи        */
    TickType_t    quiet;     /* лимит молчания, в тиках              */
    uint32_t      flags;
    TickType_t    last_feed; /* тик последней отметки                */
    uint8_t       strikes;   /* подряд идущие проваленные проверки   */
} watched_task_t;

static watched_task_t s_tasks[WDT_MAX_TASKS];

/* Замок нужен только регистрации: она меняет несколько полей сразу.
 * Отметка (feed) обходится без него — это одна 32-разрядная запись,
 * на RISC-V она атомарна, а худшее, что даёт гонка с надзирателем, —
 * отметка, увиденная на одну проверку позже. Лишний критический
 * участок в цикле с периодом 5 мс дороже, чем эта задержка. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* Определена ниже, а ipradio_watchdog_init() её уже создаёт. */
static void watchdog_task(void *arg);

/* ------------------------------------------------------------------ *
 *  Регистрация и отметки
 * ------------------------------------------------------------------ */

esp_err_t ipradio_watchdog_init(void)
{
    memset(s_tasks, 0, sizeof(s_tasks));

    BaseType_t ok = xTaskCreate(watchdog_task, "ipradio_wdt",
                                3072, NULL, 5, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_FAIL, TAG,
                        "не удалось создать задачу надзирателя");
    ESP_LOGI(TAG, "надзор поднят");
    return ESP_OK;
}

int ipradio_watchdog_register(const char *name, uint32_t quiet_ms,
                              uint32_t flags)
{
    int id = -1;

    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < WDT_MAX_TASKS; i++) {
        if (s_tasks[i].in_use) {
            continue;
        }
        s_tasks[i].in_use    = true;
        s_tasks[i].name      = name;
        s_tasks[i].handle    = xTaskGetCurrentTaskHandle();
        s_tasks[i].quiet     = pdMS_TO_TICKS(quiet_ms);
        s_tasks[i].flags     = flags;
        /* Отметка при регистрации: иначе промежуток между стартом
         * задачи и первым витком её цикла считался бы молчанием. */
        s_tasks[i].last_feed = xTaskGetTickCount();
        s_tasks[i].strikes   = 0;
        id = i;
        break;
    }
    portEXIT_CRITICAL(&s_lock);

    if (id < 0) {
        ESP_LOGE(TAG, "таблица надзора полна, '%s' осталась без надзора",
                 name);
    }
    return id;
}

void ipradio_watchdog_feed(int id)
{
    if (id < 0 || id >= WDT_MAX_TASKS || !s_tasks[id].in_use) {
        return;
    }
    s_tasks[id].last_feed = xTaskGetTickCount();
}

/* ------------------------------------------------------------------ *
 *  Надзиратель
 * ------------------------------------------------------------------ */

/* Живая задача, которой нечего делать, висит в eBlocked (ждёт очередь
 * или задержку). eSuspended тоже считаем ожиданием: в прошивке никто
 * задачи не приостанавливает, а если когда-нибудь начнут — висящая
 * на приостановке задача зависнуть не может.
 *
 * Пометка «перепроверить на железе» отсюда снята: проверять нечего.
 * INCLUDE_eTaskGetState = 1 в FreeRTOSConfig.h самого ESP-IDF, это
 * видно в исходниках; а то, что xQueueReceive с portMAX_DELAY
 * переводит задачу в eBlocked, — определение состояния в FreeRTOS,
 * а не наблюдаемое поведение. Плата тут ничего не добавит.
 *
 * ЧЕГО ЭТОТ СПОСОБ НЕ ЛОВИТ, и это важнее: взаимную блокировку.
 * Задача, вставшая на мьютексе, который никто не отпустит, тоже
 * находится в eBlocked и будет засчитана живой. Флаг BLOCKED_OK
 * отличает «ждёт работу» от «крутится», но не отличает «ждёт работу»
 * от «ждёт вечно». Для автомата это приемлемо: он берёт единственный
 * свой мьютекс на короткое время и не вкладывает захваты. */
static bool is_waiting(const watched_task_t *t)
{
    eTaskState st = eTaskGetState(t->handle);
    return st == eBlocked || st == eSuspended;
}

static void watchdog_task(void *arg)
{
    (void) arg;

    /* Надзиратель — единственная задача, подписанная на штатный
     * сторож. PANIC выключен, поэтому зависание самого надзирателя
     * даст только предупреждения в журнале каждые 5 секунд — но это
     * лучше, чем совсем ничего, а перезагрузкой наблюдаемых задач
     * надзиратель занимается сам. */
    if (esp_task_wdt_add(NULL) != ESP_OK) {
        ESP_LOGW(TAG, "надзиратель не подписался на TWDT");
    }

    for (;;) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(WDT_CHECK_MS));

        TickType_t now = xTaskGetTickCount();

        for (int i = 0; i < WDT_MAX_TASKS; i++) {
            watched_task_t *t = &s_tasks[i];
            if (!t->in_use) {
                continue;
            }

            /* Разность беззнаковых тиков корректна и через переполнение
             * счётчика (раз в ~49 суток при 1000 Гц), пока молчание
             * короче половины диапазона — у нас оно секунды. */
            /* Задача могла отметиться уже после того, как снят
             * отсчёт now: тогда разность беззнаковых уйдёт в минус
             * и переполнится в огромное число. Одна такая проверка
             * дала бы ложную строку в журнале. Двух подряд не бывает,
             * до перезагрузки не доходит, но и строки лишней не надо. */
            if ((int32_t) (now - t->last_feed) < 0) {
                t->strikes = 0;
                continue;
            }

            TickType_t silent = now - t->last_feed;
            if (silent <= t->quiet) {
                t->strikes = 0;
                continue;
            }

            /* Задача молчит дольше лимита. Для автомата это само по
             * себе ничего не значит: он ждёт очередь без таймаута.
             * Смотрим состояние: заблокирована — живая, просто нечего
             * делать; крутится — подозрительно. */
            if ((t->flags & IPRADIO_WDT_F_BLOCKED_OK) && is_waiting(t)) {
                /* Ожидание засчитываем как жизнь, иначе исправный
                 * прибор, стоящий на полке без нажатий, ушёл бы
                 * в перезагрузку. */
                t->last_feed = now;
                t->strikes   = 0;
                continue;
            }

            /* Одна проверка могла попасть в середину законной
             * обработки события; вторая подряд — уже нет. */
            if (++t->strikes < WDT_STRIKES_MAX) {
                ESP_LOGW(TAG, "задача '%s' молчит %lu мс, перепроверяю",
                         t->name,
                         (unsigned long) (silent * portTICK_PERIOD_MS));
                continue;
            }

            ESP_LOGE(TAG, "задача '%s' не отзывается %lu мс — перезагрузка",
                     t->name,
                     (unsigned long) (silent * portTICK_PERIOD_MS));

            /* Пауза, чтобы строка успела уйти в UART до перезапуска. */
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();   /* не возвращается */
        }
    }
}
