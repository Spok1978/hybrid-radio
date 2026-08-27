/*
 * ipradio_net.h — сеть: Wi-Fi через сопроцессор, время, каталог станций.
 *
 * У ESP32-P4 своего радио нет: Wi-Fi обслуживает ESP32-C6 по SDIO
 * (docs/26-firmware-spec.md, §2.4). Для приложения это выглядит как
 * обычный esp_wifi — компонент esp_wifi_remote подменяет реализацию
 * под тем же интерфейсом.
 *
 * Каталог Radio-Browser используется ТОЛЬКО как поисковик (§4).
 * Найденная станция сохраняется в прибор, дальше он играет из своей
 * базы: интернет нужен для потока, а не для каталога.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "ipradio_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IPRADIO_SSID_MAX      33
#define IPRADIO_PASS_MAX      65
#define IPRADIO_SEARCH_MAX    20   /**< сколько станций держим в выдаче */

/** Одна станция из каталога. */
typedef struct {
    char name[IPRADIO_NAME_MAX];
    char url[192];
    char uuid[40];
    char countrycode[4];   /**< поле country в API устарело, берём этот */
    char codec[12];
    uint16_t bitrate;
} ipradio_station_t;

/** Поднять сеть. Не блокирует: подключение идёт фоном, а состояние
 *  приезжает в автомат событием IPRADIO_EV_NET_STATE.
 *
 *  ВАЖНО: порядок вызовов внутри строгий — сопроцессор поднимается
 *  раньше всего, что трогает сетевой стек. */
esp_err_t ipradio_net_init(void);

/** Подключиться к сети. Пустой ssid означает «взять сохранённую». */
esp_err_t ipradio_net_connect(const char *ssid, const char *pass);

/** Текущее состояние, оно же лежит в снимке автомата. */
ipradio_net_state_t ipradio_net_state(void);

/** Запустить синхронизацию времени. Без сети просто ничего не делает. */
esp_err_t ipradio_net_start_sntp(const char *tz);

/** Поиск в каталоге по названию. Блокирующий, звать из своей задачи.
 *  Возвращает число найденных станций, записанных в out. */
int ipradio_net_search(const char *query,
                       ipradio_station_t *out, int max_items);

/** Отметить запуск станции в каталоге. Это счётчик кликов, по которому
 *  строится рейтинг: вежливость, а не обязанность (§4). */
void ipradio_net_report_click(const char *uuid);

#ifdef __cplusplus
}
#endif
