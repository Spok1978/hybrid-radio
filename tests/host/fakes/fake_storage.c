/*
 * fake_storage.c — хранилище в памяти вместо карты.
 *
 * Настоящее ipradio_storage.c монтирует карту, пишет два файла
 * по очереди и считает контрольные суммы. Для проверки автомата
 * всё это не нужно и мешает: автомату важно только, что банк
 * читается и пишется.
 *
 * Поэтому здесь тот же набор функций, но содержимое лежит в обычной
 * переменной. Тест может выставить банк как угодно перед проверкой
 * и посмотреть, что в нём стало после.
 *
 * Формат файла этим НЕ проверяется - для него своя самопроверка,
 * ipradio_selftest.c, и она гоняет настоящий код разбора.
 */

#include <string.h>

#include "ipradio_storage.h"

static ipradio_store_t s_store;
static uint32_t        s_generation;

/* Для проверок правки ячеек (test_preset_edit.c).
 *
 * s_edit_depth - сколько раз замок взят и не отпущен. Настоящее
 * хранилище держит под ним мьютекс, и незакрытая правка вешает
 * всех, кто следом обратится к карте. Здесь её видно числом. */
static int  s_edit_depth;
static bool s_ready = true;
static bool s_fail_writes;
static int  s_writes;

/* --- то, чем пользуется тест -------------------------------------- */

void fake_storage_reset(void)
{
    memset(&s_store, 0, sizeof(s_store));
    s_store.settings.volume          = 40;
    s_store.settings.last_mode       = IPRADIO_MODE_FM;
    s_store.settings.last_band       = IPRADIO_BAND_CCIR;
    s_store.settings.last_freq_khz   = 102500;
    s_store.settings.last_preset_fm  = -1;
    s_store.settings.last_preset_net = -1;
    s_store.settings.brightness      = 80;
    s_store.settings.clock_24h       = true;
    s_generation = 0;
    s_edit_depth  = 0;
    s_ready       = true;
    s_fail_writes = false;
    s_writes      = 0;
}

/* Сколько правок открыто и не закрыто. После любого вызова - ноль. */
int fake_storage_edit_depth(void)
{
    return s_edit_depth;
}

/* Карты нет: правка не начинается вовсе, как у настоящего. */
void fake_storage_set_ready(bool ready)
{
    s_ready = ready;
}

/* Карта есть, но запись на неё не удаётся. */
void fake_storage_fail_writes(bool fail)
{
    s_fail_writes = fail;
}

/* Сколько раз писали на «карту». */
int fake_storage_writes(void)
{
    return s_writes;
}

ipradio_store_t *fake_storage_raw(void)
{
    return &s_store;
}

/* --- то, что зовёт проверяемый код --------------------------------- */

esp_err_t ipradio_storage_init(void)
{
    fake_storage_reset();
    return ESP_OK;
}

void ipradio_storage_get(ipradio_store_t *out)
{
    if (out) {
        *out = s_store;
    }
}

esp_err_t ipradio_storage_save(const ipradio_store_t *in)
{
    if (!in) {
        return ESP_ERR_INVALID_ARG;
    }
    s_store = *in;
    s_generation++;
    return ESP_OK;
}

esp_err_t ipradio_storage_save_settings(const ipradio_settings_t *s)
{
    if (!s) {
        return ESP_ERR_INVALID_ARG;
    }
    s_store.settings = *s;
    s_generation++;
    return ESP_OK;
}

ipradio_store_t *ipradio_storage_edit_begin(void)
{
    /* Как настоящее: без карты правка не начинается и замок
     * не берётся. */
    if (!s_ready) {
        return NULL;
    }
    s_edit_depth++;
    return &s_store;
}

esp_err_t ipradio_storage_edit_end(bool save)
{
    s_edit_depth--;
    if (save) {
        /* Настоящее поднимает поколение ДО попытки записи
         * (save_locked в ipradio_storage.c) - повторяем. */
        s_generation++;
        if (s_fail_writes) {
            return ESP_FAIL;
        }
        s_writes++;
    }
    return ESP_OK;
}

bool ipradio_storage_ready(void)
{
    return s_ready;
}

uint32_t ipradio_storage_generation(void)
{
    return s_generation;
}

void ipradio_storage_put_ram(const ipradio_store_t *in)
{
    if (in) {
        s_store = *in;
    }
}

bool ipradio_storage_get_preset(ipradio_mode_t mode, int cell,
                                ipradio_preset_t *out)
{
    if (!out || cell < 1 || cell > IPRADIO_PRESET_MAX) {
        return false;
    }
    *out = ipradio_storage_bank(&s_store, mode)[cell - 1];
    return out->used;
}

void ipradio_storage_get_settings(ipradio_settings_t *out)
{
    if (out) {
        *out = s_store.settings;
    }
}
