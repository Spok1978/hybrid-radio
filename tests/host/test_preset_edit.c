/*
 * test_preset_edit.c — проверки правки ячеек банка
 * (firmware/main/ipradio_preset_edit.c).
 *
 * Главное, что здесь проверяется после КАЖДОГО вызова, - замок
 * хранилища отпущен. До 2026-10-06 поиск по каталогу при полном банке
 * возвращался с взятым замком, и следующее обращение к карте из любой
 * задачи вставало навсегда. Поддельное хранилище считает взятия
 * и отпускания, и такая ошибка видна здесь числом.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "ipradio_preset_edit.h"
#include "ipradio_storage.h"

void             fake_storage_reset(void);
ipradio_store_t *fake_storage_raw(void);
int              fake_storage_edit_depth(void);
void             fake_storage_set_ready(bool ready);
void             fake_storage_fail_writes(bool fail);
int              fake_storage_writes(void);

static int s_pass;
static int s_fail;

static void check(bool ok, const char *what)
{
    if (ok) {
        s_pass++;
        printf("I (pedit)   ok   %s\n", what);
    } else {
        s_fail++;
        printf("E (pedit)   ОТКАЗ %s\n", what);
    }
}

static ipradio_preset_t *cell_of(ipradio_mode_t mode, int cell)
{
    return &ipradio_storage_bank(fake_storage_raw(), mode)[cell - 1];
}

static void put(ipradio_mode_t mode, int cell, const char *name, const char *url)
{
    ipradio_preset_t *p = cell_of(mode, cell);
    memset(p, 0, sizeof(*p));
    p->used = true;
    p->type = mode;
    snprintf(p->name, sizeof(p->name), "%s", name);
    if (url) {
        snprintf(p->url, sizeof(p->url), "%s", url);
    }
}

static void unlocked(const char *what)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "замок отпущен: %s", what);
    check(fake_storage_edit_depth() == 0, buf);
}

/* ------------------------------------------------------------------ */

static void test_rename(void)
{
    printf("I (pedit) переименование\n");
    fake_storage_reset();
    put(IPRADIO_MODE_NET, 2, "Старое", "http://a/2");

    check(ipradio_preset_rename(IPRADIO_MODE_NET, 2, "  Новое имя  ")
          == IPRADIO_PEDIT_OK, "переименование прошло");
    check(strcmp(cell_of(IPRADIO_MODE_NET, 2)->name, "Новое имя") == 0,
          "пробелы по краям срезаны");
    check(fake_storage_writes() == 1, "записано на карту один раз");
    unlocked("после переименования");

    check(ipradio_preset_rename(IPRADIO_MODE_NET, 2, "Новое имя")
          == IPRADIO_PEDIT_OK, "то же имя - тоже успех");
    check(fake_storage_writes() == 1, "то же имя на карту не пишется");
    unlocked("после того же имени");

    check(strcmp(cell_of(IPRADIO_MODE_NET, 2)->url, "http://a/2") == 0,
          "адрес не тронут");

    check(ipradio_preset_rename(IPRADIO_MODE_NET, 3, "Нечто")
          == IPRADIO_PEDIT_EMPTY, "свободную не переименовать");
    check(!cell_of(IPRADIO_MODE_NET, 3)->used, "свободная осталась свободной");
    unlocked("после свободной");

    check(ipradio_preset_rename(IPRADIO_MODE_NET, 0, "x") == IPRADIO_PEDIT_BAD_ARG,
          "ячейка 0 - отказ");
    check(ipradio_preset_rename(IPRADIO_MODE_NET, IPRADIO_PRESET_MAX + 1, "x")
          == IPRADIO_PEDIT_BAD_ARG, "ячейка за краем банка - отказ");
    check(ipradio_preset_rename(IPRADIO_MODE_NET, 2, NULL) == IPRADIO_PEDIT_BAD_ARG,
          "имя NULL - отказ");
    unlocked("после неверных аргументов");

    /* Длинное русское имя: 40 букв по два байта = 80 байт, а поле 64.
     * Резать надо по границе знака: 31 буква = 62 байта. */
    char longname[128] = "";
    for (int i = 0; i < 40; i++) {
        strcat(longname, "ж");
    }
    check(ipradio_preset_rename(IPRADIO_MODE_NET, 2, longname) == IPRADIO_PEDIT_OK,
          "длинное имя принято");
    check(strlen(cell_of(IPRADIO_MODE_NET, 2)->name) == 62,
          "длинное имя обрезано по границе буквы");

    /* Банки не смешиваются. */
    put(IPRADIO_MODE_FM, 2, "Эфирная", NULL);
    check(ipradio_preset_rename(IPRADIO_MODE_FM, 2, "Эфир новый") == IPRADIO_PEDIT_OK,
          "эфирная переименована");
    check(strcmp(cell_of(IPRADIO_MODE_NET, 2)->name, "Эфир новый") != 0,
          "интернетная с тем же номером не тронута");
}

static void test_clear(void)
{
    printf("I (pedit) очистка\n");
    fake_storage_reset();
    put(IPRADIO_MODE_NET, 4, "Удаляемая", "http://a/4");
    put(IPRADIO_MODE_NET, 5, "Соседка", "http://a/5");

    check(ipradio_preset_clear(IPRADIO_MODE_NET, 4) == IPRADIO_PEDIT_OK,
          "очистка прошла");
    check(!cell_of(IPRADIO_MODE_NET, 4)->used, "ячейка свободна");
    check(cell_of(IPRADIO_MODE_NET, 4)->url[0] == '\0', "адрес стёрт");
    check(cell_of(IPRADIO_MODE_NET, 5)->used, "соседняя не тронута");
    unlocked("после очистки");

    check(ipradio_preset_clear(IPRADIO_MODE_NET, 4) == IPRADIO_PEDIT_EMPTY,
          "повторная очистка - уже свободна");
    unlocked("после повторной");
}

