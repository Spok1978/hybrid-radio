/* Запуск проверок на машине разработчика.
 *
 * Наборы:
 *   - формат хранилища (ipradio_selftest.c) — тот же код исполняется
 *     и на плате, под ключом IPRADIO_SELFTEST;
 *   - автомат состояния (test_state.c) — только здесь: на плате
 *     у него своя задача, а тесту нужен пошаговый разбор;
 *   - формат хранилища ещё раз, со своими выделениями cJSON
 *     (test_json_hooks.c) — так он работает на плате с 2026-10-06;
 *   - правка ячеек банка (test_preset_edit.c).
 *
 * Код возврата: 0 — всё сошлось, 1 — есть отказы.
 */
#include <stdbool.h>
#include <stdio.h>

#include "ipradio_selftest.h"

bool test_state_run(void);
bool test_json_hooks_run(void);
bool test_preset_edit_run(void);

int main(void)
{
    printf("=== проверки на машине разработчика ===\n");

    bool ok = true;
    ok &= ipradio_selftest_run();
    printf("\n");
    ok &= test_state_run();
    printf("\n");
    ok &= test_json_hooks_run();
    printf("\n");
    ok &= test_preset_edit_run();

    printf("\n%s\n", ok ? "ВСЁ СОШЛОСЬ" : "ЕСТЬ ОТКАЗЫ");
    return ok ? 0 : 1;
}
