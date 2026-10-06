/*
 * test_state.c — проверки автомата состояния.
 *
 * Идут только на машине разработчика: на плате у автомата своя
 * задача и своя очередь, а здесь мы подаём событие и тут же смотрим,
 * что стало с состоянием. Без планировщика, без ожидания,
 * без непредсказуемости.
 *
 * Хранилище подменено на то, что в памяти (fakes/fake_storage.c):
 * карта для проверки логики не нужна, а мешала бы.
 */

#include <stdio.h>
#include <string.h>

#include "ipradio_state.h"
#include "ipradio_storage.h"

/* Прокрутить очередь вручную. Собирается только при IPRADIO_HOST_TEST. */
void ipradio_state_pump(void);

/* Из поддельного хранилища. */
void             fake_storage_reset(void);
ipradio_store_t *fake_storage_raw(void);

static int s_pass;
static int s_fail;

static void check(bool ok, const char *what)
{
    if (ok) {
        s_pass++;
        printf("I (state)   ok   %s\n", what);
    } else {
        s_fail++;
        printf("E (state)   ОТКАЗ %s\n", what);
    }
}

/* Подать событие и сразу его разобрать. */
static void post(ipradio_event_type_t type, int32_t arg)
{
    ipradio_post_simple(type, arg);
    ipradio_state_pump();
}

static ipradio_snapshot_t now(void)
{
    ipradio_snapshot_t s;
    ipradio_get(&s);
    return s;
}

/* Завести станцию в банке. */
static void put_station(ipradio_mode_t mode, int cell, const char *name)
{
    ipradio_preset_t *bank = ipradio_storage_bank(fake_storage_raw(), mode);
    ipradio_preset_t *p = &bank[cell - 1];

    memset(p, 0, sizeof(*p));
    p->used = true;
    p->type = mode;
    snprintf(p->name, IPRADIO_NAME_MAX, "%s", name);

    if (mode == IPRADIO_MODE_FM) {
        p->band     = IPRADIO_BAND_CCIR;
        p->freq_khz = 100000 + (uint32_t) cell * 100;
    } else {
        snprintf(p->url, IPRADIO_URL_MAX, "http://example/%d", cell);
    }
}

/* ------------------------------------------------------------------ *
 *  1. Громкость
 * ------------------------------------------------------------------ */

static void test_volume(void)
{
    printf("I (state) громкость\n");

    post(IPRADIO_EV_VOLUME_SET, 70);
    check(now().volume == 70, "ползунок задаёт уровень");

    post(IPRADIO_EV_VOLUME_DELTA, 10);
    check(now().volume == 80, "регулятор прибавляет");

    /* За края шкалы уходить нельзя. */
    post(IPRADIO_EV_VOLUME_SET, 200);
    check(now().volume == 100, "выше ста не поднимается");

    post(IPRADIO_EV_VOLUME_SET, -5);
    check(now().volume == 0, "ниже нуля не опускается");

    /* Поворот регулятора снимает приглушение: иначе человек крутит,
     * ничего не слышит, а при снятии mute получается неожиданно
     * громко (§6.3). */
    post(IPRADIO_EV_VOLUME_SET, 50);
    post(IPRADIO_EV_MUTE_TOGGLE, 0);
    check(now().muted, "приглушение включилось");

    post(IPRADIO_EV_VOLUME_DELTA, 5);
    check(!now().muted, "поворот регулятора снял приглушение");
    check(now().volume == 55, "и уровень при этом поднялся");
}

/* ------------------------------------------------------------------ *
 *  2. Пресеты работают только в своём режиме
 * ------------------------------------------------------------------ */

