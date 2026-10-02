"""Схема KiCad из нетлиста: каждая деталь в своей клетке сетки, выводы
подписаны метками цепей.

Почему не generate_schematic() из SKiDL: в версии 2.3.0 он кладёт детали
друг на друга (дроссель и конденсатор в одну точку), совпавшие выводы
KiCad считает соединёнными, и в схеме замыкаются цепи — GND с выходами
тюнера и ЦАП, вход LDO с выходом. Поймано compare_netlists.py 2026-10-01.

Здесь раскладка детерминированная: клетки не пересекаются, соединение
делается только метками на концах выводов, проводов нет вовсе. Значит
случайно закоротить нечем. Результат всё равно сверяется через KiCad:

    .venv/Scripts/python hardware/skidl/make_schematic.py
    kicad-cli sch export netlist --format kicadxml -o sch.xml "hardware/kicad/Hybryd Radio.kicad_sch"
    .venv/Scripts/python hardware/skidl/compare_netlists.py sch.xml
"""

import os
import re
import sys
import uuid
from collections import OrderedDict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from check_carrier import NETLIST, field, parse_sexp  # noqa: E402

KICAD_DIR = NETLIST.parent
SCH = KICAD_DIR / "Hybryd Radio.kicad_sch"
STD_SYMBOLS = Path(os.environ["LOCALAPPDATA"]) / "Programs/KiCad/10.0/share/kicad/symbols"
LIB_FILES = {"hybrid-radio": KICAD_DIR / "hybrid-radio.kicad_sym"}

# Заголовки блоков — по имени подсхемы SKiDL в sheetpath нетлиста.
BLOCK_TITLES = OrderedDict([
    ("power_input1", "Вход 12 В, защита, признак питания"),
    ("supply_5v1", "5 В: DC-DC и USB-A к плате P4"),
    ("supply_3v3_tuner1", "3,3 В тюнера"),
    ("tuner1", "FM-тюнер RRD-102"),
    ("dac1", "ЦАП GY-PCM5102A"),
    ("mixer_channel1", "Микшер, левый канал"),
    ("mixer_channel2", "Микшер, правый канал"),
    ("amp_channel1", "Усилитель, левый канал"),
    ("amp_channel2", "Усилитель, правый канал"),
    ("amp_mute1", "Приглушение усилителей"),
    ("expander1", "Расширитель MCP23017"),
    ("controls1", "Энкодеры, кнопки, резерв"),
    ("ribbon1", "Шлейф к гребёнке J3 платы P4"),
    ("indication1", "Индикация и контрольные точки"),
])

# Монтажные требования, которые иначе теряются между документами
# (разбор №4, 2026-10-02). Печатаются под заголовком блока.
NOTES = {
    "power_input1": ["БП только стабилизированный 12 В / 5 А; F1 — T5A 5x20"],
    "dac1": ["Перемычки H1L-H4L и SCK на обороте модуля РАЗОМКНУТЬ:",
             "FLT/DEMP/FMT/XSMT задаются с этой платы"],
    "amp_channel1": ["GAIN 26 дБ: перепаять делитель модуля (20k на GND, 100k на GVDD), docs/04"],
    "amp_channel2": ["GAIN 26 дБ: перепаять делитель модуля (20k на GND, 100k на GVDD), docs/04"],
    "amp_mute1": ["R14 подобрать по замеру MUTE модуля HW-327 (docs/33, §8)"],
    "expander1": ["GPA7 и GPB7 — только выходы (Microchip DS20001952E): не подключать"],
}
NOTE_LINE = 2.54

GRID = 1.27
CHAR_W = 1.05        # ширина символа метки, мм (шрифт 1,27)
PAPER = ("A2", 594.0, 420.0)
MARGIN = 20.0


def snap(v):
    return round(round(v / GRID) * GRID, 2)


def new_uuid():
    return str(uuid.uuid4())


# ------------------------------------------------------------ S-выражения
class Q(str):
    """Строка, которая в исходнике была в кавычках."""


def parse_keep_quotes(text):
    tokens = re.findall(r'\(|\)|"(?:[^"\\]|\\.)*"|[^\s()]+', text)
    stack, cur = [], []
    for t in tokens:
        if t == "(":
            stack.append(cur)
            cur = []
        elif t == ")":
            done, cur = cur, stack.pop()
            cur.append(done)
        elif t.startswith('"'):
            cur.append(Q(t[1:-1].replace('\\"', '"').replace("\\\\", "\\")))
        else:
            cur.append(t)
    return cur[0]


def dump(node):
    if isinstance(node, list):
        return "(" + " ".join(dump(x) for x in node) + ")"
    return quote(node) if isinstance(node, Q) else node


