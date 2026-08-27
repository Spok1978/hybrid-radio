/*
 * ipradio_storage.h — хранение на карте microSD.
 *
 * Один файл — источник истины: /ipradio.json (docs/26-firmware-spec.md, §8).
 * Прошивка своя, поэтому чужой формат не поддерживается и проекция
 * не нужна — решение Q13 снято, разбор в docs/25-station-storage.md, §10.
 *
 * Правила записи, из §8 и вопроса Q14:
 *   - запись атомарная: временный файл, сброс на носитель, переименование;
 *   - две копии по очереди с контрольной суммой, потому что гарантии
 *     FATFS при обрыве питания не установлены;
 *   - при отсутствии или повреждении обеих копий создаётся дефолтная,
 *     и прибор поднимается с пустым списком, а не отказывается стартовать.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "ipradio_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IPRADIO_URL_MAX        192
#define IPRADIO_UUID_MAX       40
#define IPRADIO_PRESET_MAX     8

/** Ячейка банка пресетов. Тип хранится В САМОЙ ЗАПИСИ — именно поэтому
 *  нажатие пресета само переключает режим (docs/22-mode-switching.md). */
typedef struct {
    bool            used;
    ipradio_mode_t  type;
    char            name[IPRADIO_NAME_MAX];

    /* Для эфирных */
    ipradio_band_t  band;
    uint32_t        freq_khz;

    /* Для интернетных */
    char            url[IPRADIO_URL_MAX];
    char            stationuuid[IPRADIO_UUID_MAX];
} ipradio_preset_t;

/** Настройки, переживающие выключение. */
typedef struct {
    uint8_t         volume;          /**< 0…100                          */
    ipradio_mode_t  last_mode;
    ipradio_band_t  last_band;
    uint32_t        last_freq_khz;
    int8_t          last_preset;     /**< 1…N или -1                     */
    uint8_t         brightness;      /**< 0…100                          */
    char            tz[32];          /**< часовой пояс, формат POSIX TZ  */
    bool            clock_24h;
} ipradio_settings_t;

/** Всё содержимое файла. */
typedef struct {
    ipradio_settings_t settings;
    ipradio_preset_t   presets[IPRADIO_PRESET_MAX];
} ipradio_store_t;

/** Смонтировать карту и прочитать файл.
 *  Возвращает ESP_OK и при повреждении файла: в этом случае store
 *  заполняется умолчаниями, а в журнал уходит предупреждение.
 *  ESP_ERR_NOT_FOUND означает, что карты нет вовсе. */
esp_err_t ipradio_storage_init(void);

/** Копия текущего содержимого. */
void ipradio_storage_get(ipradio_store_t *out);

/** Записать. Атомарно, с чередованием двух копий. */
esp_err_t ipradio_storage_save(const ipradio_store_t *in);

/** Сохранить только настройки, не трогая пресеты. */
esp_err_t ipradio_storage_save_settings(const ipradio_settings_t *s);

/** Есть ли карта и читается ли она. */
bool ipradio_storage_ready(void);

#ifdef __cplusplus
}
#endif
