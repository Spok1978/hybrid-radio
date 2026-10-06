/*
 * ipradio_store_format.c — формат файла хранилища: разбор и печать.
 *
 * Вынесено из ipradio_storage.c 2026-09-15. Причина простая: код
 * формата НИЧЕГО не знает про карту памяти, а весь остальной модуль
 * знает только про неё. Разделение позволяет проверять формат
 * самопроверкой (ipradio_selftest.c), не трогая карту вообще.
 *
 * Зависимости здесь только от cJSON и строк. Ни FreeRTOS, ни FATFS,
 * ни драйверов - это сделано намеренно и должно таким остаться.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "cJSON.h"

#include "ipradio_store_format.h"

static const char *TAG = "store.fmt";

void ipradio_store_fill_defaults(ipradio_store_t *st)
{
    memset(st, 0, sizeof(*st));
    st->settings.volume        = 40;
    st->settings.last_mode     = IPRADIO_MODE_FM;
    st->settings.last_band     = IPRADIO_BAND_CCIR;
    st->settings.last_freq_khz = 102500;
    st->settings.last_preset_fm  = -1;
    st->settings.last_preset_net = -1;
    st->settings.brightness    = 80;
    st->settings.clock_24h     = true;
    strncpy(st->settings.tz, "MSK-3", sizeof(st->settings.tz) - 1);
}

/* ------------------------------------------------------------------ *
 *  Контрольная сумма
 * ------------------------------------------------------------------ */

/* FNV-1a: короткая, без таблиц, для нашей задачи более чем достаточна.
 * Нам нужно отличить целый файл от обрезанного, а не защититься
 * от подделки. */
uint32_t ipradio_store_crc(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (uint8_t) *s++;
        h *= 16777619u;
    }
    return h;
}

/* ------------------------------------------------------------------ *
 *  Разбор
 * ------------------------------------------------------------------ */

static void json_get_str(const cJSON *obj, const char *key,
                         char *dst, size_t dst_size)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(it) && it->valuestring) {
        strncpy(dst, it->valuestring, dst_size - 1);
        dst[dst_size - 1] = '\0';
    }
}

static int json_get_int(const cJSON *obj, const char *key, int fallback)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(it) ? it->valueint : fallback;
}

static bool json_get_bool(const cJSON *obj, const char *key, bool fallback)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsBool(it) ? cJSON_IsTrue(it) : fallback;
}

/* Разобрать одну ячейку банка. false - ячейка пуста или мусор. */
static bool parse_preset(const cJSON *el, ipradio_preset_t *p)
{
    memset(p, 0, sizeof(*p));

    if (!cJSON_IsObject(el)) {
        return false;
    }

    /* Занятость - по явному признаку.
     *
     * Для файлов, записанных до появления этого поля, есть запасной
     * путь: ячейка считается занятой, если в ней лежит хоть что-то
     * осмысленное. Иначе после обновления прошивки старый банк
     * прочитался бы как полностью пустой. */
    const cJSON *u = cJSON_GetObjectItemCaseSensitive(el, "used");
    if (u) {
        p->used = cJSON_IsTrue(u);
    } else {
        p->used = cJSON_GetObjectItemCaseSensitive(el, "type") ||
                  cJSON_GetObjectItemCaseSensitive(el, "name");
    }
    if (!p->used) {
        return false;
    }

    p->type     = (ipradio_mode_t) json_get_int(el, "type", 0);
    p->band     = (ipradio_band_t) json_get_int(el, "band", 1);
    p->freq_khz = (uint32_t) json_get_int(el, "freq_khz", 0);
    json_get_str(el, "name", p->name, sizeof(p->name));
    json_get_str(el, "url",  p->url,  sizeof(p->url));
    json_get_str(el, "stationuuid", p->stationuuid, sizeof(p->stationuuid));
    return true;
}

/* Разобрать один банк целиком. */
static void parse_bank(const cJSON *payload, const char *key,
                       ipradio_preset_t *bank, ipradio_mode_t mode)
{
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(payload, key);
    if (!cJSON_IsArray(arr)) {
        return;
    }

    int i = 0;
    const cJSON *el = NULL;
    cJSON_ArrayForEach(el, arr) {
        if (i >= IPRADIO_PRESET_MAX) {
            break;
        }
        if (parse_preset(el, &bank[i])) {
            /* Принадлежность задаёт массив, а не запись в файле:
             * поле type приводим к банку на случай, если в файле
             * оно разошлось. */
            bank[i].type = mode;
        }
        i++;
    }
}

/* Перенос со старого образца: один общий массив -> два банка.
 *
 * Раскладываем по полю type, каждый банк заполняем подряд с начала.
 * Если станций одного вида было больше восьми - лишние теряются,
 * но в старом формате их и было не больше десяти на оба вида. */
