/*
 * ipradio_net.c — Wi-Fi через сопроцессор, время и каталог станций.
 *
 * Правила обращения к Radio-Browser, которые сервис требует соблюдать
 * (docs/26-firmware-spec.md, §4):
 *
 *   - список зеркал берётся ЧЕРЕЗ DNS: A-записи all.api.radio-browser.info;
 *     сервер выбирается случайно, при отказе переключаемся на другой;
 *   - обязателен внятный User-Agent — это требование документации,
 *     а не пожелание;
 *   - поле country в ответах устарело, использовать countrycode;
 *   - при запуске станции полагается дёрнуть счётчик кликов.
 *
 * Случайный выбор зеркала — не про надёжность нашего прибора, а про
 * то, чтобы не бить всегда в один и тот же общественный сервер.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"

#include "esp_event.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "cJSON.h"

#include "ipradio_net.h"
#include "ipradio_state.h"

static const char *TAG = "net";

/* Требование сервиса: внятный User-Agent с именем и версией. */
#define USER_AGENT        "ipradio/0.1"

/* Зеркала каталога.
 *
 * ПЕРВЫМ идёт all.api.radio-browser.info - имя, за которым сервис
 * сам держит список живых зеркал через DNS. Так и рекомендует
 * документация: «Never use a direct link to a single new server.
 * It is much better to get a list of the servers», имена серверов
 * меняются.
 *
 * Раньше это имя стояло в списке отвергнутых. В коде было записано,
 * что оно «живёт только в DNS как указатель на зеркала, и ни одно
 * зеркало не обслуживает его как виртуальный хост». Это оказалось
 * неверно, и вывод был сделан из другого опыта: тогда соединялись
 * по IP и подставляли Host вручную. Вот ЭТО и не работает. Если же
 * ходить по имени и дать клиенту подставить Host самому, всё
 * отвечает - проверено 2026-09-06 запросом /json/stations/byname.
 *
 * Почему это важно. В прежнем списке из четырёх имён nl1 и at1
 * УЖЕ НЕ СУЩЕСТВУЮТ - их нет в DNS вовсе (проверено там же).
 * А зеркало выбиралось случайно, то есть каждый второй поиск
 * station уходил в никуда и человек видел «ничего не нашлось»
 * при исправной сети. de1 и de2 при этом указывают на один и тот
 * же адрес, что и all.
 *
 * Оставшиеся два - подстраховка на случай, если сломается сам
 * указатель. Идём по порядку, а не вразнобой: раскладывать
 * нагрузку между зеркалами - забота DNS, а не наша. */
static const char *const MIRRORS[] = {
    "all.api.radio-browser.info",
    "de1.api.radio-browser.info",
    "de2.api.radio-browser.info",
};
#define MIRROR_MAX        (sizeof(MIRRORS) / sizeof(MIRRORS[0]))
#define HTTP_TIMEOUT_MS   8000
#define RESPONSE_MAX      (48 * 1024)

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define RECONNECT_DELAY_MS  5000

static EventGroupHandle_t   s_wifi_events;
static ipradio_net_state_t  s_state = IPRADIO_NET_NOT_CONFIGURED;
static bool                 s_have_credentials;
static bool                 s_wifi_started;
static char                 s_mirror[64];   /* выбранное зеркало */

/* ------------------------------------------------------------------ *
 *  Состояние
 * ------------------------------------------------------------------ */

static void set_state(ipradio_net_state_t st)
{
    if (s_state == st) {
        return;
    }
    s_state = st;
    ipradio_post_simple(IPRADIO_EV_NET_STATE, (int32_t) st);
}

ipradio_net_state_t ipradio_net_state(void)
{
    return s_state;
}

/* ------------------------------------------------------------------ *
 *  Wi-Fi
 * ------------------------------------------------------------------ */

static char *fetch_ex(const char *path, bool post);
static char *fetch(const char *path);
static char *fetch_post(const char *path);

static esp_timer_handle_t s_reconnect_timer;
static bool               s_auth_failed;

