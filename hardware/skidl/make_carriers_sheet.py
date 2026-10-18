"""Лист для макетирования и проверок: все три доп. платы на одном листе.

    .venv/Scripts/python hardware/skidl/make_carriers_sheet.py

-> hardware/kicad/carriers/hybrid-radio-carriers.{kicad_pro,kicad_sch}

Отличия от Hybryd Radio.kicad_sch (make_schematic.py):
- три зоны в рамках: ПИТАНИЕ / СИГНАЛЬНАЯ / УСИЛИТЕЛИ;
- силовые цепи (+12V, +5V, +3V3_FM, +3V3_ESP, GND) — по одному символу
  питания на зону, провода к нему от всех выводов цепи;
- плата P4 — символом «внешнее устройство» (P4): гребёнка J3 1:1 со шлейфом
  и вход USB-C, куда по кабелю приходит 5 В с J2;
- R14 и R15 — DNP (ставится один, см. заметку у блока);
- монтажные заметки и порядок поэтапного включения.

Соединения внутри зоны — проводами (sch_router.py: ортогональная трассировка
по сетке 1,27 мм, чужие цепи только пересекаются насквозь, без узлов), у
каждой цепи — метка с именем. Силовые цепи внутри зоны тоже проводами, к
одному символу питания на зону. Между зонами цепи связаны одноимёнными
метками (так требует промпт). Сверка — compare_netlists.py по выгрузке KiCad.

Цепи берутся из нетлиста SKiDL (hardware/kicad/Hybryd Radio.net) — того же,
из которого составлен перечень docs/35-carrier-boards-parts.md.
"""

import json
import sys
from collections import OrderedDict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import make_schematic as ms  # noqa: E402
import sch_router  # noqa: E402
from make_schematic import CHAR_W, GRID, new_uuid, quote, snap  # noqa: E402

OUT_DIR = ms.KICAD_DIR / "carriers"
NAME = "hybrid-radio-carriers"
SCH = OUT_DIR / f"{NAME}.kicad_sch"

MARGIN = 20.0
ZONE_PAD = 8.0

ZONES = [
    ("ПИТАНИЕ — плата питания (125 x 70, медь снизу)",
     ["power_input1", "supply_5v1", "indication_power1", "links_power1"]),
    ("СИГНАЛЬНАЯ ПЛАТА (230 x 70, два слоя) и плата P4",
     ["p4", "ribbon1", "supply_3v3_tuner1", "tuner1", "dac1", "mixer_channel1", "mixer_channel2",
      "amp_mute1", "expander1", "controls1", "indication_signal1", "links_signal1"]),
    ("УСИЛИТЕЛИ — площадка усилителей (150 x 70)",
     ["amp_channel1", "amp_channel2", "links_amp1"]),
]

BLOCK_TITLES = dict(ms.BLOCK_TITLES)
BLOCK_TITLES["p4"] = "Плата P4 (внешнее устройство): гребёнка J3 и вход USB-C"

NOTES = dict(ms.NOTES)
NOTES["p4"] = ["Шлейф IDC 40 к J3 сигнальной платы: контакт в контакт (1:1)",
               "USB-C (41, 42) — кабель USB-A -> USB-C от J2 платы питания",
               "Контакты 1, 3 (VCC_5V) и прочие с крестиком не подключать"]
NOTES["amp_mute1"] = ["R14 и R15 — DNP: впаять ОДИН. Площадка MUTE HW-327 идёт на",
                      "вывод 2 TPA3118 (SDZ, LOW — выкл.) -> R14 на GND;",
                      "на вывод 12 (MUTE, HIGH — выкл.) -> R15 на +3V3_ESP. Выяснить прозвонкой"]

# Силовые цепи -> символ питания KiCad (значение = имя цепи).
POWER = {"+12V": "+12V", "+5V": "+5V", "GND": "GND", "+3V3_FM": "+3V3", "+3V3_ESP": "+3V3"}
DNP = {"R14", "R15"}

