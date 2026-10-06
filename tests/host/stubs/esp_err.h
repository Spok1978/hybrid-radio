/* Заглушка esp_err.h для сборки на машине разработчика.
 *
 * Модуль формата хранилища из esp_err использует ровно один тип
 * и две константы. Тащить ради них весь ESP-IDF незачем - он и
 * не соберётся под Windows. */
#pragma once

typedef int esp_err_t;

#define ESP_OK              0
#define ESP_FAIL           -1
#define ESP_ERR_NO_MEM      0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_FOUND   0x105
#define ESP_ERR_TIMEOUT     0x107
#define ESP_ERR_NOT_SUPPORTED 0x106
