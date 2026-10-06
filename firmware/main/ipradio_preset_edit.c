/*
 * ipradio_preset_edit.c — правка ячеек банка.
 *
 * Замысел — в ipradio_preset_edit.h.
 *
 * Правило, ради которого модуль заведён: КАЖДЫЙ путь из функции после
 * ipradio_storage_edit_begin() проходит через ipradio_storage_edit_end().
 * Проверки на машине разработчика это и ловят - поддельное хранилище
 * считает, сколько раз замок взят и отпущен.
 */

#include <stdio.h>
#include <string.h>

#include "ipradio_preset_edit.h"

static bool cell_ok(int cell)
{
    return cell >= 1 && cell <= IPRADIO_PRESET_MAX;
}

/* Отпустить замок и перевести итог записи на карту в наш код. */
static ipradio_pedit_t finish(bool save)
{
    return (ipradio_storage_edit_end(save) == ESP_OK)
           ? IPRADIO_PEDIT_OK : IPRADIO_PEDIT_WRITE_FAILED;
}

/* Скопировать имя, срезав пробелы по краям.
 *
 * Пробелом считаются только ASCII-пробел и табуляция: все байты
 * кириллицы в UTF-8 больше 0x7F, спутать их с пробелом нельзя.
 * В каталоге имена с пробелом на конце встречаются: «Silver Rain »
 * из журнала 2026-10-05.
 *
 * Если имя длиннее поля, режем по границе знака, а не байта:
 * половина русской буквы на конце рисуется мусором. */
static void name_copy(char *dst, size_t cap, const char *src)
{
    while (*src == ' ' || *src == '\t') {
        src++;
    }
    size_t n = strlen(src);
    while (n > 0 && (src[n - 1] == ' ' || src[n - 1] == '\t')) {
        n--;
    }
    if (n >= cap) {
        n = cap - 1;
        /* Байт 10xxxxxx - продолжение знака; отступаем к его началу. */
        while (n > 0 && ((unsigned char) src[n] & 0xC0) == 0x80) {
            n--;
        }
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

ipradio_pedit_t ipradio_preset_rename(ipradio_mode_t mode, int cell,
                                      const char *name)
{
    if (!cell_ok(cell) || !name) {
        return IPRADIO_PEDIT_BAD_ARG;
    }

    char clean[IPRADIO_NAME_MAX];
    name_copy(clean, sizeof(clean), name);

    ipradio_store_t *st = ipradio_storage_edit_begin();
    if (!st) {
        return IPRADIO_PEDIT_NO_CARD;
    }

    ipradio_preset_t *p = &ipradio_storage_bank(st, mode)[cell - 1];
    if (!p->used) {
        ipradio_storage_edit_end(false);
        return IPRADIO_PEDIT_EMPTY;
    }

    /* То же имя - писать на карту незачем. */
    if (strcmp(p->name, clean) == 0) {
        ipradio_storage_edit_end(false);
        return IPRADIO_PEDIT_OK;
    }

    memcpy(p->name, clean, strlen(clean) + 1);
    return finish(true);
}

ipradio_pedit_t ipradio_preset_clear(ipradio_mode_t mode, int cell)
{
    if (!cell_ok(cell)) {
        return IPRADIO_PEDIT_BAD_ARG;
    }

    ipradio_store_t *st = ipradio_storage_edit_begin();
    if (!st) {
        return IPRADIO_PEDIT_NO_CARD;
    }

    ipradio_preset_t *p = &ipradio_storage_bank(st, mode)[cell - 1];
    if (!p->used) {
        ipradio_storage_edit_end(false);
        return IPRADIO_PEDIT_EMPTY;
    }

    memset(p, 0, sizeof(*p));
    return finish(true);
}

ipradio_pedit_t ipradio_preset_put_net(int cell, const char *name,
                                       const char *url, const char *uuid,
                                       int *out_cell)
{
    /* Адрес, не влезающий в поле, не обрезаем, а отказываем: обрезанный
     * адрес не играет, а человек увидел бы «станция не отвечает»
     * про исправную станцию. */
    if (cell < 0 || cell > IPRADIO_PRESET_MAX ||
        !url || !url[0] || strlen(url) >= IPRADIO_URL_MAX) {
        return IPRADIO_PEDIT_BAD_ARG;
    }

    ipradio_store_t *st = ipradio_storage_edit_begin();
    if (!st) {
        return IPRADIO_PEDIT_NO_CARD;
    }

    ipradio_preset_t *bank = ipradio_storage_bank(st, IPRADIO_MODE_NET);
    int idx = -1;

    if (cell > 0) {
        /* Замена из меню ячейки: туда, куда сказали. Если та же
         * станция лежит в другой ячейке, будет две одинаковых - это
         * выбор человека, и отменять его молча мы не вправе. */
        idx = cell - 1;
    } else {
        /* Обычный поиск: та же станция могла быть сохранена раньше -
         * тогда просто включаем её, второй такой же не заводим. */
        for (int c = 0; c < IPRADIO_PRESET_MAX; c++) {
            if (bank[c].used && strcmp(bank[c].url, url) == 0) {
                ipradio_storage_edit_end(false);
                if (out_cell) {
                    *out_cell = c + 1;
                }
                return IPRADIO_PEDIT_OK;
            }
        }
        for (int c = 0; c < IPRADIO_PRESET_MAX; c++) {
            if (!bank[c].used) {
                idx = c;
                break;
            }
        }
        if (idx < 0) {
            /* ВОТ ЗДЕСЬ до 2026-10-06 замок оставался взятым:
             * поиск возвращался, не вызвав edit_end. */
            ipradio_storage_edit_end(false);
            return IPRADIO_PEDIT_FULL;
        }
    }

    ipradio_preset_t *p = &bank[idx];
    memset(p, 0, sizeof(*p));
    p->used = true;
    p->type = IPRADIO_MODE_NET;
    name_copy(p->name, sizeof(p->name), name ? name : "");
    memcpy(p->url, url, strlen(url) + 1);
    if (uuid) {
        snprintf(p->stationuuid, sizeof(p->stationuuid), "%s", uuid);
    }

    if (out_cell) {
        *out_cell = idx + 1;
    }
    return finish(true);
}