static void test_preset_banks(void)
{
    printf("I (state) банки пресетов\n");

    fake_storage_reset();
    put_station(IPRADIO_MODE_FM,  1, "Эфирная раз");
    put_station(IPRADIO_MODE_FM,  2, "Эфирная два");
    put_station(IPRADIO_MODE_NET, 1, "Сетевая раз");

    /* Начинаем в эфире. */
    ipradio_snapshot_t s = now();
    if (s.mode != IPRADIO_MODE_FM) {
        post(IPRADIO_EV_MODE_TOGGLE, 0);
    }
    check(now().mode == IPRADIO_MODE_FM, "начинаем в эфире");

    post(IPRADIO_EV_PRESET_PRESSED, 1);
    check(now().mode == IPRADIO_MODE_FM,
          "нажатие пресета НЕ переключило режим");
    check(now().active_preset == 1, "встали на первую эфирную");
    /* У эфирной станции имя ложится в поле RDS, а не в station_name:
     * последнее занято именем интернет-станции. Заглушка до первого
     * настоящего RDS, признак rds_valid при этом не ставится. */
    check(strcmp(now().rds_name, "Эфирная раз") == 0,
          "имя станции из эфирного банка");
    check(!now().rds_valid, "имя из ячейки не выдаётся за данные эфира");

    /* Вторая ячейка интернетного банка пуста - в эфире её нажатие
     * ничего не должно менять, даже если в сети что-то есть. */
    post(IPRADIO_EV_PRESET_PRESSED, 2);
    check(strcmp(now().rds_name, "Эфирная два") == 0,
          "вторая ячейка тоже из эфирного банка");
}

/* ------------------------------------------------------------------ *
 *  3. Переключение режима помнит свою ячейку
 * ------------------------------------------------------------------ */

static void test_mode_keeps_preset(void)
{
    printf("I (state) своя ячейка у каждого режима\n");

    fake_storage_reset();
    put_station(IPRADIO_MODE_FM,  3, "Эфирная три");
    put_station(IPRADIO_MODE_NET, 1, "Сетевая раз");

    /* Сеть должна быть доступна, иначе переключение заблокируют. */
    post(IPRADIO_EV_NET_STATE, IPRADIO_NET_CONNECTED);

    if (now().mode != IPRADIO_MODE_FM) {
        post(IPRADIO_EV_MODE_TOGGLE, 0);
    }
    post(IPRADIO_EV_PRESET_PRESSED, 3);
    check(now().active_preset == 3, "в эфире стоим на третьей");

    post(IPRADIO_EV_MODE_TOGGLE, 0);
    check(now().mode == IPRADIO_MODE_NET, "ушли в интернет");

    /* ВОТ РАДИ ЧЕГО ЭТА ПРОВЕРКА.
     *
     * До 2026-09-15 номер ячейки был общий на оба режима. После
     * разделения банков он остался бы указывать на третью ячейку
     * ИНТЕРНЕТНОГО банка - то есть на станцию, которую человек
     * не выбирал, и мост включил бы именно её. Ошибку нашли руками,
     * и вот проверка, чтобы она не вернулась. */
    check(now().active_preset != 3,
          "номер ячейки не утёк из эфирного банка");

    post(IPRADIO_EV_PRESET_PRESSED, 1);
    check(now().active_preset == 1, "в интернете встали на первую");

    post(IPRADIO_EV_MODE_TOGGLE, 0);
    check(now().mode == IPRADIO_MODE_FM, "вернулись в эфир");
    check(now().active_preset == 3, "эфир помнит свою третью");

    post(IPRADIO_EV_MODE_TOGGLE, 0);
    check(now().active_preset == 1, "интернет помнит свою первую");
}

/* ------------------------------------------------------------------ *
 *  4. Без сети в интернет не пускаем
 * ------------------------------------------------------------------ */

static void test_mode_denied(void)
{
    printf("I (state) переключение без сети\n");

    fake_storage_reset();

    post(IPRADIO_EV_NET_STATE, IPRADIO_NET_CONNECTED);
    if (now().mode != IPRADIO_MODE_FM) {
        post(IPRADIO_EV_MODE_TOGGLE, 0);
    }

    post(IPRADIO_EV_NET_STATE, IPRADIO_NET_DISCONNECTED);

    uint16_t seq_before = now().mode_denied_seq;
    post(IPRADIO_EV_MODE_TOGGLE, 0);

    check(now().mode == IPRADIO_MODE_FM, "остались в эфире");
    check(now().mode_denied_seq != seq_before,
          "счётчик отказов поднялся — интерфейсу есть о чём сказать");

    /* Обратно в эфир можно всегда, даже без сети. */
    post(IPRADIO_EV_NET_STATE, IPRADIO_NET_CONNECTED);
    post(IPRADIO_EV_MODE_TOGGLE, 0);
    check(now().mode == IPRADIO_MODE_NET, "с сетью пустило");

    post(IPRADIO_EV_NET_STATE, IPRADIO_NET_DISCONNECTED);
    post(IPRADIO_EV_MODE_TOGGLE, 0);
    check(now().mode == IPRADIO_MODE_FM, "обратно в эфир без сети можно");
}

