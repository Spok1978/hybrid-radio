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
#define IPRADIO_SCAN_MAX      20   /**< сколько сетей показываем          */

/** Одна найденная сеть Wi-Fi. */
typedef struct {
    char    ssid[IPRADIO_SSID_MAX];
    int8_t  rssi;        /**< дБм, обычно от -30 до -90 */
    bool    open;        /**< без пароля                */
} ipradio_ap_t;

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

/** Подключиться к сети. Пустой ssid означает «взять сохранённую».
 *  Учётные данные сохраняются в NVS самим esp_wifi, отдельно
 *  их хранить не надо. */
esp_err_t ipradio_net_connect(const char *ssid, const char *pass);

/** Есть ли сохранённая сеть. По этому различаются два из трёх
 *  состояний недоступности (§5.2): «не настроен» и «нет связи». */
bool ipradio_net_has_credentials(void);

/** Имя сети, к которой подключаемся или подключены. Пустая строка,
 *  если сохранённой сети нет. Нужно диалогу 11: «нет связи с сетью N».
 *  Возвращает указатель на внутренний буфер, копировать сразу. */
const char *ipradio_net_ssid(void);

/** Поискать сети в эфире. Блокирующий, около двух секунд —
 *  звать из своей задачи, не из задачи интерфейса.
 *  Возвращает число найденных, записанных в out; отрицательное —
 *  ошибка. Дубликаты по имени убираются, остаётся сильнейший. */
int ipradio_net_scan(ipradio_ap_t *out, int max_items);

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

/** То же, но своей задачей и не дожидаясь ответа.
 *
 *  Звать ИМЕННО ЭТО из задачи интерфейса: запрос идёт с таймаутом
 *  восемь секунд, а у задачи интерфейса лимит молчания у сторожа -
 *  пять. Прямой вызов давал перезагрузку на медленном зеркале. */
void ipradio_net_report_click_async(const char *uuid);

#ifdef __cplusplus
}
#endif
