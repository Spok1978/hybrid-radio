"""Посадочные места купленных модулей -> hardware/kicad/hybrid-radio.pretty/.

Номера площадок совпадают с номерами выводов символов из make_symbols.py.
Координаты — вид сверху, миллиметры, ось Y вниз (как в KiCad).

Откуда геометрия (подробно — docs/33, §13):

RRD-102 — официальная библиотека JLCEDA/EasyEDA, LCSC C9900205627,
    корпус COMM-SMD_L11.0-W11.0-P2.00-TL: два ряда по 5 площадок 2,0 × 1,2 мм,
    шаг 2,0 мм, ряды через 11,0 мм. Площадки 1-5 слева сверху вниз,
    6-10 справа снизу вверх.

GY-PCM5102A — модуль ставится в гнёзда-гребёнки 2,54 мм (решение
    2026-10-07). Шаг 2,54 подтверждён тем, что стандартная гребёнка 2,54
    из комплекта встала на все выводы. Взаимное положение рядов снято с фото
    продавца в долях шага и сверено с замером заказчика (≈7 мм между SCK и
    выводом «1» по штырям; по фото 6,1 мм центр-центр — расхождение ~0,4 мм).
    ПРОВЕРИТЬ печатью 1:1 до заказа плат.
"""

import sys
from pathlib import Path

OUT = Path(__file__).resolve().parents[1] / "kicad" / "hybrid-radio.pretty"
P = 2.54


def fmt(v):
    return f"{v:.3f}".rstrip("0").rstrip(".")


def rect(layer, x0, y0, x1, y1, width):
    return (f'\t(fp_rect (start {fmt(x0)} {fmt(y0)}) (end {fmt(x1)} {fmt(y1)})\n'
            f'\t\t(stroke (width {width}) (type solid)) (fill no) (layer "{layer}"))')


def text(kind, value, x, y, layer, hide=False):
    return (f'\t(property "{kind}" "{value}" (at {fmt(x)} {fmt(y)} 0) (layer "{layer}")'
            f'{" (hide yes)" if hide else ""}\n'
            f'\t\t(effects (font (size 1 1) (thickness 0.15))))')


def footprint(name, descr, attr, pads, body, note, holes=()):
    """pads: [(номер, x, y, тип)] где тип 'tht' (гнездо 2,54), 'wire'
    (провод 0,5 мм²: отверстие 0,9, площадка 2,1 — как Connector_Wire
    SolderWire-0.5sqmm в библиотеке KiCad) или ('smd', w, h).
    holes: [(x, y)] — крепёжные отверстия 3,2 мм без металлизации."""
    x0, y0, x1, y1 = body
    # Шелкография и курьёр — по корпусу и по площадкам, что выходят за него
    # (у RRD-102 полуотверстия на самом краю): линия шелкографии не должна
    # лежать на меди площадок (руководство разводки §9.2; DRC 2026-10-08 —
    # 10 «silk_over_copper» на U3). Зазор линии до площадки 0,2 мм.
    half = {"tht": 1.0, "wire": 1.05}
    ext = [(x - half[k], y - half[k], x + half[k], y + half[k]) if k in half
           else (x - k[1] / 2, y - k[2] / 2, x + k[1] / 2, y + k[2] / 2) for _n, x, y, k in pads]
    gap = 0.2 + 0.06
    sx0 = min([x0 - 0.12] + [e[0] - gap for e in ext if e[0] < x0])
    sy0 = min([y0 - 0.12] + [e[1] - gap for e in ext if e[1] < y0])
    sx1 = max([x1 + 0.12] + [e[2] + gap for e in ext if e[2] > x1])
    sy1 = max([y1 + 0.12] + [e[3] + gap for e in ext if e[3] > y1])
    lines = [f'(footprint "{name}"', '\t(version 20260206)', '\t(generator "make_footprints.py")',
             '\t(layer "F.Cu")', f'\t(descr "{descr}")', f'\t(attr {attr})',
             text("Reference", "REF**", (x0 + x1) / 2, sy0 - 1.0, "F.SilkS"),
             text("Value", name, (x0 + x1) / 2, sy1 + 1.0, "F.Fab"),
             text("Datasheet", "", 0, 0, "F.Fab", hide=True),
             rect("F.Fab", x0, y0, x1, y1, 0.1),
             rect("F.SilkS", sx0, sy0, sx1, sy1, 0.12),
             rect("F.CrtYd", sx0 - 0.38, sy0 - 0.38, sx1 + 0.38, sy1 + 0.38, 0.05)]
    for num, x, y, kind in pads:
        if kind in ("tht", "wire"):
            shape = "rect" if num == 1 else "circle"
            # Гнёзда 2,54: площадка 2,0 при сверле 1,0 — кольцо 0,5 для домашнего
            # травления; зазор между соседними 0,54 мм.
            size, drill = ("2", "1") if kind == "tht" else ("2.1", "0.9")
            lines.append(f'\t(pad "{num}" thru_hole {shape} (at {fmt(x)} {fmt(y)}) (size {size} {size}) '
                         f'(drill {drill}) (layers "*.Cu" "*.Mask"))')
        else:
            _, w, h = kind
            lines.append(f'\t(pad "{num}" smd rect (at {fmt(x)} {fmt(y)}) (size {fmt(w)} {fmt(h)}) '
                         f'(layers "F.Cu" "F.Paste" "F.Mask"))')
    for x, y in holes:
        lines.append(f'\t(pad "" np_thru_hole circle (at {fmt(x)} {fmt(y)}) (size 3.2 3.2) '
                     f'(drill 3.2) (layers "*.Cu" "*.Mask"))')
    lines.append(")")
    path = OUT / f"{name}.kicad_mod"
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return path