def quote(tok):
    return '"' + str(tok).replace("\\", "\\\\").replace('"', '\\"') + '"'


# ------------------------------------------------------------ библиотеки
_lib_cache = {}


def load_lib(lib):
    if lib not in _lib_cache:
        path = LIB_FILES.get(lib, STD_SYMBOLS / f"{lib}.kicad_sym")
        root = parse_keep_quotes(path.read_text(encoding="utf-8"))
        _lib_cache[lib] = {item[1]: item for item in root if isinstance(item, list) and item[0] == "symbol"}
    return _lib_cache[lib]


def resolve_symbol(lib, name):
    """Символ с раскрытым extends и именем вида "Lib:Name" для lib_symbols схемы."""
    symbols = load_lib(lib)
    sym = symbols[name]
    parent = field(sym, "extends")
    if parent:
        base = resolve_symbol(lib, parent)
        own_props = {p[1]: p for p in sym if isinstance(p, list) and p[0] == "property"}
        body = []
        for item in base[2:]:
            if isinstance(item, list) and item[0] == "property" and item[1] in own_props:
                body.append(own_props.pop(item[1]))
            elif isinstance(item, list) and item[0] == "symbol":
                body.append(["symbol", Q(item[1].replace(f"{parent}_", f"{name}_", 1))] + item[2:])
            else:
                body.append(item)
        body += list(own_props.values())
        return ["symbol", Q(f"{lib}:{name}")] + body
    out = ["symbol", Q(f"{lib}:{name}")]
    for item in sym[2:]:
        out.append(item)
    return out


def symbol_pins(sym):
    """[(номер, x, y, угол)] — выводы единицы 1, основного начертания."""
    pins = []
    for unit in sym:
        if not (isinstance(unit, list) and unit[0] == "symbol"):
            continue
        m = re.search(r"_(\d+)_(\d+)$", unit[1])
        if not m or m.group(1) not in ("0", "1") or m.group(2) not in ("0", "1"):
            continue
        for item in unit:
            if isinstance(item, list) and item[0] == "pin":
                at = next(x for x in item if isinstance(x, list) and x[0] == "at")
                num = next(x for x in item if isinstance(x, list) and x[0] == "number")[1]
                pins.append((num, float(at[1]), float(at[2]), int(float(at[3]))))
    return pins


def body_bbox(sym, pins):
    """Габарит рисунка символа (без выводов) в координатах библиотеки, Y вверх."""
    xs, ys = [], []
    for unit in sym:
        if not (isinstance(unit, list) and unit[0] == "symbol"):
            continue
        m = re.search(r"_(\d+)_(\d+)$", unit[1])
        if not m or m.group(1) not in ("0", "1") or m.group(2) not in ("0", "1"):
            continue
        for g in unit:
            if not (isinstance(g, list) and g[0] in ("rectangle", "polyline", "circle", "arc", "bezier")):
                continue
            stack = [g]
            while stack:
                node = stack.pop()
                for x in node:
                    if isinstance(x, list):
                        if x[0] in ("start", "end", "xy", "mid", "center") and len(x) >= 3:
                            xs.append(float(x[1]))
                            ys.append(float(x[2]))
                        stack.append(x)
            if g[0] == "circle":
                c = next(x for x in g if isinstance(x, list) and x[0] == "center")
                r = float(next(x for x in g if isinstance(x, list) and x[0] == "radius")[1])
                xs += [float(c[1]) - r, float(c[1]) + r]
                ys += [float(c[2]) - r, float(c[2]) + r]
    if not xs:
        xs = [p[1] for p in pins] or [0]
        ys = [p[2] for p in pins] or [0]
    return min(xs), max(xs), min(ys), max(ys)


def text_w(t):
    return len(t) * CHAR_W