static void reconnect_cb(void *arg)
{
    (void) arg;
    esp_wifi_connect();
}

static void wifi_handler(void *arg, esp_event_base_t base,
                         int32_t id, void *data)
{
    (void) arg; (void) data;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        set_state(IPRADIO_NET_CONNECTING);
        esp_wifi_connect();

    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        /* Переподключаемся сами и без ограничения на число попыток:
         * прибор стоит на полке, роутер может быть выключен часами,
         * и «сдаться навсегда» здесь неправильно. Пауза нужна, чтобы
         * не выжигать эфир при выключенной точке доступа. */
        set_state(IPRADIO_NET_DISCONNECTED);
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);

        /* Пауза делается ТАЙМЕРОМ, а не сном прямо здесь.
         *
         * Обработчик исполняется в задаче системного цикла событий.
         * Уснув в нём на пять секунд, мы останавливаем весь цикл:
         * получение адреса, события SNTP и всех прочих подписчиков.
         * Раньше так и было. */
        const wifi_event_sta_disconnected_t *d = data;
        if (d && (d->reason == WIFI_REASON_AUTH_FAIL ||
                  d->reason == WIFI_REASON_NO_AP_FOUND ||
                  d->reason == WIFI_REASON_HANDSHAKE_TIMEOUT)) {
            /* Пароль не подошёл или сети нет. Повторять бессмысленно
             * до смены учётных данных, а молчать нельзя: человек так
             * и не узнает, почему не подключается. */
            ESP_LOGE(TAG, "не подключиться, причина %d — нужен ввод заново",
                     (int) d->reason);
            s_auth_failed = true;
            set_state(IPRADIO_NET_NOT_CONFIGURED);
            return;
        }

        esp_timer_start_once(s_reconnect_timer,
                             (uint64_t) RECONNECT_DELAY_MS * 1000);

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *) data;
        ESP_LOGI(TAG, "адрес получен: " IPSTR, IP2STR(&ev->ip_info.ip));
        set_state(IPRADIO_NET_CONNECTED);
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

esp_err_t ipradio_net_init(void)
{
    const esp_timer_create_args_t rc = {
        .callback = reconnect_cb,
        .name     = "wifi_reconnect",
    };
    ESP_ERROR_CHECK(esp_timer_create(&rc, &s_reconnect_timer));

    s_wifi_events = xEventGroupCreate();
    if (!s_wifi_events) {
        return ESP_ERR_NO_MEM;
    }

    /* Порядок ниже важен и продиктован работой через сопроцессор:
     * хранилище настроек, стек интерфейсов, цикл событий, и только
     * потом сам Wi-Fi. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    /* Настройки хранятся в NVS самим драйвером. Если там что-то есть,
     * поднимаемся сразу; если нет — ждём, пока пользователь настроит. */
    wifi_config_t wc;
    if (esp_wifi_get_config(WIFI_IF_STA, &wc) == ESP_OK &&
        wc.sta.ssid[0] != '\0') {
        s_have_credentials = true;
        ESP_LOGI(TAG, "сохранённая сеть: %s", (char *) wc.sta.ssid);
        ESP_ERROR_CHECK(esp_wifi_start());
        s_wifi_started = true;
    } else {
        ESP_LOGW(TAG, "сеть не настроена — интернет-станции недоступны");
        set_state(IPRADIO_NET_NOT_CONFIGURED);
    }

    return ESP_OK;
}