def rrd102():
    pads = [(i + 1, -5.5, -4.0 + 2.0 * i, ("smd", 2.0, 1.2)) for i in range(5)]
    pads += [(6 + i, 5.5, 4.0 - 2.0 * i, ("smd", 2.0, 1.2)) for i in range(5)]
    # Вид совпадает со стороной микросхемы: та же раскладка 1-5 слева сверху
    # вниз, 10-6 справа — в реверсе модуля (briankhuu.com, 2015, вид {FRONT}
    # со стороны RDA5807M). Значит паять МИКРОСХЕМОЙ ВВЕРХ.
    return footprint("RRD-102", "RDA5807M module RRD-102 V2.0, castellated 2.0 mm pitch, "
                     "MOUNT CHIP SIDE UP; pads from JLCEDA/EasyEDA official library "
                     "LCSC C9900205627", "smd",
                     pads, (-5.5, -5.5, 5.5, 5.5), "RRD-102 V2.0, pads per LCSC C9900205627")


def gy_pcm5102a():
    # Начало координат — вывод VIN (верхний в ряду из 6 у правого края).
    # Ряд из 6: VIN, GND, LCK, DIN, BCK, SCK вниз по шагу.
    pads = [(i + 1, 0.0, P * i, "tht") for i in range(6)]
    # Нижний ряд из 9: вывод «1» (FLT, наш номер 15) на 6,43 мм левее столбца
    # из 6 и на 0,43 мм ниже SCK. Сняты по центрам отверстий на фото продавца:
    # шаг 39,3 px = 2,54 мм, SCK (719,3; 409,5) px, «1» (620; 416,3) px ->
    # 2,53 и 0,17 шага. Сходится с замером заказчика: 7,13 мм по штырям минус
    # ширина штыря 0,64 = 6,49 мм против sqrt(6,43^2 + 0,43^2) = 6,44 мм
    # (самопроверка 2026-10-07; первая оценка 2,4 / 0,25 шага была неточной).
    # Слева направо: L G R G A3V3 FMT XSMT DEMP FLT = наши номера 7..15.
    row_y = 5 * P + 0.43
    x_flt = -6.43
    for k, num in enumerate(range(15, 6, -1)):       # 15 справа ... 7 слева
        pads.append((num, x_flt - P * k, row_y, "tht"))
    # Контур — по габариту 36,9 × 22,4 от заказчика, привязан к правому верхнему
    # выводу; используется только как зона запрета для соседей, с запасом.
    body = (2.5 - 36.9, -3.2, 2.5, -3.2 + 22.4)
    return footprint("GY-PCM5102A", "PCM5102A module GY-PCM5102A (TZT) in 2.54 mm socket strips; "
                     "row offset from seller photo, VERIFY WITH 1:1 PRINT", "through_hole",
                     pads, body, "GY-PCM5102A in sockets, verify 1:1 print")