/* ------------------------------------------------------------------ *
 *  5. Долгое нажатие пишет в банк своего режима
 * ------------------------------------------------------------------ */

static void test_preset_hold(void)
{
    printf("I (state) запись пресета удержанием\n");

    fake_storage_reset();

    post(IPRADIO_EV_NET_STATE, IPRADIO_NET_CONNECTED);
    if (now().mode != IPRADIO_MODE_FM) {
        post(IPRADIO_EV_MODE_TOGGLE, 0);
    }

    post(IPRADIO_EV_FREQ_DELTA, 0);          /* встанем на текущую */
    post(IPRADIO_EV_PRESET_HOLD, 4);

    const ipradio_preset_t *fm =
        ipradio_storage_bank(fake_storage_raw(), IPRADIO_MODE_FM);
    const ipradio_preset_t *net =
        ipradio_storage_bank(fake_storage_raw(), IPRADIO_MODE_NET);

    check(fm[3].used, "записалось в эфирный банк");
    check(fm[3].type == IPRADIO_MODE_FM, "тип эфирный");
    check(!net[3].used, "интернетный банк не тронут");
}

/* ------------------------------------------------------------------ *
 *  6. Ячейку поправили с экрана: имя на экран, поток не трогаем
 * ------------------------------------------------------------------ */

static void test_preset_edited(void)
{
    printf("I (state) правка ячейки с экрана\n");

    fake_storage_reset();
    put_station(IPRADIO_MODE_NET, 2, "Старое");
    put_station(IPRADIO_MODE_FM,  3, "Эфирная три");

    post(IPRADIO_EV_NET_STATE, IPRADIO_NET_CONNECTED);
    if (now().mode != IPRADIO_MODE_FM) {
        post(IPRADIO_EV_MODE_TOGGLE, 0);
    }
    post(IPRADIO_EV_PRESET_PRESSED, 3);          /* эфир помнит третью */
    post(IPRADIO_EV_MODE_TOGGLE, 0);
    post(IPRADIO_EV_PRESET_PRESSED, 2);
    check(now().mode == IPRADIO_MODE_NET && now().active_preset == 2,
          "в интернете играет вторая");

    uint16_t seq = now().preset_seq;

    /* Переименование той, что играет. */
    snprintf(ipradio_storage_bank(fake_storage_raw(), IPRADIO_MODE_NET)[1].name,
             IPRADIO_NAME_MAX, "%s", "Новое");
    post(IPRADIO_EV_PRESET_EDITED, IPRADIO_PRESET_ARG(IPRADIO_MODE_NET, 2));
    check(strcmp(now().station_name, "Новое") == 0,
          "новое имя на экране сразу");
    check(now().preset_seq == seq,
          "поток не перезапущен: счётчик нажатий тот же");
    check(now().active_preset == 2, "отметка на месте");

    /* Очистка той, что играет: звук идёт, отметка остаётся. */
    memset(&ipradio_storage_bank(fake_storage_raw(), IPRADIO_MODE_NET)[1], 0,
           sizeof(ipradio_preset_t));
    post(IPRADIO_EV_PRESET_EDITED, IPRADIO_PRESET_ARG(IPRADIO_MODE_NET, 2));
    check(now().active_preset == 2 && now().preset_seq == seq,
          "очистка играющей не трогает поток");

    /* Уходим в эфир и возвращаемся: на пустую вставать нельзя. */
    post(IPRADIO_EV_MODE_TOGGLE, 0);
    check(now().mode == IPRADIO_MODE_FM && now().active_preset == 3,
          "в эфире своя третья");
    post(IPRADIO_EV_MODE_TOGGLE, 0);
    check(now().mode == IPRADIO_MODE_NET, "снова в интернете");
    check(now().active_preset < 1,
          "на очищенную ячейку не встали");
    check(now().play == IPRADIO_PLAY_IDLE,
          "без станции не висит «буферизация»");

    /* Очистка ячейки ДРУГОГО режима, которую тот помнит. */
    memset(&ipradio_storage_bank(fake_storage_raw(), IPRADIO_MODE_FM)[2], 0,
           sizeof(ipradio_preset_t));
    post(IPRADIO_EV_PRESET_EDITED, IPRADIO_PRESET_ARG(IPRADIO_MODE_FM, 3));
    post(IPRADIO_EV_MODE_TOGGLE, 0);
    check(now().mode == IPRADIO_MODE_FM && now().active_preset < 1,
          "эфир забыл очищенную третью");
}

