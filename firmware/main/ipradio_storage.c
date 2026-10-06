/*
 * ipradio_storage.c — чтение и запись /ipradio.json на карте microSD.
 *
 * Устойчивость к обрыву питания строится тремя слоями, потому что
 * гарантий файловой системы у нас нет (вопрос Q14):
 *
 *   1. Запись во временный файл, затем переименование. Даже если
 *      переименование на FAT окажется не атомарным, испорченным будет
 *      только один из двух файлов.
 *   2. Две копии по очереди, A и B. Пишем всегда в ту, что старше.
 *   3. Контрольная сумма в самом файле. При старте берём ту копию,
 *      которая проходит проверку и имеет больший номер поколения.
 *
 * Разбор — docs/26-firmware-spec.md, §8.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "cJSON.h"

#include "ipradio_storage.h"
#include "ipradio_store_format.h"

static const char *TAG = "storage";

#define MOUNT_POINT   "/sd"
#define FILE_A        MOUNT_POINT "/ipradio.json"
#define FILE_B        MOUNT_POINT "/ipradio.bak"
#define FILE_TMP      MOUNT_POINT "/ipradio.tmp"

/* Выводы карты подтверждены схемой и docs/IO.md вендора:
 * D0…D3 = GPIO39…42, CMD = GPIO44, CLK = GPIO43. В стороннем описании
 * железа эта карта была сдвинута — см. docs/29-yaml-review.md. */
#define SD_PIN_CLK    43
#define SD_PIN_CMD    44
#define SD_PIN_D0     39
#define SD_PIN_D1     40
#define SD_PIN_D2     41
#define SD_PIN_D3     42

static sdmmc_card_t   *s_card;
static bool            s_ready;
/* Хранилище целиком - 2548 байт. Оно живёт под мьютексом и трогается
 * только из задач, так что внутренняя память ему ни к чему. */
EXT_RAM_BSS_ATTR static ipradio_store_t s_store;
static sd_pwr_ctrl_handle_t s_pwr;
static uint32_t        s_generation;   /* растёт при каждой записи */

/* Замок на всё содержимое модуля.
 *
 * Писателей три задачи сразу: автомат (запись пресета, настройки при
 * выключении), интерфейс (переименование, удаление, сохранение
 * найденной станции, яркость, часы) и задача автопоиска. Раньше замка
 * не было вовсе, а временный файл при записи один на всех: два
 * одновременных сохранения перемешивали его содержимое, и под ударом
 * оказывались ОБЕ копии. Двойное хранение с контрольной суммой тут
 * не спасает - оно защищает от обрыва питания, а не от двух писателей.
 *
 * ЧТО ЭТОТ ЗАМОК НЕ ЛЕЧИТ: потерю обновления. Все зовущие делают
 * «прочитать всё - изменить одно поле - записать всё», и если двое
 * сделают это подряд, второй затрёт правку первого. Файл при этом
 * останется целым. Лечится это только сведением записи в одну задачу
 * либо API вида «измени вот это поле», и делать так стоит, когда
 * появится повод: сейчас одновременные правки - редкость, а порча
 * файла была бы потерей всех настроек. */
static SemaphoreHandle_t s_lock;
static bool            s_next_is_b;    /* куда писать в следующий раз */

/* ------------------------------------------------------------------ *
 *  Умолчания
 * ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ *
 *  Файлы
 * ------------------------------------------------------------------ */