static void migrate_flat_bank(const cJSON *payload, ipradio_store_t *st)
{
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(payload, "presets");
    if (!cJSON_IsArray(arr)) {
        return;
    }

    int nf = 0, nn = 0;
    const cJSON *el = NULL;
    cJSON_ArrayForEach(el, arr) {
        ipradio_preset_t p;
        if (!parse_preset(el, &p)) {
            continue;
        }
        if (p.type == IPRADIO_MODE_NET) {
            if (nn < IPRADIO_PRESET_MAX) {
                st->net[nn] = p;
                st->net[nn].type = IPRADIO_MODE_NET;
                nn++;
            }
        } else {
            if (nf < IPRADIO_PRESET_MAX) {
                st->fm[nf] = p;
                st->fm[nf].type = IPRADIO_MODE_FM;
                nf++;
            }
        }
    }

    ESP_LOGW(TAG, "банк старого образца: перенесено эфирных %d, "
                  "интернетных %d", nf, nn);
}

bool ipradio_store_parse(const char *text, ipradio_store_t *st,
                           uint32_t *generation)
{
    cJSON *root = cJSON_Parse(text);
    if (!root) {
        return false;
    }

    bool ok = false;

    /* Контрольная сумма считается от поля payload как от строки —
     * поэтому сравнение не зависит от того, как cJSON расставит
     * пробелы при повторной печати. */
    const cJSON *crc_it = cJSON_GetObjectItemCaseSensitive(root, "crc");
    cJSON       *body   = cJSON_GetObjectItemCaseSensitive(root, "payload");
    if (!cJSON_IsString(crc_it) || !body) {
        goto done;
    }

    /* Полезная часть бывает двух видов.
     *
     * Новый: вложенный объект. Строку для подсчёта суммы печатаем
     * из него сами - cJSON_PrintUnformatted детерминирован, и та же
     * печать шла в файл при записи.
     *
     * Старый: строка с экранированным JSON внутри. Так писали до
     * 2026-09-06; файлы, лежащие на картах, надо продолжать читать. */
    cJSON *payload   = NULL;
    char  *for_crc   = NULL;
    bool   own_for_crc = false;

    if (cJSON_IsObject(body)) {
        for_crc = cJSON_PrintUnformatted(body);
        own_for_crc = true;
        payload = body;            /* принадлежит root, не удаляем */
    } else if (cJSON_IsString(body) && body->valuestring) {
        for_crc = body->valuestring;
        payload = cJSON_Parse(body->valuestring);
    }

    if (!for_crc || !payload) {
        if (own_for_crc) {
            free(for_crc);
        }
        goto done;
    }

    char expect[16];
    snprintf(expect, sizeof(expect), "%08" PRIx32, ipradio_store_crc(for_crc));
    bool crc_ok = (strcmp(expect, crc_it->valuestring) == 0);
    if (own_for_crc) {
        free(for_crc);
    }
    if (!crc_ok) {
        ESP_LOGW(TAG, "контрольная сумма не сошлась");
        if (payload != body) {
            cJSON_Delete(payload);
        }
        goto done;
    }

    ipradio_store_fill_defaults(st);
    *generation = (uint32_t) json_get_int(payload, "generation", 0);

    const cJSON *set = cJSON_GetObjectItemCaseSensitive(payload, "settings");
    if (cJSON_IsObject(set)) {
        st->settings.volume        = (uint8_t) json_get_int(set, "volume", 40);
        st->settings.last_mode     = (ipradio_mode_t) json_get_int(set, "last_mode", 0);
        st->settings.last_band     = (ipradio_band_t) json_get_int(set, "last_band", 1);
        st->settings.last_freq_khz = (uint32_t) json_get_int(set, "last_freq_khz", 102500);
        /* Две ячейки, по одной на режим. Из файлов старого образца
         * читаем единственное last_preset и кладём его тому режиму,
         * который был последним: в другом банке этот номер означал
         * бы чужую станцию. */
        st->settings.last_preset_fm =
            (int8_t) json_get_int(set, "last_preset_fm", -1);
        st->settings.last_preset_net =
            (int8_t) json_get_int(set, "last_preset_net", -1);

        if (!cJSON_GetObjectItemCaseSensitive(set, "last_preset_fm") &&
            !cJSON_GetObjectItemCaseSensitive(set, "last_preset_net")) {
            int8_t old_cell = (int8_t) json_get_int(set, "last_preset", -1);
            if (st->settings.last_mode == IPRADIO_MODE_NET) {
                st->settings.last_preset_net = old_cell;
            } else {
                st->settings.last_preset_fm = old_cell;
            }
        }
        st->settings.brightness    = (uint8_t) json_get_int(set, "brightness", 80);
        st->settings.clock_24h     = json_get_bool(set, "clock_24h", true);
        json_get_str(set, "tz", st->settings.tz, sizeof(st->settings.tz));
    }

    parse_bank(payload, "presets_fm",  st->fm,  IPRADIO_MODE_FM);
    parse_bank(payload, "presets_net", st->net, IPRADIO_MODE_NET);

    /* Файл старого образца: один общий массив "presets", тип лежит
     * в каждой ячейке. Такие карты у заказчика на руках, и терять
     * записанные станции нельзя - раскладываем по банкам. */
    if (!cJSON_GetObjectItemCaseSensitive(payload, "presets_fm") &&
        !cJSON_GetObjectItemCaseSensitive(payload, "presets_net")) {
        migrate_flat_bank(payload, st);
    }

    /* Удаляем ТОЛЬКО если разбирали сами.
     *
     * В новом образце payload - это узел внутри root, и удалять его
     * отдельно нельзя: следом cJSON_Delete(root) освободит его
     * повторно. Именно так и было написано до 2026-09-15, и это
     * ломало кучу. На плате не проявлялось, а самопроверка на машине
     * поймала при первом же прогоне: 0xC0000374, heap corruption. */
    if (payload != body) {
        cJSON_Delete(payload);
    }
    ok = true;

done:
    cJSON_Delete(root);
    return ok;
}

