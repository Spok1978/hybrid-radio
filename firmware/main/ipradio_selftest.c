/*
 * ipradio_selftest.c — самопроверка формата хранилища.
 *
 * Зачем и почему на плате — в ipradio_selftest.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "ipradio_selftest.h"
#include "ipradio_store_format.h"

static const char *TAG = "selftest";

static int s_pass;
static int s_fail;

/* Одна проверка. Печатаем КАЖДУЮ, а не только упавшие: молчание
 * неотличимо от «проверки не запускались». */
static void check(bool ok, const char *what)
{
    if (ok) {
        s_pass++;
        ESP_LOGI(TAG, "  ok   %s", what);
    } else {
        s_fail++;
        ESP_LOGE(TAG, "  ОТКАЗ %s", what);
    }
}

/* ------------------------------------------------------------------ *
 *  1. Печать и разбор: то, что записали, должно прочитаться
 * ------------------------------------------------------------------ */

static void test_round_trip(void)
{
    ESP_LOGI(TAG, "печать и разбор");

    ipradio_store_t *a = calloc(1, sizeof(*a));
    ipradio_store_t *b = calloc(1, sizeof(*b));
    if (!a || !b) {
        check(false, "память под проверку");
        free(a);
        free(b);
        return;
    }

    ipradio_store_fill_defaults(a);

    a->settings.volume          = 73;
    a->settings.brightness      = 55;
    a->settings.last_mode       = IPRADIO_MODE_NET;
    a->settings.last_preset_fm  = 3;
    a->settings.last_preset_net = 7;
    snprintf(a->settings.tz, sizeof(a->settings.tz), "%s", "EET-2");

    a->fm[0].used     = true;
    a->fm[0].type     = IPRADIO_MODE_FM;
    a->fm[0].freq_khz = 101500;
    a->fm[0].band     = IPRADIO_BAND_CCIR;
    snprintf(a->fm[0].name, IPRADIO_NAME_MAX, "%s", "Эхо");

    a->net[2].used = true;
    a->net[2].type = IPRADIO_MODE_NET;
    snprintf(a->net[2].name, IPRADIO_NAME_MAX, "%s", "Радио Искатель");
    snprintf(a->net[2].url,  IPRADIO_URL_MAX,
             "%s", "http://iskatel.hostingradio.ru:8015/iskatel-128.mp3");

    char *text = ipradio_store_build(a, 42);
    check(text != NULL, "печать прошла");
    if (!text) {
        free(a);
        free(b);
        return;
    }

    /* Полезная часть - ОБЪЕКТ, а не строка с экранированием.
     * Проверяем по тексту: после двоеточия должна идти фигурная
     * скобка, а не кавычка. Если здесь кавычка, вернулось старое
     * двойное кодирование, и файл раздуется вдвое. */
    check(strstr(text, "\"generation\":42") != NULL, "номер записи на месте");
    check(strchr(text, '\\') == NULL, "внутри нет экранирования");

    /* Собираем документ так же, как это делает запись на карту. */
    char crc[16];
    snprintf(crc, sizeof(crc), "%08lx",
             (unsigned long) ipradio_store_crc(text));

    size_t need = strlen(text) + 64;
    char *doc = malloc(need);
    if (!doc) {
        check(false, "память под документ");
        free(text);
        free(a);
        free(b);
        return;
    }
    snprintf(doc, need, "{\"crc\":\"%s\",\"payload\":%s}", crc, text);

    uint32_t gen = 0;
    check(ipradio_store_parse(doc, b, &gen), "разбор прошёл");
    check(gen == 42, "номер записи прочитан");

    check(b->settings.volume == 73,           "громкость");
    check(b->settings.brightness == 55,       "яркость");
    check(b->settings.last_preset_fm == 3,    "последняя эфирная ячейка");
    check(b->settings.last_preset_net == 7,   "последняя интернетная ячейка");
    check(strcmp(b->settings.tz, "EET-2") == 0, "часовой пояс");

    check(b->fm[0].used && b->fm[0].freq_khz == 101500, "эфирная ячейка");
    check(strcmp(b->fm[0].name, "Эхо") == 0,            "имя эфирной");
    check(!b->fm[1].used,                               "пустая осталась пустой");

    check(b->net[2].used,                               "интернетная ячейка");
    check(strcmp(b->net[2].name, "Радио Искатель") == 0, "имя интернетной");
    check(strstr(b->net[2].url, "iskatel") != NULL,      "адрес потока");

    /* Банки НЕ должны перемешиваться. */
    check(!b->net[0].used, "эфирная не попала в интернетный банк");
    check(!b->fm[2].used,  "интернетная не попала в эфирный банк");

    free(doc);
    free(text);
    free(a);
    free(b);
}

/* ------------------------------------------------------------------ *
 *  2. Испорченный файл не должен приниматься
 * ------------------------------------------------------------------ */

static void test_bad_crc(void)
{
    ESP_LOGI(TAG, "порченый файл");

    ipradio_store_t *st = calloc(1, sizeof(*st));
    if (!st) {
        check(false, "память под проверку");
        return;
    }

    uint32_t gen = 0;
    const char *bad =
        "{\"crc\":\"deadbeef\",\"payload\":{\"generation\":1}}";
    check(!ipradio_store_parse(bad, st, &gen), "сумма не сошлась — отказ");

    check(!ipradio_store_parse("не json вовсе", st, &gen), "мусор — отказ");

    free(st);
}