static char *read_whole_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    /* Потолок по РЕАЛЬНОМУ размеру, а не «на всякий случай».
     *
     * Хранилище - настройки плюс восемь ячеек, это три-шесть
     * килобайт. Шестнадцати хватает с тройным запасом, а прежние
     * шестьдесят четыре означали, что при испорченной таблице
     * файлов мы поверим любому вранью ftell и выделим под него
     * память. */
    if (size <= 0 || size > 16 * 1024) {
        fclose(f);
        return NULL;
    }

    char *buf = malloc((size_t) size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t got = fread(buf, 1, (size_t) size, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

static esp_err_t write_atomically(const char *path, const char *payload)
{
    /* Обёртку с контрольной суммой складываем строкой сами.
     *
     * Раньше здесь строился второй документ cJSON, куда payload
     * клался ЗНАЧЕНИЕМ СТРОКИ. Это стоило дорого: cJSON копировал
     * строку себе, потом печатал заново - и при печати экранировал
     * каждую кавычку, а payload это JSON, в нём кавычки на каждом
     * поле. Файл на карте раздувался примерно вдвое, а в памяти
     * одновременно жили три копии.
     *
     * Теперь payload вкладывается КАК ОБЪЕКТ, без экранирования:
     * {"crc":"xxxxxxxx","payload":{...}}. Сумма при этом считается
     * от той же самой строки, что уходит в файл, - значит, читатель
     * может пересчитать её по тексту, ничего не разбирая.
     *
     * Старые файлы читаются по-прежнему: разбор понимает оба вида,
     * см. parse_document. */
    char crc[16];
    snprintf(crc, sizeof(crc), "%08" PRIx32, ipradio_store_crc(payload));

    const char *head = "{\"crc\":\"";
    const char *mid  = "\",\"payload\":";
    const char *tail = "}";

    size_t need = strlen(head) + strlen(crc) + strlen(mid) +
                  strlen(payload) + strlen(tail) + 1;

    char *text = malloc(need);
    if (!text) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(text, need, "%s%s%s%s%s", head, crc, mid, payload, tail);

    esp_err_t err = ESP_FAIL;
    FILE *f = fopen(FILE_TMP, "wb");
    if (!f) {
        ESP_LOGE(TAG, "не открылся временный файл");
        goto done;
    }

    size_t len = strlen(text);
    if (fwrite(text, 1, len, f) != len) {
        fclose(f);
        ESP_LOGE(TAG, "запись не прошла целиком");
        goto done;
    }

    /* Сначала на носитель, только потом переименование. Без этого
     * переименование может опередить данные. */
    fflush(f);
    fsync(fileno(f));
    fclose(f);

    unlink(path);                    /* на FAT rename поверх не работает */
    if (rename(FILE_TMP, path) != 0) {
        ESP_LOGE(TAG, "переименование не удалось");
        goto done;
    }

    err = ESP_OK;

done:
    free(text);
    return err;
}

/* ------------------------------------------------------------------ *
 *  Интерфейс
 * ------------------------------------------------------------------ */

static esp_err_t mount_card(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot         = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    /* ПИТАНИЕ СЛОТА. Без этого карта не отвечает вовсе.
     *
     * У ESP32-P4 линия питания карты запитана не напрямую, а через
     * внутренний стабилизатор кристалла, и его надо включить руками.
     * Пока он выключен, карта просто обесточена: контроллер шлёт ей
     * команду инициализации и получает тайм-аут. Ровно это и было
     * на плате 2026-09-05 - карта вставлена, а в журнале
     * «send_op_cond returned 0x107».
     *
     * Канал 4 - тот, к которому слот подведён на этой плате; взято
     * из BSP вендора, где то же самое делается перед монтированием. */
    sd_pwr_ctrl_ldo_config_t ldo = { .ldo_chan_id = 4 };
    esp_err_t perr = sd_pwr_ctrl_new_on_chip_ldo(&ldo, &s_pwr);
    if (perr != ESP_OK) {
        ESP_LOGE(TAG, "питание карты не включилось: %s",
                 esp_err_to_name(perr));
        return perr;
    }
    host.pwr_ctrl_handle = s_pwr;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk   = SD_PIN_CLK;
    slot.cmd   = SD_PIN_CMD;
    slot.d0    = SD_PIN_D0;
    slot.d1    = SD_PIN_D1;
    slot.d2    = SD_PIN_D2;
    slot.d3    = SD_PIN_D3;
    slot.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount = {
        /* Карту НЕ форматируем при неудаче монтирования: на ней могут
         * быть станции, которые пользователь набивал руками. Лучше
         * подняться без карты и сказать об этом. */
        .format_if_mount_failed = false,
        .max_files              = 4,
        .allocation_unit_size   = 16 * 1024,
    };

    return esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot, &mount, &s_card);
}

esp_err_t ipradio_storage_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    ipradio_store_fill_defaults(&s_store);

    esp_err_t err = mount_card();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "карта не смонтирована (%s) — работаем без неё",
                 esp_err_to_name(err));
        s_ready = false;
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "карта смонтирована, %llu МБ",
             ((uint64_t) s_card->csd.capacity * s_card->csd.sector_size) / (1024 * 1024));
    s_ready = true;

    /* Читаем обе копии и берём ту, что целее и новее. */
    /* ДВЕ копии хранилища - НЕ на стеке.
     *
     * После разделения банков структура выросла примерно до пяти
     * килобайт, две штуки - десять. Стек главной задачи 10240 байт,
     * и этого хватило ровно на то, чтобы при первой же загрузке
     * словить Stack protection fault и уйти в цикл перезагрузок.
     * До разделения две копии были по три килобайта и влезали.
     *
     * Статические и в PSRAM: читаются один раз при подъёме, из одной
     * задачи, делить их не с кем. */
    static EXT_RAM_BSS_ATTR ipradio_store_t a, b;
    uint32_t gen_a = 0, gen_b = 0;
    bool ok_a = false, ok_b = false;

    char *text = read_whole_file(FILE_A);
    if (text) {
        ok_a = ipradio_store_parse(text, &a, &gen_a);
        free(text);
    }
    text = read_whole_file(FILE_B);
    if (text) {
        ok_b = ipradio_store_parse(text, &b, &gen_b);
        free(text);
    }

    if (ok_a && (!ok_b || gen_a >= gen_b)) {
        s_store = a;
        s_generation = gen_a;
        s_next_is_b = true;
        ESP_LOGI(TAG, "прочитана копия A, поколение %" PRIu32, gen_a);
    } else if (ok_b) {
        s_store = b;
        s_generation = gen_b;
        s_next_is_b = false;
        ESP_LOGW(TAG, "копия A не читается, взята B, поколение %" PRIu32, gen_b);
    } else {
        ESP_LOGW(TAG, "обе копии недоступны — начинаем с умолчаний");
        s_generation = 0;
        s_next_is_b = false;
        ipradio_storage_save(&s_store);   /* создать дефолтную */
    }

    return ESP_OK;
}