/* ------------------------------------------------------------------ *
 *  Печать
 * ------------------------------------------------------------------ */

/* Имена станций санитизируются при записи: кавычки и переводы строк
 * из них вычищаются (§8). cJSON экранировал бы их правильно, но файл
 * должен оставаться пригодным для правки руками через SSH. */
static void sanitize(const char *src, char *dst, size_t dst_size)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < dst_size; i++) {
        char c = src[i];
        if (c == '"' || c == '\\' || c == '\n' || c == '\r') {
            c = ' ';
        }
        dst[j++] = c;
    }
    dst[j] = '\0';
}

/* Записать один банк массивом. */
static void build_bank(cJSON *root, const char *key,
                       const ipradio_preset_t *bank)
{
    cJSON *arr = cJSON_AddArrayToObject(root, key);
    if (!arr) {
        return;
    }

    char clean[IPRADIO_NAME_MAX];

    for (int i = 0; i < IPRADIO_PRESET_MAX; i++) {
        const ipradio_preset_t *p = &bank[i];
        cJSON *el = cJSON_CreateObject();
        if (!el) {
            return;
        }

        /* Признак занятости пишем ЯВНО.
         *
         * Раньше пустая ячейка выходила пустым объектом {}, а при
         * чтении пустой объект - тоже объект, и ячейка помечалась
         * занятой. После первой же перезагрузки банк выглядел полным:
         * на плате 2026-09-05 сохранить найденную станцию стало
         * некуда, экран писал «все ячейки заняты». */
        cJSON_AddBoolToObject(el, "used", p->used);

        if (p->used) {
            sanitize(p->name, clean, sizeof(clean));
            cJSON_AddNumberToObject(el, "type", p->type);
            cJSON_AddStringToObject(el, "name", clean);

            /* Пишем только то, что относится к этому виду станции:
             * у эфирной нет адреса, у интернетной - частоты. */
            if (p->type == IPRADIO_MODE_FM) {
                cJSON_AddNumberToObject(el, "band",     p->band);
                cJSON_AddNumberToObject(el, "freq_khz", p->freq_khz);
            } else {
                cJSON_AddStringToObject(el, "url",         p->url);
                cJSON_AddStringToObject(el, "stationuuid", p->stationuuid);
            }
        }
        cJSON_AddItemToArray(arr, el);
    }
}

char *ipradio_store_build(const ipradio_store_t *st, uint32_t generation)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    cJSON_AddNumberToObject(root, "generation", generation);

    cJSON *set = cJSON_AddObjectToObject(root, "settings");
    cJSON_AddNumberToObject(set, "volume",        st->settings.volume);
    cJSON_AddNumberToObject(set, "last_mode",     st->settings.last_mode);
    cJSON_AddNumberToObject(set, "last_band",     st->settings.last_band);
    cJSON_AddNumberToObject(set, "last_freq_khz", st->settings.last_freq_khz);
    cJSON_AddNumberToObject(set, "last_preset_fm",  st->settings.last_preset_fm);
    cJSON_AddNumberToObject(set, "last_preset_net", st->settings.last_preset_net);
    cJSON_AddNumberToObject(set, "brightness",    st->settings.brightness);
    cJSON_AddBoolToObject  (set, "clock_24h",     st->settings.clock_24h);
    cJSON_AddStringToObject(set, "tz",            st->settings.tz);

    build_bank(root, "presets_fm",  st->fm);
    build_bank(root, "presets_net", st->net);

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return text;
}