esp_err_t ipradio_net_connect(const char *ssid, const char *pass)
{
    if (ssid && ssid[0]) {
        wifi_config_t wc = { 0 };
        /* Поля драйвера - не строки Си: SSID бывает ровно 32 знака,
         * ключ WPA - ровно 64 шестнадцатеричных, и завершающего нуля
         * в поле тогда нет (wifi_sta_config_t в esp_wifi_types).
         * Прежнее копирование «размер минус один» отрезало последний
         * знак у таких имён, и подключиться было нельзя. */
        memcpy(wc.sta.ssid, ssid, strnlen(ssid, sizeof(wc.sta.ssid)));
        if (pass) {
            memcpy(wc.sta.password, pass,
                   strnlen(pass, sizeof(wc.sta.password)));
        }
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
        s_have_credentials = true;
        /* Новые учётные данные - новая попытка: прошлый отказ
         * авторизации больше не запрещает подключаться. */
        s_auth_failed = false;
    }

    if (!s_have_credentials) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_wifi_stop();
    esp_err_t err = esp_wifi_start();
    s_wifi_started = (err == ESP_OK);
    return err;
}

/* ------------------------------------------------------------------ *
 *  Поиск сетей
 * ------------------------------------------------------------------ */

bool ipradio_net_has_credentials(void)
{
    return s_have_credentials;
}

void ipradio_net_ssid(char *out, size_t cap)
{
    if (!out || cap == 0) {
        return;
    }
    out[0] = '\0';

    /* Имя берём у драйвера, а не храним своё: он и так его держит
     * в NVS, и две копии рано или поздно разошлись бы. */
    wifi_config_t wc;
    if (esp_wifi_get_config(WIFI_IF_STA, &wc) == ESP_OK) {
        /* Не "%s": в 32-знаковом имени нуля нет, и печать ушла бы
         * дальше, в поле пароля. */
        snprintf(out, cap, "%.*s",
                 (int) strnlen((const char *) wc.sta.ssid, sizeof(wc.sta.ssid)),
                 (const char *) wc.sta.ssid);
    }
}

int ipradio_net_scan(ipradio_ap_t *out, int max_items)
{
    if (!out || max_items <= 0) {
        return -1;
    }

    /* Драйвер мог быть и не запущен: при ненастроенной сети мы его
     * не поднимаем, чтобы не гонять радио впустую. А сканировать
     * без запущенного драйвера нельзя - поднимаем здесь.
     * Останавливать обратно не надо: человек пришёл настраивать
     * сеть, значит через минуту она понадобится. */
    if (!s_wifi_started) {
        esp_err_t err = esp_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Wi-Fi не запустился: %s", esp_err_to_name(err));
            return -1;
        }
        s_wifi_started = true;
    }

    /* Активное сканирование: посылаем запросы, а не ждём маяков.
     * Пассивное надёжнее для скрытых сетей, но занимает секунды
     * на каждый канал, и человек успевает решить, что прибор завис. */
    wifi_scan_config_t cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,              /* все каналы */
        .show_hidden = false,      /* к скрытой всё равно не подключиться
                                      без ручного ввода имени */
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = { .min = 60, .max = 150 },
    };

    esp_err_t err = esp_wifi_scan_start(&cfg, true /* блокирующе */);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "сканирование не пошло: %s", esp_err_to_name(err));
        return -1;
    }

    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    if (found == 0) {
        return 0;
    }

    /* Просим у драйвера больше, чем покажем: одна и та же сеть
     * приходит по разу на каждую точку доступа, и после склейки
     * дубликатов список заметно короче исходного. */
    uint16_t want = found;
    if (want > IPRADIO_SCAN_MAX * 3) {
        want = IPRADIO_SCAN_MAX * 3;
    }

    wifi_ap_record_t *recs = calloc(want, sizeof(*recs));
    if (!recs) {
        esp_wifi_clear_ap_list();
        return -1;
    }

    err = esp_wifi_scan_get_ap_records(&want, recs);
    if (err != ESP_OK) {
        free(recs);
        return -1;
    }

    /* Склейка дубликатов. Драйвер отдаёт список, отсортированный
     * по убыванию уровня, поэтому первая встреченная запись сети -
     * она же сильнейшая, и достаточно пропускать последующие. */
    int n = 0;
    for (uint16_t i = 0; i < want && n < max_items; i++) {
        const char *ssid = (const char *) recs[i].ssid;
        if (!ssid[0]) {
            continue;             /* скрытая: показывать нечего */
        }

        bool seen = false;
        for (int j = 0; j < n; j++) {
            if (strcmp(out[j].ssid, ssid) == 0) {
                seen = true;
                break;
            }
        }
        if (seen) {
            continue;
        }

        snprintf(out[n].ssid, sizeof(out[n].ssid), "%s", ssid);
        out[n].rssi = recs[i].rssi;
        out[n].open = (recs[i].authmode == WIFI_AUTH_OPEN);
        n++;
    }

    free(recs);
    esp_wifi_clear_ap_list();

    ESP_LOGI(TAG, "найдено сетей: %u, показываем %d", (unsigned) found, n);
    return n;
}