GENERAL_NOTES = [
    "#МОНТАЖ: СОЕДИНЕНИЯ МЕЖДУ ПЛАТАМИ",
    "1. P4 <-> сигнальная плата: шлейф IDC 2x20 на J3, контакт в контакт.",
    "2. Плата питания J2 (USB-A) -> P4 USB-C: кабель USB-A -> USB-C (питание и заряд батареи P4).",
    "3. Плата питания J14 -> сигнальная J15: три провода +5V, GND, PWR_SENSE (контакт 1-1, 2-2, 3-3).",
    "4. Плата питания J13 -> площадка усилителей J17: два провода +12V, GND, сечением на ток до 4 А.",
    "5. Сигнальная J16 -> A1 (левый): AMP_L_INP -> IN+, AMP_L_INN -> IN-; A2 (правый): AMP_R_INP, AMP_R_INN;",
    "   AMP_SD -> площадки MUTE обоих модулей, GND -> MUTE_GND. A1/A2 OUT+/OUT- -> J5/J6 -> динамики 4 Ом.",
    "6. U1 (SY8205), A1, A2 — на стойках, провода к площадкам. Энкодеры и кнопки — провода к J7, J9-J12.",
    "",
    "#ПЕРЕД ПЕРВОЙ ПОДАЧЕЙ 12 В",
    "- Прозвонить +12V-GND (TP1-TP3) и +5V-GND (TP2-TP3): КЗ нет (сопротивление растёт — заряжаются C1, C4).",
    "- Модули U3, U4, U5 не установлены, шлейф и кабель USB не подключены, усилители не подключены.",
    "",
    "#ПОЭТАПНОЕ ВКЛЮЧЕНИЕ (БП 12 В / 5 А на J1)",
    "1. Плата питания: TP1 (+12V) ~12 В, горит D3; TP2 (+5V) = 5,0 В, горит D4.",
    "2. J14 -> J15: TP4 (+3V3_FM) = 3,3 В; TP5 (PWR_SENSE) = 12 x 12k / (39k + 12k) = 2,82 В.",
    "3. Без питания: впаять U3, вставить U4 и U5; шлейф к P4, кабель USB-A -> USB-C. Включить, P4 загружается.",
    "4. Площадка усилителей: J13 -> J17, на J17 ~12 В; впаян R14 или R15; затем динамики на J5, J6.",
    "5. Звук: TP6 (SUM_L), TP7 (SUM_R) — осциллографом; уровень — RV1, RV2. TP3, TP8 — GND.",
]


def read():
    comps, pin_net = ms.read_netlist()
    # P4: гребёнка 1:1 с J3 (шлейф), USB-C — +5V и GND через кабель от J2.
    comps.append({"ref": "P4", "value": "ESP32-P4-WIFI6-Touch-LCD-5", "footprint": "",
                  "lib": "hybrid-radio", "part": "ESP32-P4-Board", "block": "p4"})
    for n in range(1, 41):
        net = pin_net.get(("J3", str(n)))
        if net:
            pin_net[("P4", str(n))] = net
    pin_net[("P4", "41")] = "+5V"
    pin_net[("P4", "42")] = "GND"
    # Флаги питания: источник этих цепей KiCad не видит — +12V приходит
    # с разъёма J1 (через Q1), +3V3_ESP — с гребёнки P4, +3V3_FM — с HT7333
    # через бусину FB2 (пассивный элемент). ERC 2026-10-09: power_pin_not_driven.
    for i, (net, block) in enumerate((("+12V", "power_input1"), ("+3V3_ESP", "ribbon1"),
                                      ("+3V3_FM", "supply_3v3_tuner1")), start=3):
        ref = f"#FLG{i}"
        comps.append({"ref": ref, "value": "PWR_FLAG", "footprint": "", "lib": "power",
                      "part": "PWR_FLAG", "block": block})
        pin_net[(ref, "1")] = net
    return comps, pin_net


# ------------------------------------------------------------ раскладка
PAD = 7.62            # коридор под провода вокруг каждой детали, мм
BLOCK_GAP = 12.7
ROW_GAP = 12.7
POWER_STRIP = 25.4    # полоса слева в зоне под символы питания