def mcp23017_cjmcu():
    """MCP23017 CJMCU в гнёздах 2,54 мм (решение 2026-10-07).

    Вид сверху (сторона микросхемы). Подписи на модуле — с обратной
    стороны: там одиночный ряд слева, двойной справа, столбец A внешний
    (заказчик). Сверху — зеркально: двойной ряд слева (A — крайний),
    одиночный справа. Ряды строго напротив друг друга (заказчик): строка i
    одиночного ряда = строка i столбцов B и A.

    Расстояние одиночный ряд — столбец A: замер линейкой 18,57 мм. Ближайшая
    точка сетки 2,54 — 7 шагов = 17,78 мм (8 шагов = 20,32 уже не влезает в
    ширину 21,4 мм по фото продавца). Столбец B — 6 шагов, 15,24 мм.
    ПРОВЕРИТЬ печатью 1:1."""
    pads = []
    for i in range(10):
        y = P * i
        pads.append((1 + i, 0.0, y, "tht"))          # одиночный ряд: A2 ... VCC
        pads.append((11 + i, -6 * P, y, "tht"))      # столбец B: VCC, INTB, GPB0..7
        pads.append((21 + i, -7 * P, y, "tht"))      # столбец A: GND, INTA, GPA0..7
    pads.sort()
    mx = (21.4 - 7 * P) / 2          # поля по ширине модуля 21,4 мм (фото продавца)
    my = (26.4 - 9 * P) / 2          # поля по длине 26,4 мм
    body = (-7 * P - mx, -my, mx, 9 * P + my)
    return footprint("MCP23017-CJMCU", "MCP23017 module CJMCU in 2.54 mm socket strips; "
                     "single row 7 pitches from column A, VERIFY WITH 1:1 PRINT", "through_hole",
                     pads, body, "MCP23017 CJMCU in sockets, verify 1:1 print")


def weact_sy8205():
    """WeAct SY8205 на двух стойках M2 по диагонали, выводы — короткими
    проводами (решение 2026-10-07: штырей модуль не предусматривает,
    у каждого вывода площадка с двумя отверстиями под провод).

    Вид сверху: IN слева (− сверху, + снизу), OUT справа (− сверху, + снизу),
    отверстия — у OUT− (правое верхнее) и у IN+ (левое нижнее); по фото
    продавца. Между центрами отверстий 15,4 мм вдоль длинной стороны и
    14,6 мм вдоль короткой (замер линейкой). Симметрию отверстий
    относительно центра модуля принимаю — проверить печатью 1:1.
    Отверстия 3,2 мм под M2 — запас ±0,6 мм на неточность замера.

    Площадки под провода 0,5 мм² — снаружи контура, напротив выводов,
    чтобы паять после установки модуля. Контур 32,3 × 25 мм — замер
    заказчика (вероятно завышен, для зоны запрета это запас)."""
    hx, hy = 15.4 / 2, 14.6 / 2
    bw, bh = 32.3 / 2, 25.0 / 2
    px = bw + 3.0
    pads = [(1, -px, 5.0, "wire"), (2, -px, -5.0, "wire"),     # IN+, IN-
            (3, px, 5.0, "wire"), (4, px, -5.0, "wire"),       # OUT+, OUT-
            (5, -bw + 8.0, -bh - 3.0, "wire")]                 # EN, не используется
    holes = [(hx, -hy), (-hx, hy)]
    return footprint("WeAct-SY8205", "WeAct SY8205 buck module on two M2 standoffs, wired; "
                     "hole pitch 15.4 x 14.6 mm, VERIFY WITH 1:1 PRINT", "through_hole",
                     pads, (-bw, -bh, bw, bh), "WeAct SY8205 on M2 standoffs, wired",
                     holes=holes)