/* ------------------------------------------------------------------ *
 *  3. Перенос со старого образца: один банк -> два
 * ------------------------------------------------------------------ */

static void test_migrate_flat(void)
{
    ESP_LOGI(TAG, "перенос старого банка");

    /* Файл образца до 2026-09-15: один массив, тип в каждой ячейке,
     * одно общее last_preset. Такие карты у заказчика на руках. */
    const char *payload =
        "{\"generation\":9,"
        "\"settings\":{\"volume\":40,\"last_mode\":1,\"last_preset\":2},"
        "\"presets\":["
        "{\"used\":true,\"type\":0,\"name\":\"ФМ раз\",\"freq_khz\":100100},"
        "{\"used\":true,\"type\":1,\"name\":\"Сеть раз\",\"url\":\"http://a/1\"},"
        "{\"used\":true,\"type\":0,\"name\":\"ФМ два\",\"freq_khz\":102500},"
        "{\"used\":false},"
        "{\"used\":true,\"type\":1,\"name\":\"Сеть два\",\"url\":\"http://a/2\"}"
        "]}";

    char crc[16];
    snprintf(crc, sizeof(crc), "%08lx",
             (unsigned long) ipradio_store_crc(payload));

    size_t need = strlen(payload) + 64;
    char *doc = malloc(need);
    ipradio_store_t *st = calloc(1, sizeof(*st));
    if (!doc || !st) {
        check(false, "память под проверку");
        free(doc);
        free(st);
        return;
    }
    snprintf(doc, need, "{\"crc\":\"%s\",\"payload\":%s}", crc, payload);

    uint32_t gen = 0;
    check(ipradio_store_parse(doc, st, &gen), "старый файл прочитался");

    /* Эфирные должны лечь в эфирный банк подряд, без дыр. */
    check(st->fm[0].used && st->fm[0].freq_khz == 100100, "эфирная первая");
    check(st->fm[1].used && st->fm[1].freq_khz == 102500, "эфирная вторая");
    check(!st->fm[2].used, "лишних эфирных не появилось");

    check(st->net[0].used && strcmp(st->net[0].name, "Сеть раз") == 0,
          "интернетная первая");
    check(st->net[1].used && strcmp(st->net[1].name, "Сеть два") == 0,
          "интернетная вторая");
    check(!st->net[2].used, "лишних интернетных не появилось");

    /* Тип должен быть приведён к банку, даже если в файле он врал. */
    check(st->fm[0].type == IPRADIO_MODE_FM,  "тип эфирной приведён");
    check(st->net[0].type == IPRADIO_MODE_NET, "тип интернетной приведён");

    /* Общее last_preset уходит тому режиму, который был последним.
     * В файле last_mode = 1, то есть интернет. */
    check(st->settings.last_preset_net == 2, "старая ячейка ушла в интернет");
    check(st->settings.last_preset_fm == -1, "эфирная осталась пустой");

    free(doc);
    free(st);
}

/* ------------------------------------------------------------------ *
 *  4. Пустая ячейка не должна считаться занятой
 * ------------------------------------------------------------------ */

static void test_empty_cell(void)
{
    ESP_LOGI(TAG, "пустая ячейка");

    /* На плате 2026-09-05 банк выглядел полным сразу после
     * перезагрузки: пустая ячейка писалась как {} , а при чтении
     * пустой объект - тоже объект, и ячейка помечалась занятой.
     * Экран сообщал «все ячейки заняты», записать станцию было
     * некуда. Проверка держит эту дверь закрытой. */
    const char *payload =
        "{\"generation\":1,\"settings\":{},"
        "\"presets_net\":[{},{\"used\":false},"
        "{\"used\":true,\"type\":1,\"name\":\"Есть\",\"url\":\"http://a\"}]}";

    char crc[16];
    snprintf(crc, sizeof(crc), "%08lx",
             (unsigned long) ipradio_store_crc(payload));

    size_t need = strlen(payload) + 64;
    char *doc = malloc(need);
    ipradio_store_t *st = calloc(1, sizeof(*st));
    if (!doc || !st) {
        check(false, "память под проверку");
        free(doc);
        free(st);
        return;
    }
    snprintf(doc, need, "{\"crc\":\"%s\",\"payload\":%s}", crc, payload);

    uint32_t gen = 0;
    check(ipradio_store_parse(doc, st, &gen), "файл прочитался");
    check(!st->net[0].used, "пустой объект не занят");
    check(!st->net[1].used, "явно свободная не занята");
    check(st->net[2].used,  "занятая занята");

    free(doc);
    free(st);
}

/* ------------------------------------------------------------------ *
 *  Запуск
 * ------------------------------------------------------------------ */

bool ipradio_selftest_run(void)
{
    s_pass = 0;
    s_fail = 0;

    ESP_LOGW(TAG, "=== самопроверка формата хранилища ===");

    test_round_trip();
    test_bad_crc();
    test_migrate_flat();
    test_empty_cell();

    if (s_fail == 0) {
        ESP_LOGW(TAG, "=== сошлось всё: %d проверок ===", s_pass);
    } else {
        ESP_LOGE(TAG, "=== ОТКАЗОВ %d из %d ===", s_fail, s_pass + s_fail);
    }
    return s_fail == 0;
}
