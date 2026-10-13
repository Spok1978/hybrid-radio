/*
 * ipradio_fmprobe.c — проверка приёма эфира на столе.
 *
 * Только для отладки, ключ IPRADIO_DEBUG_FM_PROBE в board_pins.h.
 * Через несколько секунд после загрузки проходит весь FM-диапазон
 * аппаратным поиском тюнера, затем на каждой найденной станции стоит
 * несколько секунд и пишет в журнал уровень сигнала. Имя станции
 * из RDS тюнер пишет сам («tuner: RDS: …»), если станция его передаёт.
 *
 * Звука при этом нет и быть не должно, если выходы тюнера никуда
 * не подключены: проверяется только приём.
 *
 * Ход виден по строкам «ПРИЁМ».
 */

#include "board_pins.h"

#if IPRADIO_DEBUG_FM_PROBE

#include <inttypes.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "ipradio_fmprobe.h"
#include "ipradio_tuner.h"

static const char *TAG = "fmprobe";

#define PROBE_MAX        48      /* столько засечек держит и экран настройки */
#define PROBE_DWELL_MS   4000    /* сколько стоим на станции: RDS успевает   */
/* После прохода встать сюда, а не на сильнейшую (0 - на сильнейшую).
 * 96,2 МГц - станция, у которой 2026-10-14 точно шёл RDS. */
#define PROBE_STAY_KHZ   106200

static uint32_t      s_found[PROBE_MAX];
static volatile int  s_count;
static volatile bool s_done;

/* Зовётся из задачи поиска тюнера, см. ipradio_tuner.h. */
static void on_scan(uint32_t freq_khz, bool done, void *ctx)
{
    (void) ctx;
    if (done) {
        s_done = true;
        return;
    }
    if (s_count < PROBE_MAX) {
        s_found[s_count++] = freq_khz;
    }
}

static void probe_task(void *arg)
{
    (void) arg;

    /* Даём тюнеру и мосту подняться. */
    vTaskDelay(pdMS_TO_TICKS(5000));

    if (!ipradio_tuner_present()) {
        ESP_LOGE(TAG, "ПРИЁМ: тюнера нет на шине, проверять нечего");
        goto out;
    }

    ESP_LOGW(TAG, "ПРИЁМ: проход по диапазону");
    if (ipradio_tuner_scan_start(on_scan, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "ПРИЁМ: проход не запустился");
        goto out;
    }
    for (int i = 0; i < 600 && !s_done; i++) {       /* не дольше минуты */
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!s_done) {
        ipradio_tuner_scan_abort();
        ESP_LOGW(TAG, "ПРИЁМ: проход не уложился в минуту, прерван");
    }

    ESP_LOGW(TAG, "ПРИЁМ: найдено станций %d", s_count);

    int best = -1;
    uint8_t best_sig = 0;
    for (int i = 0; i < s_count; i++) {
        ipradio_tuner_set_freq(s_found[i]);
        vTaskDelay(pdMS_TO_TICKS(PROBE_DWELL_MS));
        uint8_t sig = ipradio_tuner_signal();
        ESP_LOGW(TAG, "ПРИЁМ: %2d. %" PRIu32 ".%02" PRIu32 " МГц, сигнал %u %%",
                 i + 1, s_found[i] / 1000, (s_found[i] % 1000) / 10,
                 (unsigned) sig);
        if (sig > best_sig) {
            best_sig = sig;
            best = i;
        }
    }

    if (PROBE_STAY_KHZ) {
        ipradio_tuner_set_freq(PROBE_STAY_KHZ);
        ESP_LOGW(TAG, "ПРИЁМ: стою на %u кГц, слушаю RDS",
                 (unsigned) PROBE_STAY_KHZ);
    } else if (best >= 0) {
        ipradio_tuner_set_freq(s_found[best]);
        ESP_LOGW(TAG, "ПРИЁМ: остаюсь на сильнейшей, %" PRIu32 ".%02" PRIu32
                      " МГц (%u %%)",
                 s_found[best] / 1000, (s_found[best] % 1000) / 10,
                 (unsigned) best_sig);
    }
    ESP_LOGW(TAG, "ПРИЁМ: проверка окончена");

out:
    vTaskDeleteWithCaps(NULL);
}

void ipradio_fmprobe_start(void)
{
    if (xTaskCreateWithCaps(probe_task, "fmprobe", 4096, NULL, 3, NULL,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "ПРИЁМ: задача не создалась");
    }
}

#endif /* IPRADIO_DEBUG_FM_PROBE */