def hw327():
    """HW-327 (TPA3118, моно) на четырёх стойках, выводы — проводами
    (решение 2026-10-07). Отверстия по углам: центры 29,2 мм по короткой
    стороне и 39,8 мм по длинной, диаметр 2,55 мм (замер заказчика).
    Контур 40,5 × 51,2 мм — замер заказчика (для зоны запрета).

    Вид сверху (сторона деталей, фото продавца): сверху слева MUTE, сверху
    справа INPUT, снизу слева POWER, снизу справа OUT. Площадки под провода
    0,5 мм² — снаружи контура напротив этих групп. Какая из площадок MUTE —
    сигнал, а какая земля, ещё не прозвонено (docs/33, §11)."""
    bw, bh = 40.5 / 2, 51.2 / 2
    hx, hy = 29.2 / 2, 39.8 / 2
    top, bot = -bh - 3.0, bh + 3.0
    pads = [(1, -bw + 6, bot, "wire"), (2, -bw + 11, bot, "wire"),      # PWR+, PWR-
            (4, bw - 11, bot, "wire"), (3, bw - 6, bot, "wire"),        # OUT-, OUT+
            (7, -bw + 6, top, "wire"), (8, -bw + 11, top, "wire"),      # MUTE_SD, MUTE_GND
            (6, bw - 11, top, "wire"), (5, bw - 6, top, "wire")]        # IN-, IN+
    holes = [(sx * hx, sy * hy) for sx in (-1, 1) for sy in (-1, 1)]
    return footprint("HW-327", "HW-327 TPA3118 mono amplifier module on four standoffs, wired; "
                     "hole pitch 29.2 x 39.8 mm, module holes 2.55 mm", "through_hole",
                     sorted(pads), (-bw, -bh, bw, bh), "HW-327 on standoffs, wired",
                     holes=holes)


def mounting_hole_m3():
    """Крепёжное отверстие платы M3 (3,2 мм без металлизации). Своё, а не
    MountingHole_3.2mm_M3 из библиотеки KiCad: у того надпись H1-H4 на
    шелкографии вылезает за край платы, а скрыть её через pcbnew API
    не удалось (make_board.py, 2026-10-07). Здесь надпись на слое F.Fab."""
    lines = ['(footprint "MountingHole_M3_FabRef"', '	(version 20260206)',
             '	(generator "make_footprints.py")', '	(layer "F.Cu")',
             '	(descr "Mounting hole 3.2 mm M3, NPTH, reference on F.Fab")', '	(attr board_only exclude_from_pos_files exclude_from_bom)',
             text("Reference", "REF**", 0, 0, "F.Fab"),
             text("Value", "M3", 0, 2.5, "F.Fab", hide=True),
             '	(fp_circle (center 0 0) (end 3 0) (stroke (width 0.05) (type solid)) (fill no) (layer "F.CrtYd"))',
             '	(pad "" np_thru_hole circle (at 0 0) (size 3.2 3.2) (drill 3.2) (layers "*.Cu" "*.Mask"))',
             ")"]
    path = OUT / "MountingHole_M3_FabRef.kicad_mod"
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return path


def check_against_symbols():
    """Номера площадок каждого посадочного места = номера выводов символа."""
    sys.path.insert(0, str(Path(__file__).parent))
    from make_symbols import SYMBOLS
    errors = []
    for name, _ref, _desc, left, right in SYMBOLS:
        fp = OUT / f"{name}.kicad_mod"
        if name in ("HT7333-A", "ESP32-P4-Board"):
            continue                     # TO-92 из библиотеки KiCad; P4 — внешнее устройство, без места
        if not fp.exists():
            errors.append(f"{name}: нет посадочного места")
            continue
        import re
        pads = set(re.findall(r'\(pad "(\d+)"', fp.read_text(encoding="utf-8")))
        pins = {str(n) for n, _ in left + right}
        if pads != pins:
            errors.append(f"{name}: выводы {sorted(pins - pads, key=int)} без площадок, "
                          f"площадки {sorted(pads - pins, key=int)} без выводов")
    return errors


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    OUT.mkdir(parents=True, exist_ok=True)
    for make in (rrd102, gy_pcm5102a, mcp23017_cjmcu, weact_sy8205, hw327, mounting_hole_m3):
        print(make())
    errors = check_against_symbols()
    print("площадки = выводы символов: " + ("всё сошлось" if not errors else "; ".join(errors)))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
