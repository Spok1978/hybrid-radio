/* Заглушка esp_log.h: печатаем в stdout тем же уровнем, что и на плате.
 *
 * Уровни сохранены, чтобы вывод теста на машине читался так же,
 * как журнал с платы, и одно можно было сверять с другим. */
#pragma once

#include <stdio.h>

#define ESP_LOGE(tag, fmt, ...) printf("E (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("W (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) printf("I (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) ((void) 0)
#define ESP_LOGV(tag, fmt, ...) ((void) 0)