/* ------------------------------------------------------------------ *
 *  Время
 * ------------------------------------------------------------------ */

esp_err_t ipradio_net_start_sntp(const char *tz)
{
    if (tz && tz[0]) {
        setenv("TZ", tz, 1);
        tzset();
    }

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    /* Не ждём результата: часы подтянутся сами, когда сеть появится,
     * а прибор должен играть эфир уже сейчас. */
    cfg.start = true;
    cfg.sync_cb = NULL;

    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err == ESP_ERR_INVALID_STATE) {
        err = ESP_OK;     /* уже запущен */
    }
    return err;
}

/* ------------------------------------------------------------------ *
 *  Выбор зеркала каталога
 * ------------------------------------------------------------------ */

/* Выбрать зеркало. Случайно, как просит документация сервиса:
 * нагрузка должна расходиться по зеркалам сама. */
static bool pick_mirror(void)
{
    /* По кругу, начиная с первого. Номер сохраняем между вызовами:
     * зеркало сбрасывается только при отказе, и тогда надо взять
     * СЛЕДУЮЩЕЕ, а не то же самое снова. */
    static size_t next;

    const char *m = MIRRORS[next % MIRROR_MAX];
    next++;

    snprintf(s_mirror, sizeof(s_mirror), "%s", m);
    ESP_LOGI(TAG, "зеркало каталога: %s", s_mirror);
    return true;
}

/* ------------------------------------------------------------------ *
 *  Запросы к каталогу
 * ------------------------------------------------------------------ */

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} response_t;

static esp_err_t http_collect(esp_http_client_event_t *evt)
{
    response_t *r = (response_t *) evt->user_data;

    if (evt->event_id != HTTP_EVENT_ON_DATA || !r) {
        return ESP_OK;
    }
    if (r->len + evt->data_len >= r->cap) {
        /* Ответ не влез. Раньше это происходило молча: буфер
         * обрезался, разбор JSON падал, и человек видел «ничего
         * не нашлось» вместо объяснения. */
        ESP_LOGW(TAG, "ответ каталога больше %d КБ, обрезан",
                 RESPONSE_MAX / 1024);
        return ESP_OK;      /* ответ длиннее, чем мы готовы принять */
    }
    memcpy(r->buf + r->len, evt->data, evt->data_len);
    r->len += evt->data_len;
    r->buf[r->len] = '\0';
    return ESP_OK;
}