def outward(ang):
    """Направление от корпуса наружу на листе (Y вниз) по углу вывода в библиотеке."""
    return {0: (-1, 0), 180: (1, 0), 90: (0, 1), 270: (0, -1)}[ang]


class Part:
    def __init__(self, c, key, sym, pin_net):
        self.c, self.key = c, key
        self.pins = ms.symbol_pins(sym)
        self.bbox = ms.body_bbox(sym, self.pins)
        self.fields, room = ms.field_layout(c["ref"], c["value"], self.pins, self.bbox)
        xs = [p[1] for p in self.pins] + [self.bbox[0], self.bbox[1]]
        ys = [p[2] for p in self.pins] + [self.bbox[2], self.bbox[3]]
        self.minx, self.maxx, self.miny, self.maxy = min(xs), max(xs), min(ys), max(ys)
        fl, fr, fu, fd = room
        self.left, self.right, self.up, self.down = fl + PAD, fr + PAD, fu + PAD, fd + PAD
        self.w = (self.maxx - self.minx) + self.left + self.right
        self.h = (self.maxy - self.miny) + self.up + self.down
        self.sx = self.sy = 0.0

    def place_at(self, x, y):
        """(x, y) — левый верхний угол клетки на листе."""
        self.sx = snap(x + self.left - self.minx)
        self.sy = snap(y + self.up + self.maxy)

    def pin_xy(self, p):
        return snap(self.sx + p[1]), snap(self.sy - p[2])

    def body_rect(self):
        return (self.sx + self.minx, self.sy - self.maxy, self.sx + self.maxx, self.sy - self.miny)

    def field_rects(self):
        out = []
        for t, dx, dy, j in self.fields:
            w = len(t) * CHAR_W
            x0 = self.sx + dx - (0 if j == "left" else w if j == "right" else w / 2)
            out.append((x0, self.sy + dy - 1.0, x0 + w, self.sy + dy + 1.0))
        return out


def power_part(net, lib_symbols, root, zone_i, k):
    base = POWER[net]
    key = f"power:{base}"
    if key not in lib_symbols:
        lib_symbols[key] = ms.resolve_symbol("power", base)
    c = {"ref": f"#PWR{zone_i}{k:02d}", "value": net, "footprint": "", "lib": "power",
         "part": base, "block": None}
    return Part(c, key, lib_symbols[key], {})


def layout_zone(zone_i, ztitle, blocks, by_block, lib_symbols, x0, y0, width, items, pin_net, power_nets):
    """Детали зоны по блокам; возвращает (части, нижний край зоны, прямоугольники текстов)."""
    parts, text_rects = [], []
    items.append(text(ztitle, x0 + ZONE_PAD, y0 + ZONE_PAD + 4, size=5, bold=True))
    text_rects.append((x0 + ZONE_PAD, y0 + ZONE_PAD - 1, x0 + ZONE_PAD + len(ztitle) * 4.6, y0 + ZONE_PAD + 5))
    # символы питания — столбиком в полосе слева
    py = y0 + ZONE_PAD + 22
    pwr = []
    for k, net in enumerate(power_nets):
        p = power_part(net, lib_symbols, None, zone_i, k)
        p.place_at(x0 + ZONE_PAD, py)
        py += p.h
        pwr.append(p)
    left = x0 + ZONE_PAD + POWER_STRIP
    bx, by, row_h = left, y0 + ZONE_PAD + 14, 0.0
    right_edge = x0 + width - ZONE_PAD
    for block in blocks:
        comps = sorted(by_block.get(block, []), key=lambda c: ms.ref_key(c["ref"]))
        if not comps:
            continue
        cells = []
        for c in comps:
            key = f'{c["lib"]}:{c["part"]}'
            if key not in lib_symbols:
                lib_symbols[key] = ms.resolve_symbol(c["lib"], c["part"])
            cells.append(Part(c, key, lib_symbols[key], pin_net))
        title = BLOCK_TITLES.get(block, block)
        notes = NOTES.get(block, [])
        title_w = max([len(title) * 2.3] + [len(n) * CHAR_W for n in notes])
        block_w = max(min(sum(p.w for p in cells), 200), max(p.w for p in cells))
        if bx + max(block_w, title_w) > right_edge and bx > left:
            bx, by, row_h = left, by + row_h + ROW_GAP, 0.0
        items.append(text(title, bx, by, size=2.54, bold=True))
        for i, n in enumerate(notes):
            items.append(text(n, bx, by + (i + 1) * ms.NOTE_LINE, italic=True, color="200 0 0 1"))
        text_rects.append((bx, by - 3.0, bx + title_w, by + len(notes) * ms.NOTE_LINE + 0.8))
        cx, cy, line_h, used_w = bx, by + 2 + len(notes) * ms.NOTE_LINE, 0.0, 0.0
        for p in cells:
            if cx + p.w > bx + block_w + 0.1 and cx > bx:
                cx, cy, line_h = bx, cy + line_h, 0.0
            p.place_at(cx, cy)
            parts.append(p)
            cx += p.w
            used_w = max(used_w, cx - bx)
            line_h = max(line_h, p.h)
        row_h = max(row_h, cy + line_h - by)
        bx += max(used_w, title_w) + BLOCK_GAP
    bottom = max(by + row_h, py) + ZONE_PAD
    return parts, pwr, bottom, text_rects