/* ------------------------------------------------------------------ *
 *  7. Включить интернетную ячейку из эфира
 * ------------------------------------------------------------------ */

static void test_net_preset_from_fm(void)
{
    printf("I (state) интернетная ячейка из эфира\n");

    fake_storage_reset();
    put_station(IPRADIO_MODE_FM,  4, "Эфирная четыре");
    put_station(IPRADIO_MODE_NET, 4, "Сетевая четыре");

    post(IPRADIO_EV_NET_STATE, IPRADIO_NET_CONNECTED);
    if (now().mode != IPRADIO_MODE_FM) {
        post(IPRADIO_EV_MODE_TOGGLE, 0);
    }
    post(IPRADIO_EV_PRESET_PRESSED, 4);
    check(now().mode == IPRADIO_MODE_FM && now().active_preset == 4,
          "в эфире играет четвёртая эфирная");

    /* ВОТ РАДИ ЧЕГО ЭТА ПРОВЕРКА: прежде мост при загрузке и поиск
     * по каталогу слали PRESET_PRESSED, и из эфира включалась
     * эфирная ячейка с тем же номером. */
    uint16_t seq = now().preset_seq;
    post(IPRADIO_EV_NET_PRESET_PRESSED, 4);
    check(now().mode == IPRADIO_MODE_NET, "перешли в интернет");
    check(now().active_preset == 4, "встали на четвёртую");
    check(strcmp(now().station_name, "Сетевая четыре") == 0,
          "играет интернетная, а не эфирная с тем же номером");
    check((uint16_t) (now().preset_seq - seq) == 1,
          "мост запустит поток ровно один раз");

    post(IPRADIO_EV_MODE_TOGGLE, 0);
    check(now().active_preset == 4 &&
          strcmp(now().rds_name, "Эфирная четыре") == 0,
          "эфир при возврате помнит свою четвёртую");

    /* Пустая интернетная ячейка: ни режима, ни нажатия. */
    seq = now().preset_seq;
    post(IPRADIO_EV_NET_PRESET_PRESSED, 6);
    check(now().mode == IPRADIO_MODE_FM && now().preset_seq == seq,
          "пустая ячейка ничего не меняет");

    /* Без сети - как у кнопки MODE: остаёмся в эфире, есть повод
     * для диалога. */
    post(IPRADIO_EV_NET_STATE, IPRADIO_NET_DISCONNECTED);
    uint16_t denied = now().mode_denied_seq;
    post(IPRADIO_EV_NET_PRESET_PRESSED, 4);
    check(now().mode == IPRADIO_MODE_FM, "без сети остались в эфире");
    check(now().mode_denied_seq != denied, "интерфейсу есть о чём сказать");
}

/* ------------------------------------------------------------------ *
 *  Запуск
 * ------------------------------------------------------------------ */

bool test_state_run(void)
{
    s_pass = 0;
    s_fail = 0;

    printf("W (state) === проверки автомата состояния ===\n");

    fake_storage_reset();
    ipradio_state_init();

    test_volume();
    test_preset_banks();
    test_mode_keeps_preset();
    test_mode_denied();
    test_preset_hold();
    test_preset_edited();
    test_net_preset_from_fm();

    if (s_fail == 0) {
        printf("W (state) === сошлось всё: %d проверок ===\n", s_pass);
    } else {
        printf("E (state) === ОТКАЗОВ %d из %d ===\n",
               s_fail, s_pass + s_fail);
    }
    return s_fail == 0;
}
