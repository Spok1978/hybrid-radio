/*
 * fake_deps.c — мелкие соседи автомата.
 *
 * Сторож и тюнер автомату нужны как факт, а не как поведение:
 * он их зовёт, но от результата не зависит. Поэтому здесь пустышки.
 */

#include "ipradio_watchdog.h"

int ipradio_watchdog_register(const char *name, uint32_t limit_ms,
                              uint32_t flags)
{
    (void) name; (void) limit_ms; (void) flags;
    return 0;
}

void ipradio_watchdog_feed(int id) { (void) id; }