static char *fetch_ex(const char *path, bool post)
{
    if (s_state != IPRADIO_NET_CONNECTED) {
        return NULL;
    }
    if (s_mirror[0] == '\0' && !pick_mirror()) {
        return NULL;
    }

    /* Каталог - по https (2026-10-08): ответ каталога решает, какой
     * адрес ляжет в ячейку и куда прибор пойдёт за звуком, и подменить
     * его по дороге без шифрования ничего не стоит. Прежде здесь был
     * http ради внутренней памяти: рукопожатие TLS её съедало. С переносом
     * mbedTLS в PSRAM (docs/34-memory.md) этот довод отпал. Сертификаты
     * у всех трёх зеркал действительны (проверено 2026-10-08), корни -
     * из встроенного набора ESP-IDF (MBEDTLS_CERTIFICATE_BUNDLE). */
    char url[256];
    snprintf(url, sizeof(url), "https://%s%s", s_mirror, path);

    response_t r = {
        .buf = calloc(1, RESPONSE_MAX),
        .len = 0,
        .cap = RESPONSE_MAX,
    };
    if (!r.buf) {
        return NULL;
    }

    esp_http_client_config_t cfg = {
        .url           = url,
        .event_handler = http_collect,
        .user_data     = &r,
        .timeout_ms    = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        free(r.buf);
        return NULL;
    }

    /* Host не ставим: адрес именованный, клиент подставит сам.
     * Ручная подстановка здесь была причиной того, что каталог
     * не отвечал вовсе. */
    esp_http_client_set_header(cli, "User-Agent", USER_AGENT);
    if (post) {
        esp_http_client_set_method(cli, HTTP_METHOD_POST);
    }

    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "запрос не удался (%s, код %d) — сменим зеркало",
                 esp_err_to_name(err), status);
        s_mirror[0] = '\0';     /* при отказе переключаемся на другое */
        free(r.buf);
        return NULL;
    }

    return r.buf;
}

/* Экранирование для строки запроса: пробелы и всё небезопасное
 * переводим в проценты, иначе поиск по названию с пробелом сломается. */
static void url_escape(const char *src, char *dst, size_t dst_size)
{
    static const char *hex = "0123456789ABCDEF";
    size_t j = 0;

    for (size_t i = 0; src[i] && j + 4 < dst_size; i++) {
        unsigned char c = (unsigned char) src[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
            dst[j++] = (char) c;
        } else {
            dst[j++] = '%';
            dst[j++] = hex[c >> 4];
            dst[j++] = hex[c & 0x0F];
        }
    }
    dst[j] = '\0';
}

static void json_str(const cJSON *o, const char *k, char *dst, size_t n)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsString(it) && it->valuestring) {
        strncpy(dst, it->valuestring, n - 1);
        dst[n - 1] = '\0';
    }
}

/* Один запрос к каталогу. Разбит отдельно, потому что при неудаче
 * мы пробуем ещё раз с другой строкой. */
static int search_once(const char *query,
                       ipradio_station_t *out, int max_items);

/* Убрать ведущее «радио» или «radio».
 *
 * Поиск в каталоге идёт ПО ПОДСТРОКЕ и в том же порядке, а не по
 * словам. Человек пишет «радио silver rain», а станция называется
 * «Silver Rain Radio» - такой подстроки в имени нет, и находится
 * ноль. Слово «радио» люди приписывают почти всегда, и почти всегда
 * оно стоит не там, где в имени станции.
 *
 * Возвращает true, если было что убирать. */
static bool strip_radio_word(const char *src, char *dst, size_t n)
{
    static const char *const words[] = { "радио ", "radio ", "Радио ",
                                         "Radio " };
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        size_t len = strlen(words[i]);
        if (strncmp(src, words[i], len) == 0 && src[len]) {
            snprintf(dst, n, "%s", src + len);
            return true;
        }
    }
    return false;
}

/* Самое длинное слово запроса: последняя попытка, когда и без
 * «радио» ничего нет. Длинное слово - самое отличительное. */
static bool longest_word(const char *src, char *dst, size_t n)
{
    const char *best = NULL;
    size_t best_len = 0;
    const char *p = src;

    while (*p) {
        while (*p == ' ') p++;
        const char *w = p;
        while (*p && *p != ' ') p++;
        size_t len = (size_t) (p - w);
        if (len > best_len) {
            best_len = len;
            best = w;
        }
    }

    if (!best || best_len == 0 || best_len == strlen(src)) {
        return false;      /* одно слово - пробовать нечего */
    }
    if (best_len >= n) {
        best_len = n - 1;
    }
    memcpy(dst, best, best_len);
    dst[best_len] = 0;
    return true;
}