static void test_put_net(void)
{
    printf("I (pedit) запись интернет-станции\n");
    fake_storage_reset();

    int cell = -1;
    check(ipradio_preset_put_net(0, "Silver Rain ", "http://s/1", "uuid-1", &cell)
          == IPRADIO_PEDIT_OK, "первая станция записана");
    check(cell == 1, "в первую свободную");
    check(strcmp(cell_of(IPRADIO_MODE_NET, 1)->name, "Silver Rain") == 0,
          "пробел на конце имени срезан");
    check(cell_of(IPRADIO_MODE_NET, 1)->type == IPRADIO_MODE_NET, "тип интернетный");
    check(!cell_of(IPRADIO_MODE_FM, 1)->used, "эфирный банк не тронут");
    unlocked("после записи");

    int writes = fake_storage_writes();
    check(ipradio_preset_put_net(0, "Silver Rain", "http://s/1", "uuid-1", &cell)
          == IPRADIO_PEDIT_OK && cell == 1, "та же станция - та же ячейка");
    check(fake_storage_writes() == writes, "второй копии не завелось, записи нет");
    unlocked("после повторной");

    check(ipradio_preset_put_net(0, "Вторая", "http://s/2", NULL, &cell)
          == IPRADIO_PEDIT_OK && cell == 2, "вторая - во вторую");

    /* Заполняем банк до конца и пробуем ещё одну. */
    for (int c = 3; c <= IPRADIO_PRESET_MAX; c++) {
        char url[32];
        snprintf(url, sizeof(url), "http://s/%d", c);
        ipradio_preset_put_net(0, "Ещё", url, NULL, NULL);
    }
    writes = fake_storage_writes();
    check(ipradio_preset_put_net(0, "Лишняя", "http://s/99", NULL, &cell)
          == IPRADIO_PEDIT_FULL, "банк полон - отказ");
    check(fake_storage_writes() == writes, "при полном банке ничего не записано");
    /* Та самая ошибка поиска до 2026-10-06. */
    unlocked("при полном банке");

    /* Замена: в указанную ячейку, даже если банк полон. */
    check(ipradio_preset_put_net(3, "Замена", "http://s/new", "uuid-n", &cell)
          == IPRADIO_PEDIT_OK && cell == 3, "замена в третью");
    check(strcmp(cell_of(IPRADIO_MODE_NET, 3)->url, "http://s/new") == 0,
          "адрес заменён");
    check(strcmp(cell_of(IPRADIO_MODE_NET, 3)->stationuuid, "uuid-n") == 0,
          "идентификатор каталога заменён");
    check(strcmp(cell_of(IPRADIO_MODE_NET, 4)->url, "http://s/4") == 0,
          "соседняя не тронута");
    unlocked("после замены");

    /* Неверные аргументы. */
    char longurl[IPRADIO_URL_MAX + 8];
    memset(longurl, 'a', sizeof(longurl) - 1);
    longurl[sizeof(longurl) - 1] = '\0';
    check(ipradio_preset_put_net(IPRADIO_PRESET_MAX + 1, "x", "http://x", NULL, NULL)
          == IPRADIO_PEDIT_BAD_ARG, "ячейка за краем - отказ");
    check(ipradio_preset_put_net(1, "x", NULL, NULL, NULL) == IPRADIO_PEDIT_BAD_ARG,
          "адрес NULL - отказ");
    check(ipradio_preset_put_net(1, "x", "", NULL, NULL) == IPRADIO_PEDIT_BAD_ARG,
          "пустой адрес - отказ");
    check(ipradio_preset_put_net(1, "x", longurl, NULL, NULL) == IPRADIO_PEDIT_BAD_ARG,
          "адрес длиннее поля - отказ, а не обрезка");
    unlocked("после неверных аргументов");
}

static void test_card_trouble(void)
{
    printf("I (pedit) беды с картой\n");
    fake_storage_reset();
    put(IPRADIO_MODE_NET, 1, "Есть", "http://a/1");

    fake_storage_set_ready(false);
    check(ipradio_preset_rename(IPRADIO_MODE_NET, 1, "Новое") == IPRADIO_PEDIT_NO_CARD,
          "без карты переименование - отказ");
    check(ipradio_preset_clear(IPRADIO_MODE_NET, 1) == IPRADIO_PEDIT_NO_CARD,
          "без карты очистка - отказ");
    check(ipradio_preset_put_net(0, "x", "http://x", NULL, NULL) == IPRADIO_PEDIT_NO_CARD,
          "без карты запись - отказ");
    check(strcmp(cell_of(IPRADIO_MODE_NET, 1)->name, "Есть") == 0,
          "без карты ничего не поменялось");
    unlocked("без карты");
    fake_storage_set_ready(true);

    fake_storage_fail_writes(true);
    check(ipradio_preset_rename(IPRADIO_MODE_NET, 1, "Новое")
          == IPRADIO_PEDIT_WRITE_FAILED, "отказ записи виден вызывающему");
    unlocked("после отказа записи");
    fake_storage_fail_writes(false);
}

bool test_preset_edit_run(void)
{
    s_pass = 0;
    s_fail = 0;

    printf("W (pedit) === правка ячеек банка ===\n");

    test_rename();
    test_clear();
    test_put_net();
    test_card_trouble();

    if (s_fail == 0) {
        printf("W (pedit) === сошлось всё: %d проверок ===\n", s_pass);
    } else {
        printf("E (pedit) === ОТКАЗОВ %d из %d ===\n", s_fail, s_pass + s_fail);
    }
    return s_fail == 0;
}