def build(paper_w, paper_h, first=frozenset()):
    comps, pin_net = read()
    lib_symbols, items = {}, []
    root = new_uuid()
    by_block = OrderedDict()
    for c in comps:
        by_block.setdefault(c["block"], []).append(c)
    zoned = {b for _t, bl in ZONES for b in bl}
    loose = [b for b in by_block if b not in zoned]
    if loose:
        sys.exit(f"блоки без зоны: {loose}")
    width = paper_w - 2 * MARGIN

    # --- раскладка зон
    zones, y = [], MARGIN
    for zi, (ztitle, blocks) in enumerate(ZONES, start=1):
        zone_comps = [c for b in blocks for c in by_block.get(b, [])]
        refs = {c["ref"] for c in zone_comps}
        nets_here = {n for (r, _p), n in pin_net.items() if r in refs and n and n != "__NOCONNECT"}
        power_nets = [n for n in POWER if n in nets_here]
        parts, pwr, bottom, trects = layout_zone(zi, ztitle, blocks, by_block, lib_symbols,
                                                 MARGIN, y, width, items, pin_net, power_nets)
        zones.append((zi, (MARGIN, y, MARGIN + width, bottom), parts, pwr, trects))
        y = bottom + 10
    notes_y = y
    total_h = notes_y + 6 + len(GENERAL_NOTES) * 3.81 + MARGIN
    if total_h > paper_h:
        return None, total_h

    # --- трассировка
    R = sch_router.Router(paper_w, paper_h)
    zone_of = {}
    for zi, rect, parts, pwr, trects in zones:
        for p in parts + pwr:
            zone_of[p.c["ref"]] = zi
            R.block_rect(*p.body_rect())
            for fr in p.field_rects():
                R.block_rect(*fr, soft=True)
        for tr in trects:
            R.block_rect(*tr)
    terms = {}          # (зона, цепь) -> [(клетка, наружу)]
    nc_cells = []
    for zi, rect, parts, pwr, trects in zones:
        for p in parts + pwr:
            for pin in p.pins:
                xy = sch_router.cell(*p.pin_xy(pin))
                net = p.c["value"] if p in pwr else pin_net.get((p.c["ref"], pin[0]))
                if net is None or net == "__NOCONNECT":
                    nc_cells.append((p, pin))
                    R.block_cell(xy)
                    continue
                terms.setdefault((zi, net), []).append((xy, outward(pin[3])))
    # Выход от конца вывода наружу сквозь собственный габарит детали: у
    # светодиода стрелки излучения торчат дальше конца катода (габарит x до
    # −4,57 при выводе на −3,81), и вывод оказывался замурован (D3, D4 не
    # развелись, прогон 2026-10-10).
    for zi, rect, parts, pwr, trects in zones:
        for p in parts + pwr:
            bx0, by0, bx1, by1 = p.body_rect()
            (cx0, cy0), (cx1, cy1) = sch_router.cell(bx0, by0), sch_router.cell(bx1, by1)
            for pin in p.pins:
                x, y = sch_router.cell(*p.pin_xy(pin))
                dx, dy = outward(pin[3])
                x, y = x + dx, y + dy
                while cx0 <= x <= cx1 and cy0 <= y <= cy1:
                    R.hard[R.idx(x, y)] = 0
                    x, y = x + dx, y + dy
    net_id = {}
    for (zi, net), tl in terms.items():
        net_id.setdefault(net, len(net_id) + 1)
        for xy, _o in tl:
            R.set_terminal(xy, net_id[net])
    # Клетка перед концом вывода — за его цепью: иначе её занимает изгиб
    # чужого провода, разведённого раньше, и вывод замурован (J2-1, D3-2,
    # прогон 2026-10-10).
    for (zi, net), tl in terms.items():
        for (x, y), (dx, dy) in tl:
            i = R.idx(x + dx, y + dy)
            if not R.hard[i] and R.node[i] == 0:
                R.node[i] = net_id[net]

    who = {}            # клетка конца вывода -> "обозначение-вывод" (для отчёта)
    for zi, rect, parts, pwr, trects in zones:
        for p in parts + pwr:
            for pin in p.pins:
                who[sch_router.cell(*p.pin_xy(pin))] = f'{p.c["ref"]}-{pin[0]}'
    # Земля — первой: у неё больше всего выводов, ей нужнее свободные коридоры;
    # остальные цепи её только пересекают (прогон 2026-10-09: при GND
    # последней не развелось 5 выводов земли).
    # first — цепи, не разведённые в прошлом прогоне: их — раньше всех.
    order = sorted(terms, key=lambda k: (k[1] not in first, k[1] != "GND", k[1] not in POWER,
                                         len(terms[k])))
    wires, junctions, labels, failed, pwr_extra, routed = [], [], [], [], [], []
    for zi, net in order:
        tl = terms[(zi, net)]
        x0, y0, x1, y1 = next(r for z, r, *_ in zones if z == zi)
        area = (*sch_router.cell(x0 + 1.27, y0 + 1.27), *sch_router.cell(x1 - 1.27, y1 - 1.27))
        exits = {xy: o for xy, o in tl}
        edges, nodes, bad = R.route_net(net_id[net], [xy for xy, _o in tl], exits, area)
        segs, adj = sch_router.segments(edges, nodes)
        g = sch_router.G
        for a, b in segs:
            wires.append((a[0] * g, a[1] * g, b[0] * g, b[1] * g))
        tset = {xy for xy, _o in tl}
        for c, ns in adj.items():
            if len(ns) + (1 if c in tset else 0) >= 3:
                junctions.append((c[0] * g, c[1] * g))
        for xy in bad:
            failed.append((net, who.get(xy, "?")))
            # не разведённый вывод: силовой — свой символ питания (местная
            # метка «GND» — другая цепь, чем глобальная GND), сигнальный — метка
            if net in POWER:
                pwr_extra.append((net, xy[0] * g, xy[1] * g, exits[xy]))
            else:
                labels.append((net, xy[0] * g, xy[1] * g, exits[xy]))
        routed.append((net, segs, tl))
    # Метки с именами — после всей трассировки: точка метки не должна лежать
    # на пересечении с чужим проводом (метка соединяет все провода в своей
    # точке — так слились I2C_SCL и SPARE_GPIO4, прогон 2026-10-09).
    for net, segs, tl in routed:
        if net not in POWER:
            labels.append(label_spot(net, segs, tl, R))

    # --- выдача
    for zi, rect, parts, pwr, trects in zones:
        x0, y0, x1, y1 = rect
        items.append(f'\t(rectangle (start {snap(x0)} {snap(y0)}) (end {snap(x1)} {snap(y1)})\n'
                     f'\t\t(stroke (width 0.5) (type dash)) (fill (type none))\n\t\t(uuid "{new_uuid()}"))')
        for p in parts:
            items.append(symbol_text(p, root))
        for p in pwr:
            items.append(power_text(p, root))
    for k, (net, x, y, o) in enumerate(pwr_extra):
        items.append(extra_power_text(net, x, y, o, root, k, lib_symbols))
    for p, pin in nc_cells:
        x, y = p.pin_xy(pin)
        items.append(f'\t(no_connect (at {x} {y}) (uuid "{new_uuid()}"))')
    for x0, y0, x1, y1 in wires:
        items.append(f'\t(wire (pts (xy {snap(x0)} {snap(y0)}) (xy {snap(x1)} {snap(y1)}))\n'
                     f'\t\t(stroke (width 0) (type default))\n\t\t(uuid "{new_uuid()}"))')
    for x, y in junctions:
        items.append(f'\t(junction (at {snap(x)} {snap(y)}) (diameter 0) (color 0 0 0 0)\n'
                     f'\t\t(uuid "{new_uuid()}"))')
    for net, x, y, o in labels:
        ang = {(-1, 0): 180, (1, 0): 0, (0, 1): 270, (0, -1): 90}[o]
        just = "right" if ang in (180, 270) else "left"
        items.append(f'\t(label {quote(net)} (at {snap(x)} {snap(y)} {ang})\n'
                     f'\t\t(effects (font (size 1.27 1.27)) (justify {just} bottom))\n'
                     f'\t\t(uuid "{new_uuid()}"))')
    for i, n in enumerate(GENERAL_NOTES):
        bold = n.startswith("#")             # «#» в начале строки — заголовок
        items.append(text(n.lstrip("#"), MARGIN + ZONE_PAD, notes_y + 6 + i * 3.81,
                          size=2.0 if bold else 1.8, bold=bold))
    stats = {"wires": len(wires), "junctions": len(junctions), "failed": failed, "router": R, "who": who,
             "parts": [p.c["ref"] for _z, _r, parts, _p, _t in zones for p in parts]}
    return (root, lib_symbols, items, stats), total_h