int ipradio_net_search(const char *query,
                       ipradio_station_t *out, int max_items)
{
    if (!query || !out || max_items <= 0) {
        return 0;
    }

    int n = search_once(query, out, max_items);
    if (n < 0) {
        /* Отказ связи - ещё раз. fetch_ex при отказе сбросил зеркало,
         * и pick_mirror возьмёт СЛЕДУЮЩЕЕ. Прогон 2026-10-06: один поиск
         * из восьми кончился таймаутом и выдал «ничего не нашлось»,
         * хотя второе зеркало ответило бы. Один повтор, а не больше:
         * каждый отказ - до HTTP_TIMEOUT_MS ожидания под надписью
         * «Ищем…». */
        ESP_LOGW(TAG, "каталог не ответил, пробую другое зеркало");
        n = search_once(query, out, max_items);
    }
    if (n != 0) {
        return n;           /* нашлось или связи нет вовсе */
    }

    /* Не нашлось целиком - пробуем без слова «радио». */
    char alt[64];
    if (strip_radio_word(query, alt, sizeof(alt))) {
        ESP_LOGI(TAG, "ничего; пробую «%s»", alt);
        n = search_once(alt, out, max_items);
        if (n > 0) {
            return n;
        }
    }

    /* И напоследок - по самому длинному слову. */
    if (longest_word(query, alt, sizeof(alt))) {
        ESP_LOGI(TAG, "ничего; пробую «%s»", alt);
        n = search_once(alt, out, max_items);
    }
    return n;
}

/* Выбрать адрес потока, по возможности без TLS.
 *
 * Каталог отдаёт два поля: url - как его подал владелец станции,
 * url_resolved - тот же адрес после всех переходов. Обычно берут
 * второй, он точнее.
 *
 * Но рукопожатие TLS на этой плате стоит десятки килобайт ВНУТРЕННЕЙ
 * памяти, которой у нас сорок, и оно уже валилось на
 * mbedtls_ssl_setup returned -0x7F00. Звук от смены схемы не меняется:
 * это тот же поток, только канал без шифрования. Поэтому если хоть
 * один из двух адресов обычный http - берём его.
 *
 * Если оба на https, берём url_resolved, как и раньше: выбора нет,
 * и пусть решает mbedTLS. */
static bool is_plain_http(const char *u)
{
    return strncmp(u, "http://", 7) == 0;
}

/* Адрес из ответа каталога, только если он целиком влезает в cap.
 *
 * Обрезанный адрес не играет, а записался бы в ячейку как исправный:
 * человек увидел бы «станция не отвечает» про живую станцию. Длинный
 * адрес считаем отсутствующим - станция не попадёт в выдачу вовсе
 * (search_once пропускает станции без адреса). */
static void json_url(const cJSON *o, const char *k, char *dst, size_t cap)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
    dst[0] = '\0';
    if (cJSON_IsString(it) && it->valuestring &&
        strlen(it->valuestring) < cap) {
        memcpy(dst, it->valuestring, strlen(it->valuestring) + 1);
    }
}

static void pick_url(const cJSON *el, char *dst, size_t cap)
{
    char resolved[192] = { 0 };
    char raw[192]      = { 0 };

    json_url(el, "url_resolved", resolved, sizeof(resolved));
    json_url(el, "url",          raw,      sizeof(raw));

    const char *chosen;

    if (is_plain_http(resolved)) {
        chosen = resolved;
    } else if (is_plain_http(raw)) {
        chosen = raw;
    } else if (resolved[0]) {
        chosen = resolved;
    } else {
        chosen = raw;
    }

    snprintf(dst, cap, "%s", chosen);
}

