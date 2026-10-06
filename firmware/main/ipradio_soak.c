/*
 * ipradio_soak.c — прогон на выносливость.
 *
 * Замысел — в ipradio_soak.h.
 */

#include "board_pins.h"

#if IPRADIO_DEBUG_SOAK

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "ipradio_mem.h"
#include "ipradio_net.h"
#include "ipradio_soak.h"
#include "ipradio_state.h"
#include "ipradio_storage.h"

static const char *TAG = "soak";

#define SOAK_SWITCHES       40      /* переключений за прогон            */
#define SOAK_PERIOD_MS      15000   /* сколько слушаем каждую станцию     */
#define SOAK_SEARCH_EVERY   5       /* каждое пятое - ещё и поиск         */

static void soak_task(void *arg)
{
    (void) arg;

    ipradio_snapshot_t s;
    ipradio_station_t *found = NULL;

    /* Ждём сеть: без неё интернет-режим не включится. */
    for (int i = 0; i < 90; i++) {
        ipradio_get(&s);
        if (s.net == IPRADIO_NET_CONNECTED) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (s.net != IPRADIO_NET_CONNECTED) {
        ESP_LOGE(TAG, "ПРОГОН: сети нет за 90 с, отменён");
        goto out;
    }

    int cells[IPRADIO_PRESET_MAX];
    int n = 0;
    for (int c = 1; c <= IPRADIO_PRESET_MAX; c++) {
        ipradio_preset_t p;
        if (ipradio_storage_get_preset(IPRADIO_MODE_NET, c, &p) && p.url[0]) {
            cells[n++] = c;
        }
    }
    if (n < 2) {
        ESP_LOGE(TAG, "ПРОГОН: в интернетном банке меньше двух станций");
        goto out;
    }

    found = heap_caps_malloc(sizeof(*found) * IPRADIO_SEARCH_MAX,
                             MALLOC_CAP_SPIRAM);

    ESP_LOGW(TAG, "ПРОГОН НАЧАТ: %d станций, %d переключений по %d с",
             n, SOAK_SWITCHES, SOAK_PERIOD_MS / 1000);

    for (int i = 1; i <= SOAK_SWITCHES; i++) {
        ipradio_get(&s);
        if (s.mode != IPRADIO_MODE_NET) {
            ipradio_post_simple(IPRADIO_EV_MODE_TOGGLE, 0);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        int cell = cells[i % n];
        ESP_LOGW(TAG, "ПРОГОН %d/%d: ячейка %d", i, SOAK_SWITCHES, cell);
        ipradio_post_simple(IPRADIO_EV_PRESET_PRESSED, cell);

        if (found && (i % SOAK_SEARCH_EVERY) == 0) {
            /* Поиск поверх играющей станции - так его делает человек. */
            vTaskDelay(pdMS_TO_TICKS(5000));
            uint32_t j0 = ipradio_mem_json_allocs();
            int k = ipradio_net_search("jazz", found, IPRADIO_SEARCH_MAX);
            ESP_LOGW(TAG, "ПРОГОН %d: каталог вернул %d, выделений JSON %" PRIu32,
                     i, k, ipradio_mem_json_allocs() - j0);
            vTaskDelay(pdMS_TO_TICKS(SOAK_PERIOD_MS - 5000));
        } else {
            vTaskDelay(pdMS_TO_TICKS(SOAK_PERIOD_MS));
        }
    }

    ESP_LOGW(TAG, "ПРОГОН ОКОНЧЕН: %d переключений без перезагрузки",
             SOAK_SWITCHES);

out:
    heap_caps_free(found);
    vTaskDeleteWithCaps(NULL);
}

void ipradio_soak_start(void)
{
    /* Стек в PSRAM, как у задачи поиска: тот же вызов каталога, те же
     * 12 КБ под HTTP и разбор JSON. */
    if (xTaskCreateWithCaps(soak_task, "soak", 12288, NULL, 3, NULL,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "ПРОГОН: задача не создалась");
    }
}

#endif /* IPRADIO_DEBUG_SOAK */