void ipradio_storage_get(ipradio_store_t *out)
{
    if (!out) {
        return;
    }
    /* Копия под замком: без него читатель мог застать структуру
     * посреди записи другой задачей. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_store;
    xSemaphoreGive(s_lock);
}

bool ipradio_storage_ready(void)
{
    return s_ready;
}

/* Записать текущее содержимое s_store на карту. ЗАМОК ДОЛЖЕН БЫТЬ
 * УЖЕ ВЗЯТ вызывающим. */
static esp_err_t save_locked(void)
{
    s_generation++;

    char *payload = ipradio_store_build(&s_store, s_generation);
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }

    const char *target = s_next_is_b ? FILE_B : FILE_A;
    esp_err_t err = write_atomically(target, payload);
    free(payload);

    if (err == ESP_OK) {
        /* Чередуем только после успеха: иначе неудачная запись
         * заставила бы нас в следующий раз затереть единственную
         * целую копию. */
        s_next_is_b = !s_next_is_b;
        ESP_LOGI(TAG, "сохранено в %s, поколение %" PRIu32,
                 s_next_is_b ? "A" : "B", s_generation);
    }
    return err;
}

esp_err_t ipradio_storage_save(const ipradio_store_t *in)
{
    if (!in) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        return ESP_ERR_NOT_FOUND;
    }

    /* Вся запись - под замком, включая временный файл: он один
     * на всех, и два одновременных сохранения перемешали бы его. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_store = *in;
    esp_err_t err = save_locked();
    xSemaphoreGive(s_lock);
    return err;
}

ipradio_store_t *ipradio_storage_edit_begin(void)
{
    if (!s_ready) {
        return NULL;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    return &s_store;
}

esp_err_t ipradio_storage_edit_end(bool save)
{
    esp_err_t err = ESP_OK;
    if (save) {
        err = save_locked();
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t ipradio_storage_save_settings(const ipradio_settings_t *s)
{
    if (!s) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Правим на месте, не копируя банки.
     *
     * Раньше здесь снималась полная копия хранилища на стек ради
     * одной подструктуры. После разделения банков это около пяти
     * килобайт, и зовётся отсюда в том числе задача lvgl. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_store.settings = *s;
    esp_err_t err = save_locked();
    xSemaphoreGive(s_lock);
    return err;
}

bool ipradio_storage_get_preset(ipradio_mode_t mode, int cell,
                                ipradio_preset_t *out)
{
    if (!out || cell < 1 || cell > IPRADIO_PRESET_MAX) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = ipradio_storage_bank(&s_store, mode)[cell - 1];
    xSemaphoreGive(s_lock);
    return out->used;
}

void ipradio_storage_get_settings(ipradio_settings_t *out)
{
    if (!out) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_store.settings;
    xSemaphoreGive(s_lock);
}

void ipradio_storage_put_ram(const ipradio_store_t *in)
{
    if (!in) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_store = *in;
    s_generation++;      /* чтобы экраны перечитали банк */
    xSemaphoreGive(s_lock);
}

uint32_t ipradio_storage_generation(void)
{
    /* Читается интерфейсом на каждую перерисовку полосы пресетов.
     * Одно выровненное 32-разрядное слово - замок тут дороже пользы,
     * а увидеть значение на одну проверку позже безобидно. */
    return s_generation;
}
