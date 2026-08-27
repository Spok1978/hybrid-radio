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

#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
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

#define MIRROR_HOST       "all.api.radio-browser.info"
#define MIRROR_MAX        8
#define HTTP_TIMEOUT_MS   8000
#define RESPONSE_MAX      (48 * 1024)

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define RECONNECT_DELAY_MS  5000

static EventGroupHandle_t   s_wifi_events;
static ipradio_net_state_t  s_state = IPRADIO_NET_NOT_CONFIGURED;
static bool                 s_have_credentials;
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
        vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
        esp_wifi_connect();

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *) data;
        ESP_LOGI(TAG, "адрес получен: " IPSTR, IP2STR(&ev->ip_info.ip));
        set_state(IPRADIO_NET_CONNECTED);
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

esp_err_t ipradio_net_init(void)
{
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
        strncpy((char *) wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
        if (pass) {
            strncpy((char *) wc.sta.password, pass,
                    sizeof(wc.sta.password) - 1);
        }
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
        s_have_credentials = true;
    }

    if (!s_have_credentials) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_wifi_stop();
    return esp_wifi_start();
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

/* Документация сервиса просит брать список зеркал из DNS и выбирать
 * случайное. Резолвер отдаёт несколько A-записей; берём одну наугад. */
static bool pick_mirror(void)
{
    struct addrinfo hints = {
        .ai_family   = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;

    if (getaddrinfo(MIRROR_HOST, "80", &hints, &res) != 0 || !res) {
        ESP_LOGW(TAG, "зеркала каталога не разрешились");
        return false;
    }

    char addrs[MIRROR_MAX][INET_ADDRSTRLEN];
    int n = 0;
    for (struct addrinfo *it = res; it && n < MIRROR_MAX; it = it->ai_next) {
        struct sockaddr_in *sa = (struct sockaddr_in *) it->ai_addr;
        inet_ntop(AF_INET, &sa->sin_addr, addrs[n], INET_ADDRSTRLEN);
        n++;
    }
    freeaddrinfo(res);

    if (n == 0) {
        return false;
    }

    int idx = (int) (esp_random() % (uint32_t) n);
    snprintf(s_mirror, sizeof(s_mirror), "%s", addrs[idx]);
    ESP_LOGI(TAG, "зеркало каталога: %s (из %d)", s_mirror, n);
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
        return ESP_OK;      /* ответ длиннее, чем мы готовы принять */
    }
    memcpy(r->buf + r->len, evt->data, evt->data_len);
    r->len += evt->data_len;
    r->buf[r->len] = '\0';
    return ESP_OK;
}

static char *fetch(const char *path)
{
    if (s_state != IPRADIO_NET_CONNECTED) {
        return NULL;
    }
    if (s_mirror[0] == '\0' && !pick_mirror()) {
        return NULL;
    }

    char url[256];
    snprintf(url, sizeof(url), "http://%s%s", s_mirror, path);

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
        /* Хост подставляем вручную: обращаемся по адресу зеркала,
         * а виртуальный хост у сервиса именованный. */
        .host          = NULL,
    };

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        free(r.buf);
        return NULL;
    }

    esp_http_client_set_header(cli, "User-Agent", USER_AGENT);
    esp_http_client_set_header(cli, "Host", MIRROR_HOST);

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

int ipradio_net_search(const char *query,
                       ipradio_station_t *out, int max_items)
{
    if (!query || !out || max_items <= 0) {
        return 0;
    }

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

    char *body = fetch(path);
    if (!body) {
        return 0;
    }

    cJSON *arr = cJSON_Parse(body);
    free(body);
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        return 0;
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
        json_str(el, "url_resolved", st->url, sizeof(st->url));
        if (st->url[0] == '\0') {
            json_str(el, "url", st->url, sizeof(st->url));
        }
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