def field_layout(ref, value, pins, bbox):
    """Где ставить обозначение и номинал, чтобы не налезть на метки выводов.

    Возвращает [(текст, dx, dy, justify)] — смещения от точки символа на
    листе (Y вниз), и сколько места добавить клетке: (слева, справа,
    сверху, снизу)."""
    bx0, bx1, by0, by1 = bbox
    sides = {p[3] for p in pins}
    top, bottom, cy = -by1, -by0, -(by0 + by1) / 2
    texts = ([] if ref.startswith("#") else [ref]) + [value]
    tw = max(text_w(t) for t in texts)
    small = (by1 - by0) <= 12.7

    # Мелкая деталь: текст сбоку, со стороны, где нет выводов.
    if small and 180 not in sides:
        x, just, room = bx1 + 1.27, "left", (0, tw + 2.5, 0, 0)
    elif small and 0 not in sides:
        x, just, room = bx0 - 1.27, "right", (tw + 2.5, 0, 0, 0)
    else:
        x = None
    if x is not None:
        out = [(t, x, cy + (i - (len(texts) - 1) / 2) * 2.54, just) for i, t in enumerate(texts)]
        return out, room

    # Корпус с выводами по бокам: текст над и под ним, где нет выводов.
    cx = (bx0 + bx1) / 2
    above = [t for t in texts if t != value or 90 in sides]
    below = [t for t in texts if t == value and 90 not in sides]
    if 270 in sides:
        above, below = [], above + below
    out = [(t, cx, top - 1.0 - i * 2.54, "center") for i, t in enumerate(reversed(above))]
    out += [(t, cx, bottom + 2.54 + i * 2.54, "center") for i, t in enumerate(below)]
    extra_w = max(0, tw - (bx1 - bx0)) / 2
    return out, (extra_w, extra_w, len(above) * 2.54 + 1, len(below) * 2.54 + 1)


# ------------------------------------------------------------ нетлист
def read_netlist():
    root = parse_sexp(NETLIST.read_text(encoding="utf-8"))
    comps, pin_net = [], {}
    for section in root:
        if not isinstance(section, list):
            continue
        if section[0] == "components":
            for c in section[1:]:
                src = next(x for x in c if isinstance(x, list) and x[0] == "libsource")
                sheet = next(x for x in c if isinstance(x, list) and x[0] == "sheetpath")
                comps.append({
                    "ref": field(c, "ref"), "value": field(c, "value"),
                    "footprint": field(c, "footprint") or "",
                    "lib": field(src, "lib"), "part": field(src, "part"),
                    "block": field(sheet, "names").strip("/").split("/")[-1],
                })
        elif section[0] == "nets":
            for n in section[1:]:
                name = field(n, "name")
                for node in n:
                    if isinstance(node, list) and node[0] == "node":
                        pin_net[(field(node, "ref"), field(node, "pin"))] = name
    return comps, pin_net


# ------------------------------------------------------------ раскладка
def ref_key(ref):
    m = re.match(r"([A-Z]+)(\d+)", ref)
    return (m.group(1), int(m.group(2))) if m else (ref, 0)


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    comps, pin_net = read_netlist()
    lib_symbols, items = {}, []
    root_uuid = new_uuid()

    blocks = OrderedDict((b, []) for b in BLOCK_TITLES)
    for c in comps:
        blocks.setdefault(c["block"], []).append(c)

    x0, y0, row_h = MARGIN, MARGIN, 0.0
    for block, parts in blocks.items():
        if not parts:
            continue
        parts.sort(key=lambda c: ref_key(c["ref"]))
        cells = []
        for c in parts:
            key = f'{c["lib"]}:{c["part"]}'
            if key not in lib_symbols:
                lib_symbols[key] = resolve_symbol(c["lib"], c["part"])
            pins = symbol_pins(lib_symbols[key])
            xs = [p[1] for p in pins] or [0]
            ys = [p[2] for p in pins] or [0]

            def lab(p):
                return len(pin_net.get((c["ref"], p[0]), "") or "") * CHAR_W + 2

            # Метка уходит от корпуса: вывод с углом 0 стоит слева, его метка
            # тянется влево; 180 — вправо; 90 — внизу, метка вниз; 270 — вверх.
            left = max([lab(p) for p in pins if p[3] == 0] or [0])
            right = max([lab(p) for p in pins if p[3] == 180] or [0])
            down = max([lab(p) for p in pins if p[3] == 90] or [0])
            up = max([lab(p) for p in pins if p[3] == 270] or [0])
            bbox = body_bbox(lib_symbols[key], pins)
            fields, (fl, fr, fu, fd) = field_layout(c["ref"], c["value"], pins, bbox)
            left, right, up, down = max(left, fl), max(right, fr), max(up, fu), max(down, fd)
            minx, maxx = min(min(xs), bbox[0]), max(max(xs), bbox[1])
            miny, maxy = min(min(ys), bbox[2]), max(max(ys), bbox[3])
            w = (maxx - minx) + left + right + 5
            h = (maxy - miny) + up + down + 5
            cells.append((c, key, pins, w, h, minx, maxy + up, left, fields))

        title = BLOCK_TITLES.get(block, block)
        notes = NOTES.get(block, [])
        title_w = max([len(title) * 2.3] + [len(n) * CHAR_W for n in notes])   # жирный 2,54 мм
        block_w = min(sum(cell[3] for cell in cells), 180)
        if x0 + max(block_w, title_w) > PAPER[1] - MARGIN:
            x0, y0, row_h = MARGIN, y0 + row_h + 10, 0.0

        items.append(f'\t(text "{title}" (exclude_from_sim no) (at {snap(x0)} {snap(y0)} 0)\n'
                     f'\t\t(effects (font (size 2.54 2.54) (bold yes)) (justify left bottom))\n'
                     f'\t\t(uuid "{new_uuid()}"))')
        for i, note in enumerate(notes):
            items.append(f'\t(text {quote(note)} (exclude_from_sim no) '
                         f'(at {snap(x0)} {snap(y0 + (i + 1) * NOTE_LINE)} 0)\n'
                         f'\t\t(effects (font (size 1.27 1.27) (italic yes) (color 200 0 0 1)) '
                         f'(justify left bottom))\n\t\t(uuid "{new_uuid()}"))')

        cx, cy, line_h = x0, y0 + 4 + len(notes) * NOTE_LINE, 0.0
        used_w = 0.0
        for c, key, pins, w, h, minx, maxy, label_len, fields in cells:
            if cx + w > x0 + block_w + 0.1 and cx > x0:
                cx, cy, line_h = x0, cy + line_h, 0.0
            sx = snap(cx + label_len + 2.5 - minx)
            sy = snap(cy + 2.5 + maxy)
            items.append(place_symbol(c, key, pins, sx, sy, pin_net, root_uuid, fields))
            cx += w
            used_w = max(used_w, cx - x0)
            line_h = max(line_h, h)
        block_h = cy + line_h - y0
        x0 += max(used_w, title_w) + 10
        row_h = max(row_h, block_h)

    write_schematic(root_uuid, lib_symbols, items)
    print(f"{SCH}: деталей {len(comps)}, символов {len(lib_symbols)}")