def label_spot(net, segs, tl, R):
    """Метка с именем цепи — внутрь самого длинного горизонтального отрезка
    (текст над проводом), иначе вертикального; только в клетку, которую не
    пересекает чужой провод и где нет узла. Если такой нет — на конец вывода:
    его чужие цепи не занимают никогда."""
    g = sch_router.G
    hz = sorted(((abs(b[0] - a[0]), a, b) for a, b in segs if a[1] == b[1]), reverse=True)
    vt = sorted(((abs(b[1] - a[1]), a, b) for a, b in segs if a[0] == b[0]), reverse=True)
    for _l, a, b in hz:
        y = a[1]
        for x in range(min(a[0], b[0]) + 1, max(a[0], b[0])):
            i = R.idx(x, y)
            if R.v_own[i] == 0 and R.node[i] == 0:
                return (net, x * g, y * g, (1, 0))
    for _l, a, b in vt:
        x = a[0]
        for y in range(max(a[1], b[1]) - 1, min(a[1], b[1]), -1):
            i = R.idx(x, y)
            if R.h_own[i] == 0 and R.node[i] == 0:
                return (net, x * g, y * g, (0, -1))
    xy, o = tl[0]
    return (net, xy[0] * g, xy[1] * g, o)


def extra_power_text(net, x, y, direction, root, k, lib_symbols):
    """Символ питания прямо на конце не разведённого вывода, «смотрит» наружу."""
    base = POWER[net]
    key = f"power:{base}"
    if key not in lib_symbols:
        lib_symbols[key] = ms.resolve_symbol("power", base)
    d = {(-1, 0): 180, (1, 0): 0, (0, 1): 270, (0, -1): 90}[direction]
    rot = (d - (270 if base == "GND" else 90)) % 360
    ref = f"#PWR9{k:02d}"
    return (f'\t(symbol (lib_id "{key}") (at {snap(x)} {snap(y)} {rot}) (unit 1)\n'
            f'\t\t(exclude_from_sim no) (in_bom yes) (on_board yes) (dnp no)\n'
            f'\t\t(uuid "{new_uuid()}")\n'
            f'\t\t(property "Reference" "{ref}" (at {snap(x)} {snap(y)} 0) (effects (font (size 1.27 1.27)) (hide yes)))\n'
            f'\t\t(property "Value" {quote(net)} (at {snap(x)} {snap(y)} {90 if rot % 180 else 0}) '
            f'(effects (font (size 1.27 1.27))))\n'
            f'\t\t(property "Footprint" "" (at {snap(x)} {snap(y)} 0) (effects (font (size 1.27 1.27)) (hide yes)))\n'
            f'\t\t(property "Datasheet" "" (at {snap(x)} {snap(y)} 0) (effects (font (size 1.27 1.27)) (hide yes)))\n'
            f'\t\t(pin "1" (uuid "{new_uuid()}"))\n'
            f'\t\t(instances (project "{NAME}" (path "/{root}" (reference "{ref}") (unit 1))))\n\t)')