static int search_once(const char *query,
                       ipradio_station_t *out, int max_items)
{

    char esc[128];
    url_escape(query, esc, sizeof(esc));

    char path[256];
    /* hidebroken=true просит сервис не отдавать заведомо мёртвые
     * станции: у пользователя не должно быть выбора, который заведомо
     * не заиграет. Ограничение по числу — чтобы не тянуть мегабайты. */
    snprintf(path, sizeof(path),
             "/json/stations/byname/%s?hidebroken=true&limit=%d"
             "&order=clickcount&reverse=true",
             esc, max_items);

    ESP_LOGW(TAG, "ЗАМЕР: путь запроса %s", path);
    char *body = fetch(path);
    if (!body) {
        ESP_LOGE(TAG, "ЗАМЕР: каталог не ответил");
        return -1;          /* отказ связи - не то же, что «ничего нет» */
    }

    cJSON *arr = cJSON_Parse(body);
    free(body);
    if (!cJSON_IsArray(arr)) {
        /* Обрезанный или чужой ответ (например, страница входа
         * гостевой сети) - тоже отказ, а не пустой результат. */
        cJSON_Delete(arr);
        return -1;
    }

    int n = 0;
    const cJSON *el = NULL;
    cJSON_ArrayForEach(el, arr) {
        if (n >= max_items) {
            break;
        }
        if (!cJSON_IsObject(el)) {
            continue;
        }

        ipradio_station_t *st = &out[n];
        memset(st, 0, sizeof(*st));

        json_str(el, "name",        st->name, sizeof(st->name));
        pick_url(el, st->url, sizeof(st->url));
        json_str(el, "stationuuid", st->uuid, sizeof(st->uuid));
        /* Поле country устарело — берём countrycode (§4). */
        json_str(el, "countrycode", st->countrycode, sizeof(st->countrycode));
        json_str(el, "codec",       st->codec, sizeof(st->codec));

        const cJSON *br = cJSON_GetObjectItemCaseSensitive(el, "bitrate");
        if (cJSON_IsNumber(br)) {
            st->bitrate = (uint16_t) br->valueint;
        }

        if (st->url[0] != '\0') {
            n++;
        }
    }

    cJSON_Delete(arr);
    ESP_LOGI(TAG, "найдено станций: %d", n);
    return n;
}

/* Отметка клика уходит СВОЕЙ задачей и никого не ждёт.
 *
 * Раньше её звали прямо из задачи интерфейса, а внутри запрос
 * с таймаутом 8 секунд. Лимит молчания у задачи интерфейса - 5:
 * медленное зеркало давало гарантированную перезагрузку по сторожу
 * ровно в тот момент, когда человек сохраняет станцию. Плюс экран
 * замирал на время запроса.
 *
 * Результат никому не нужен: это счётчик рейтинга на стороне
 * сервиса, вежливость, а не обязанность. */
static void click_task(void *arg)
{
    char *uuid = arg;
    char path[128];

    /* Документация просит POST; GET отвечает 200, но засчитывается
     * ли клик - неизвестно. Ставим метод явно. */
    snprintf(path, sizeof(path), "/json/url/%s", uuid);
    free(uuid);

    char *body = fetch_post(path);
    free(body);

    vTaskDeleteWithCaps(NULL);
}

void ipradio_net_report_click_async(const char *uuid)
{
    if (!uuid || !uuid[0]) {
        return;
    }
    char *copy = strdup(uuid);
    if (!copy) {
        return;
    }
    /* Стек 12 КБ и в PSRAM, как у задачи поиска (ipradio_ui_search.c):
     * запрос тот же, и с 2026-10-08 он идёт по https - рукопожатию TLS
     * прежних 6 КБ могло не хватить. Внутренняя память под стек разовой
     * задачи - та самая, которой не хватало драйверу SDIO
     * (docs/34-memory.md). Удаляется парной vTaskDeleteWithCaps. */
    if (xTaskCreateWithCaps(click_task, "click", 12288, copy, 3, NULL,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        free(copy);
    }
}

void ipradio_net_report_click(const char *uuid)
{
    if (!uuid || !uuid[0]) {
        return;
    }
    char path[128];
    snprintf(path, sizeof(path), "/json/url/%s", uuid);

    /* Ответ не нужен: это счётчик рейтинга каталога, вежливость.
     * Неудача здесь ни на что не влияет. */
    char *body = fetch(path);
    free(body);
}

static char *fetch(const char *path)
{
    return fetch_ex(path, false);
}

static char *fetch_post(const char *path)
{
    return fetch_ex(path, true);
}