def place_symbol(c, key, pins, sx, sy, pin_net, root_uuid, fields):
    out = [f'\t(symbol (lib_id "{key}") (at {sx} {sy} 0) (unit 1)\n'
           f'\t\t(exclude_from_sim no) (in_bom yes) (on_board yes) (dnp no)\n'
           f'\t\t(uuid "{new_uuid()}")']
    pos = {t: (dx, dy, j) for t, dx, dy, j in fields}

    def prop(name, value, hide):
        dx, dy, j = (0, 0, "center") if hide else pos.get(value, (0, 0, "center"))
        just = "" if j == "center" else f" (justify {j})"
        out.append(f'\t\t(property "{name}" {quote(value)} (at {snap(sx + dx)} {snap(sy + dy)} 0)\n'
                   f'\t\t\t(effects (font (size 1.27 1.27)){just}{" (hide yes)" if hide else ""}))')

    prop("Reference", c["ref"], c["ref"].startswith("#"))
    prop("Value", c["value"], False)
    prop("Footprint", c["footprint"], True)
    prop("Datasheet", "", True)
    for p in pins:
        out.append(f'\t\t(pin "{p[0]}" (uuid "{new_uuid()}"))')
    out.append(f'\t\t(instances (project "Hybryd Radio" (path "/{root_uuid}" '
               f'(reference "{c["ref"]}") (unit 1))))\n\t)')

    for num, px, py, ang in pins:
        x, y = snap(sx + px), snap(sy - py)
        net = pin_net.get((c["ref"], num))
        if net is None or net == "__NOCONNECT":
            out.append(f'\t(no_connect (at {x} {y}) (uuid "{new_uuid()}"))')
            continue
        langle = (ang + 180) % 360
        justify = {0: "left", 180: "right", 90: "left", 270: "right"}[langle]
        out.append(f'\t(label {quote(net)} (at {x} {y} {langle})\n'
                   f'\t\t(effects (font (size 1.27 1.27)) (justify {justify} bottom))\n'
                   f'\t\t(uuid "{new_uuid()}"))')
    return "\n".join(out)


def write_schematic(root_uuid, lib_symbols, items):
    libs = "\n".join("\t\t" + dump(s) for s in lib_symbols.values())
    text = (f'(kicad_sch (version 20260306) (generator "make_schematic.py") (generator_version "10.0")\n'
            f'\t(uuid "{root_uuid}")\n\t(paper "{PAPER[0]}")\n'
            f'\t(title_block (title "IP-Radio: плата-носитель модулей") '
            f'(comment 1 "Сгенерировано из hardware/skidl/carrier.py — правки вносить туда"))\n'
            f'\t(lib_symbols\n{libs}\n\t)\n'
            + "\n".join(items) +
            '\n\t(sheet_instances (path "/" (page "1")))\n)\n')
    SCH.write_text(text, encoding="utf-8")


if __name__ == "__main__":
    main()