def symbol_text(p, root):
    c = p.c
    in_bom = "no" if c["footprint"].startswith(("TestPoint:", "Connector_Wire:")) or c["ref"] == "P4" else "yes"
    on_board = "no" if c["ref"] == "P4" else "yes"
    dnp = "yes" if c["ref"] in DNP else "no"
    sx, sy = p.sx, p.sy
    out = [f'\t(symbol (lib_id "{p.key}") (at {sx} {sy} 0) (unit 1)\n'
           f'\t\t(exclude_from_sim no) (in_bom {in_bom}) (on_board {on_board}) (dnp {dnp})\n'
           f'\t\t(uuid "{new_uuid()}")']
    pos = {t: (dx, dy, j) for t, dx, dy, j in p.fields}

    def prop(name, value, hide):
        dx, dy, j = (0, 0, "center") if hide else pos.get(value, (0, 0, "center"))
        just = "" if j == "center" else f" (justify {j})"
        out.append(f'\t\t(property "{name}" {quote(value)} (at {snap(sx + dx)} {snap(sy + dy)} 0)\n'
                   f'\t\t\t(effects (font (size 1.27 1.27)){just}{" (hide yes)" if hide else ""}))')

    prop("Reference", c["ref"], c["ref"].startswith("#"))
    prop("Value", c["value"], False)
    prop("Footprint", c["footprint"], True)
    prop("Datasheet", "", True)
    for pin in p.pins:
        out.append(f'\t\t(pin "{pin[0]}" (uuid "{new_uuid()}"))')
    out.append(f'\t\t(instances (project "{NAME}" (path "/{root}" '
               f'(reference "{c["ref"]}") (unit 1))))\n\t)')
    return "\n".join(out)


def power_text(p, root):
    x, y = p.sx, p.sy
    ref, net = p.c["ref"], p.c["value"]
    vy = 3.81 if POWER[net] == "GND" else -3.81
    return (f'\t(symbol (lib_id "{p.key}") (at {x} {y} 0) (unit 1)\n'
            f'\t\t(exclude_from_sim no) (in_bom yes) (on_board yes) (dnp no)\n'
            f'\t\t(uuid "{new_uuid()}")\n'
            f'\t\t(property "Reference" "{ref}" (at {x} {y} 0) (effects (font (size 1.27 1.27)) (hide yes)))\n'
            f'\t\t(property "Value" {quote(net)} (at {x} {snap(y + vy)} 0) (effects (font (size 1.27 1.27))))\n'
            f'\t\t(property "Footprint" "" (at {x} {y} 0) (effects (font (size 1.27 1.27)) (hide yes)))\n'
            f'\t\t(property "Datasheet" "" (at {x} {y} 0) (effects (font (size 1.27 1.27)) (hide yes)))\n'
            f'\t\t(pin "1" (uuid "{new_uuid()}"))\n'
            f'\t\t(instances (project "{NAME}" (path "/{root}" (reference "{ref}") (unit 1))))\n\t)')


def text(t, x, y, size=1.27, bold=False, italic=False, color=None):
    eff = f"(font (size {size} {size})" + (" (bold yes)" if bold else "") + (" (italic yes)" if italic else "") \
        + (f" (color {color})" if color else "") + ")"
    return (f'\t(text {quote(t)} (exclude_from_sim no) (at {snap(x)} {snap(y)} 0)\n'
            f'\t\t(effects {eff} (justify left bottom))\n\t\t(uuid "{new_uuid()}"))')


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    OUT_DIR.mkdir(exist_ok=True)
    for paper, w, h in (("A1", 841.0, 594.0), ("A0", 1189.0, 841.0)):
        res, height = build(w, h)
        if res:
            break
    else:
        sys.exit(f"не влезает даже в A0: высота {height:.0f} мм")
    # Повторные прогоны: не разведённые цепи — первыми; лучший результат.
    first = set()
    for attempt in range(5):
        if not res[3]["failed"]:
            break
        first |= {net for net, _w in res[3]["failed"]}
        again, _h = build(w, h, frozenset(first))
        print(f"прогон {attempt + 2}: первыми {sorted(first)} -> не разведено {len(again[3]['failed'])}")
        if len(again[3]["failed"]) < len(res[3]["failed"]):
            res = again
    root, libs, items, st = res
    lib_text = "\n".join("\t\t" + ms.dump(s) for s in libs.values())
    sch = (f'(kicad_sch (version 20260306) (generator "make_carriers_sheet.py") (generator_version "10.0")\n'
           f'\t(uuid "{root}")\n\t(paper "{paper}")\n'
           f'\t(title_block (title "IP-Radio: доп. платы — лист для макетирования и проверок")\n'
           f'\t\t(comment 1 "Сгенерировано hardware/skidl/make_carriers_sheet.py из hardware/skidl/carrier.py")\n'
           f'\t\t(comment 2 "Перечень деталей: docs/35-carrier-boards-parts.md"))\n'
           f'\t(lib_symbols\n{lib_text}\n\t)\n' + "\n".join(items) +
           '\n\t(sheet_instances (path "/" (page "1")))\n)\n')
    SCH.write_text(sch, encoding="utf-8")

    pro = json.loads((ms.KICAD_DIR / "Hybryd Radio.kicad_pro").read_text(encoding="utf-8"))
    pro.setdefault("meta", {})["filename"] = f"{NAME}.kicad_pro"
    (OUT_DIR / f"{NAME}.kicad_pro").write_text(json.dumps(pro, indent=2, ensure_ascii=False), encoding="utf-8")
    (OUT_DIR / "fp-lib-table").write_text(
        '(fp_lib_table\n\t(version 7)\n\t(lib (name "hybrid-radio") (type "KiCad") '
        '(uri "${KIPRJMOD}/../hybrid-radio.pretty") (options "") (descr ""))\n)\n', encoding="utf-8")
    (OUT_DIR / "sym-lib-table").write_text(
        '(sym_lib_table\n\t(version 7)\n\t(lib (name "hybrid-radio") (type "KiCad") '
        '(uri "${KIPRJMOD}/../hybrid-radio.kicad_sym") (options "") (descr ""))\n)\n', encoding="utf-8")
    real = [r for r in st["parts"] if not r.startswith("#") and r != "P4"]
    print(f"{SCH}: лист {paper}, деталей {len(real)} (+ P4, флаги), проводов {st['wires']}, "
          f"узлов {st['junctions']}, не разведено выводов {len(st['failed'])}")
    for net, where in st["failed"]:
        print(f"  не разведено: {net} у вывода {where} — соединено {'символом питания' if net in POWER else 'меткой'}")


if __name__ == "__main__":
    main()
