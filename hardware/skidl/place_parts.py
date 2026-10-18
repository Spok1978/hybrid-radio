"""Черновая расстановка всех деталей на плате в один ярус.

Запускать питоном KiCad:
    "%LOCALAPPDATA%/Programs/KiCad/10.0/bin/python.exe" hardware/skidl/place_parts.py

Что делает (по просьбе заказчика 2026-10-08 — «размести всё в один ярус,
я подумаю, как оптимизировать»):
- берёт детали, их посадочные места и цепи из нетлиста SKiDL;
- привязывает каждую деталь к символу схемы (путь /<uuid символа> из
  Hybryd Radio.kicad_sch) — как это делает «Обновить плату из схемы»,
  поэтому потом F8 работает с этими же деталями, а не плодит новые;
- раскладывает столбцами по блокам схемы слева направо; что не поместилось
  на плату 200 × 70, остаётся справа за краем — видно, сколько не влезло;
- контур и отверстия H1-H4 не трогает (их ставит make_board.py).

Это не расстановка под разводку, а наглядная карта площади.
"""

import math
import os
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

import pcbnew

sys.path.insert(0, str(Path(__file__).parent))
from check_carrier import NETLIST, field, parse_sexp  # noqa: E402

from boards import BOARDS, board_of, layers_of  # noqa: E402

BOARD = sys.argv[1] if len(sys.argv) > 1 else "signal"     # power / signal / amp
KICAD = Path(__file__).resolve().parents[1] / "kicad"
PCB = KICAD / BOARD / f"{BOARD}.kicad_pcb"
SCH = KICAD / BOARD / f"{BOARD}.kicad_sch"
STD = Path(os.environ["LOCALAPPDATA"]) / "Programs/KiCad/10.0/share/kicad/footprints"
OWN = KICAD / "hybrid-radio.pretty"

X0, Y0 = 50.0, 50.0                          # контур из make_board.py
W, H = BOARDS[BOARD]["size"]
MARGIN = 5.0                                 # от края платы
GAP = 1.5                                    # между деталями

BLOCK_ORDER = BOARDS[BOARD]["blocks"]


def mm(v):
    return pcbnew.FromMM(v)


def rotate(x, y, rot):
    """Поворот KiCad против часовой при оси Y вниз:
    90° (x, y) -> (y, -x); 180° -> (-x, -y); 270° -> (-y, x)."""
    return {0: (x, y), 90: (y, -x), 180: (-x, -y), 270: (-y, x)}[int(rot) % 360]


def rot_box(off, size, rot):
    """Рамка (l, t, w, h) относительно начала посадочного места после поворота,
    из рамки при 0°. Считаем сами: повторный GetBoundingBox в этой сборке
    pcbnew отдаёт «пустой» SwigPyObject."""
    corners = [rotate(off[0] + dx, off[1] + dy, rot) for dx in (0, size[0]) for dy in (0, size[1])]
    l, t = min(c[0] for c in corners), min(c[1] for c in corners)
    return l, t, max(c[0] for c in corners) - l, max(c[1] for c in corners) - t


def read_netlist():
    root = parse_sexp(NETLIST.read_text(encoding="utf-8"))
    comps, pin_net = [], {}
    for section in root:
        if not isinstance(section, list):
            continue
        if section[0] == "components":
            for c in section[1:]:
                sheet = next(x for x in c if isinstance(x, list) and x[0] == "sheetpath")
                comps.append((field(c, "ref"), field(c, "value"), field(c, "footprint") or "",
                              field(sheet, "names").strip("/").split("/")[-1]))
        elif section[0] == "nets":
            for n in section[1:]:
                for node in n:
                    if isinstance(node, list) and node[0] == "node":
                        pin_net[(field(node, "ref"), field(node, "pin"))] = field(n, "name")
    return comps, pin_net


def kicad_pin_nets():
    """(обозначение, вывод) -> имя цепи, как его даёт сам KiCad по схеме.

    Берём не из нетлиста SKiDL, а из выгрузки KiCad: у него свои имена —
    «/ИМЯ» для локальных меток и «unconnected-(REF-вывод-PadN)» для
    свободных выводов. С любыми другими именами DRC-сверка со схемой даёт
    net_conflict (проверено 2026-10-08)."""
    cli = Path(os.environ["LOCALAPPDATA"]) / "Programs/KiCad/10.0/bin/kicad-cli.exe"
    out = Path(tempfile.gettempdir()) / "carrier_from_sch.xml"
    subprocess.run([str(cli), "sch", "export", "netlist", "--format", "kicadxml",
                    "-o", str(out), str(SCH)], check=True, capture_output=True)
    nets = {}
    for net in ET.parse(out).getroot().iter("net"):
        for node in net.iter("node"):
            nets[(node.get("ref"), node.get("pin"))] = net.get("name")
    return nets


def symbol_uuids():
    """Обозначение -> uuid символа в схеме (корневой лист)."""
    text = SCH.read_text(encoding="utf-8")
    out = {}
    for m in re.finditer(r'\(symbol \(lib_id "[^"]+"\).*?\(uuid "([0-9a-f-]+)"\).*?\(reference "([^"]+)"\)',
                         text, re.S):
        out[m.group(2)] = m.group(1)
    return out


def main():
    comps, _ = read_netlist()
    pin_net = kicad_pin_nets()
    uuids = symbol_uuids()
    plug = pcbnew.PCB_IO_MGR.FindPlugin(pcbnew.PCB_IO_MGR.KICAD_SEXP)

    # Все посадочные места грузим и настраиваем ДО загрузки платы: в этой
    # сборке объекты pcbnew после board.Add иногда «пустеют» (make_board.py).
    prepared = []
    geom = {}
    for ref, value, fpid, block in comps:
        if ref.startswith("#") or not fpid or board_of(block) != BOARD:
            continue
        lib, name = fpid.split(":")
        fp = plug.FootprintLoad(str(OWN if lib == "hybrid-radio" else STD / f"{lib}.pretty"), name)
        fp.SetFPID(pcbnew.LIB_ID(lib, name))
        fp.SetReference(ref)
        fp.SetValue(value)
        if ref in uuids:
            fp.SetPath(pcbnew.KIID_PATH("/" + uuids[ref]))
        bb = fp.GetBoundingBox(False)
        pos = fp.GetPosition()
        off = (pcbnew.ToMM(bb.GetLeft() - pos.x), pcbnew.ToMM(bb.GetTop() - pos.y))
        size = (pcbnew.ToMM(bb.GetWidth()), pcbnew.ToMM(bb.GetHeight()))
        pads = [(p, p.GetNumber()) for p in fp.Pads()]
        # Смещения площадок от начала при 0° и их «радиус» — для развязки у вывода
        pad_off = {}
        for p in fp.Pads():
            d = p.GetPosition() - pos
            r = pcbnew.ToMM(max(p.GetSize().x, p.GetSize().y)) / 2
            pad_off.setdefault(p.GetNumber(), []).append((pcbnew.ToMM(d.x), pcbnew.ToMM(d.y), r))
        geom[ref] = (off, size, pad_off)
        prepared.append((BLOCK_ORDER.index(block) if block in BLOCK_ORDER else 99, ref, fp, off, size, pads))
    prepared.sort(key=lambda t: (t[0], -t[4][1]))

    # Прежнюю надпись — текстом до загрузки: после board.Remove следующий
    # GetDrawings() в этой сборке отдаёт SwigPyObject (2026-10-08).
    from route_board import strip_top
    strip_top(PCB, lambda head: head.startswith(f'(gr_text "{LABEL_PREFIX}'))
    board = pcbnew.LoadBoard(str(PCB))
    for f in list(board.GetFootprints()):
        if not f.GetReference().startswith("H"):
            board.Remove(f)

    nets = {}
    for name in sorted(set(pin_net.values())):
        ni = pcbnew.NETINFO_ITEM(board, name)
        board.Add(ni)
        nets[name] = ni

    # Зоны по руководству разводки (§6.1–6.2): поток слева направо, своя
    # полоса на каждый функциональный узел. Без зон — одна полоса на всю плату.
    zones = [z if len(z) == 5 else (*z, 0.0, H) for z in (BOARDS[BOARD].get("zones") or [(0.0, W, [])])]
    fixed = BOARDS[BOARD].get("fixed", {})
    near = BOARDS[BOARD].get("near", [])
    by_ref = {t[1]: t for t in prepared}
    listed = [r for _z0, _z1, refs, _y0, _y1 in zones for r in refs] + list(fixed) + [n[0] for n in near]
    missing = [r for r in listed if r not in by_ref]
    if missing:
        sys.exit(f"в зонах boards.py есть детали, которых нет на плате: {missing}")
    rest = [t[1] for t in prepared if t[1] not in listed]
    zones = [(z0, z1, list(refs) + (rest if i == len(zones) - 1 else []), y0, y1)
             for i, (z0, z1, refs, y0, y1) in enumerate(zones)]

    keep = 9.0          # углы с отверстиями H1-H4: отверстие в 4 мм от краёв, R 1,6 + зазор
    on_board, overflow = [], []
    rects = {}          # обозначение -> (x0, y0, x1, y1) рамки на плате, мм
    placed = {}         # обозначение -> (x, y, поворот) начала посадочного места, мм

    def attach(ref, fp, pads):
        for pad, num in pads:
            n = pin_net.get((ref, num))
            if n in nets:
                pad.SetNet(nets[n])
        smd = any(p.GetAttribute() == pcbnew.PAD_ATTRIB_SMD for p, _n in pads)
        board.Add(fp)
        if smd and layers_of(BOARD) == 1:
            # Плата односторонняя, медь только снизу: SMD-модуль (RRD-102)
            # ставится на нижнюю сторону. Переворот зеркалит площадки — это
            # и нужно: модуль смотрит микросхемой от платы. Flip — только
            # после board.Add: у footprint без платы он падает (segfault).
            fp.Flip(fp.GetPosition(), pcbnew.FLIP_DIRECTION_LEFT_RIGHT)

    # Детали с заданным местом и поворотом (разъёмы у краёв): начало
    # посадочного места в (x, y) от левого верхнего угла платы, поворот в
    # градусах против часовой стрелки, как в KiCad.
    # Второй вид: (край, координата центра вдоль края, поворот) — деталь
    # прижимается рамкой к краю платы с отступом EDGE (или с четвёртым
    # элементом — своим отступом, мм). Для клеммников и USB-A
    # вход провода/гнездо при повороте 0 смотрит вниз (+y), поэтому
    # «bottom» — 0°, «right» — 90°, «top» — 180°, «left» — 270°.
    EDGE = 1.0
    for ref, spec in fixed.items():
        _blk, ref, fp, off, size, pads = by_ref[ref]
        if isinstance(spec[0], str):
            side, along, rot = spec[:3]
            edge = spec[3] if len(spec) > 3 else EDGE
            fp.SetOrientationDegrees(rot)
            # Рамку после поворота считаем сами из рамки при 0° (off, size):
            # повторный GetBoundingBox в этой сборке pcbnew отдаёт «пустой»
            # SwigPyObject. Поворот KiCad против часовой при оси Y вниз:
            # 90° (x, y) -> (y, -x); 180° -> (-x, -y); 270° -> (-y, x).
            l, t, w, h = rot_box(off, size, rot)
            if side in ("top", "bottom"):
                bx = along - w / 2
                by = edge if side == "top" else H - edge - h
            else:
                by = along - h / 2
                bx = edge if side == "left" else W - edge - w
            fp.SetPosition(pcbnew.VECTOR2I(mm(X0 + bx - l), mm(Y0 + by - t)))
            placed[ref] = (X0 + bx - l, Y0 + by - t, rot)
        else:
            fx, fy, rot = spec
            fp.SetOrientationDegrees(rot)
            fp.SetPosition(pcbnew.VECTOR2I(mm(X0 + fx), mm(Y0 + fy)))
            placed[ref] = (X0 + fx, Y0 + fy, rot)
        px, py, rot = placed[ref]
        l, t, w, h = rot_box(off, size, rot)
        rects[ref] = (px + l, py + t, px + l + w, py + t + h)
        on_board.append(ref)
        attach(ref, fp, pads)

    # Вокруг микросхем с развязкой при упаковке — свободное поле HALO, чтобы
    # place_near было куда поставить конденсатор вплотную к выводу.
    haloed = {n[1] for n in near}
    for z0, z1, refs, y0, y1 in zones:
        left = X0 + z0 + (MARGIN if z0 == 0 else GAP)
        right = X0 + z1 - (MARGIN if z1 >= W else 0)
        top = Y0 + y0 + (MARGIN if y0 == 0 else GAP)
        x, y, col_w = left, top, 0.0
        ymax = Y0 + y1 - (MARGIN if y1 >= H else GAP)
        for ref in refs:
            _blk, ref, fp, off, size, pads = by_ref[ref]
            w, h = size
            # Только у мелких корпусов: у модулей (GY-PCM5102A, CJMCU) ближе
            # вывода не пускает контур самого модуля, а поле съедает зону.
            hl = HALO if ref in haloed and max(size) < 15.0 else 0.0
            w, h = w + 2 * hl, h + 2 * hl
            if y + h > ymax and y > top:
                x, y, col_w = x + col_w + GAP, top, 0.0
            # Углы с отверстиями — квадраты keep × keep: ниже верхнего угла;
            # если тогда задевает нижний — следующий столбец.
            for _ in range(50):
                in_corner_x = x < X0 + keep or (x + w > X0 + W - keep and x < X0 + W)
                if not in_corner_x:
                    break
                if y < Y0 + keep:
                    y = Y0 + keep
                if y + h <= Y0 + H - keep:
                    break
                if x < X0 + keep and col_w == 0.0:
                    x, y = X0 + keep, top
                else:
                    x, y, col_w = x + max(col_w, w) + GAP, top, 0.0
            fp.SetPosition(pcbnew.VECTOR2I(mm(x + hl - off[0]), mm(y + hl - off[1])))
            placed[ref] = (x + hl - off[0], y + hl - off[1], 0)
            rects[ref] = (x + hl, y + hl, x + w - hl, y + h - hl)
            (on_board if x + w <= right + 0.01 and y + h <= ymax + 0.01 else overflow).append(ref)
            attach(ref, fp, pads)
            y += h + GAP
            col_w = max(col_w, w)

    place_near(near, by_ref, geom, placed, rects, pin_net, attach, on_board, keep)
    place_label(board, rects, keep)
    ref_pos = place_refs(by_ref, geom, placed, rects, keep)

    board.Save(str(PCB))
    import json
    tmp = Path(tempfile.gettempdir()) / f"refs_{BOARD}.json"
    tmp.write_text(json.dumps(ref_pos), encoding="utf-8")
    r = subprocess.run([sys.executable, __file__, BOARD, "--refs", str(tmp)], capture_output=True,
                       text=True, encoding="utf-8", errors="replace")
    out = [x for x in (r.stdout or "").splitlines() if "обозначений" in x]
    print(out[-1] if out else f"--refs: ошибка, код {r.returncode}: {(r.stderr or '')[-300:]}")
    print(f"деталей {len(prepared)}: на плате {len(on_board)}, за краем {len(overflow)}")
    area = sum((w + GAP) * (h + GAP) for *_x, (w, h), _p in prepared)
    print(f"площадь деталей с зазором {GAP} мм: {area:.0f} мм² = "
          f"{100 * area / (W * H):.0f} % платы {W:g} x {H:g}")
    print("не влезли в свою зону:", " ".join(overflow))


# Надпись на плате: название, ревизия, дата (руководство разводки §9.2).
# Латиница: шрифт KiCad кириллицу рисует, но при травлении мелкий текст
# надёжнее простой. Ревизия — до первого изготовления «1».
LABEL_PREFIX = "IP-Radio"
REVISION = "1"
LABEL_H = 1.5           # высота букв, мм


def place_label(board, rects, keep):
    """Надпись в первое свободное место: по рядам сверху вниз, слева направо."""
    import datetime
    text = f"{LABEL_PREFIX} {BOARD} board rev {REVISION} {datetime.date.today():%Y-%m-%d}"
    w, h = len(text) * LABEL_H * 0.95, LABEL_H * 1.6       # оценка рамки штрихового шрифта
    for j in range(int((H - h) / 0.5)):
        for i in range(int((W - w) / 0.5)):
            box = (X0 + i * 0.5, Y0 + j * 0.5, X0 + i * 0.5 + w, Y0 + j * 0.5 + h)
            if fits(box, rects, keep):
                t = pcbnew.PCB_TEXT(board)
                t.SetText(text)
                t.SetLayer(pcbnew.F_SilkS)
                t.SetTextSize(pcbnew.VECTOR2I(mm(LABEL_H), mm(LABEL_H)))
                t.SetTextThickness(mm(0.25))
                t.SetPosition(pcbnew.VECTOR2I(mm((box[0] + box[2]) / 2), mm((box[1] + box[3]) / 2)))
                board.Add(t)
                rects["label"] = box
                print(f"надпись «{text}» у ({box[0] - X0:.1f}, {box[1] - Y0:.1f}) мм от угла")
                return
    print("надписи места нет")


REF_H = 1.0             # высота позиционных обозначений, мм


def place_refs(by_ref, geom, placed, rects, keep):
    """Позиционные обозначения — туда, где они читаемы и ни на что не
    налезают (руководство разводки §9.2: «не залезают на пады»; DRC
    2026-10-08 — 11 обозначений на контурах соседей при плотной упаковке).

    Кандидаты по порядку: над деталью, под ней, слева, справа, сдвиги вдоль
    верха и низа, внутри своей рамки. Рамка текста не должна задевать
    площадки (любые, с запасом 0,25 мм), рамки чужих деталей, уже
    поставленные обозначения, надпись платы и углы с отверстиями."""
    pads = []
    for ref, (px, py, rot) in placed.items():
        for lst in geom[ref][2].values():
            for dx, dy, r in lst:
                rx, ry = rotate(dx, dy, rot)
                pads.append((px + rx - r - 0.25, py + ry - r - 0.25, px + rx + r + 0.25, py + ry + r + 0.25))
    texts = [rects["label"]] if "label" in rects else []

    def hit(a, b):
        return a[0] < b[2] and a[2] > b[0] and a[1] < b[3] and a[3] > b[1]

    miss, out = [], {}
    for ref in sorted(placed):
        rot = placed[ref][2]
        w, h = len(ref) * REF_H * 0.95 + 0.2, REF_H * 1.4
        if int(rot) % 180:                 # текст поворачивается вместе с деталью
            w, h = h, w
        x0, y0, x1, y1 = rects[ref]
        cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
        d = 0.15
        cands = [(cx, y0 - h / 2 - d), (cx, y1 + h / 2 + d), (x0 - w / 2 - d, cy), (x1 + w / 2 + d, cy)]
        for k in range(1, 8):
            for sx in (-k * 1.0, k * 1.0):
                cands += [(cx + sx, y0 - h / 2 - d), (cx + sx, y1 + h / 2 + d)]
        cands.append((cx, cy))
        for tx, ty in cands:
            box = (tx - w / 2, ty - h / 2, tx + w / 2, ty + h / 2)
            if box[0] < X0 + 0.5 or box[1] < Y0 + 0.5 or box[2] > X0 + W - 0.5 or box[3] > Y0 + H - 0.5:
                continue
            if any(box[0] < c + keep and box[2] > c and box[1] < e + keep and box[3] > e
                   for c in (X0, X0 + W - keep) for e in (Y0, Y0 + H - keep)):
                continue
            if any(hit(box, pb) for pb in pads) or any(hit(box, t) for t in texts):
                continue
            if any(hit(box, rb) for r2, rb in rects.items() if r2 not in (ref, "label")):
                continue
            out[ref] = (tx, ty)
            texts.append(box)
            break
        else:
            miss.append(ref)
    print("обозначениям нет места:", " ".join(miss) or "нет")
    return out


def apply_refs(path_json):
    """Отдельный процесс: в этой сборке pcbnew поле Reference() у footprint
    после board.Add — SwigPyObject без методов (2026-10-08). На свежей
    загрузке платы — работает."""
    import json
    pos = json.loads(Path(path_json).read_text(encoding="utf-8"))
    board = pcbnew.LoadBoard(str(PCB))
    n = 0
    for f in board.GetFootprints():
        if f.GetReference() in pos:
            tx, ty = pos[f.GetReference()]
            fld = f.Reference()
            fld.SetTextSize(pcbnew.VECTOR2I(mm(REF_H), mm(REF_H)))
            fld.SetTextThickness(mm(0.15))
            fld.SetPosition(pcbnew.VECTOR2I(mm(tx), mm(ty)))
            n += 1
    board.Save(str(PCB))
    print(f"обозначений переставлено: {n}")


HALO = 3.0          # поле вокруг микросхемы с развязкой при упаковке, мм
NEAR_GAP = 0.25     # зазор рамок развязки до соседей, мм (рамка уже с запасом вокруг площадок)


def place_near(near, by_ref, geom, placed, rects, pin_net, attach, on_board, keep):
    """Развязка вплотную к выводу питания (руководство §4.5: <= 2 мм).

    Перебор: поворот 0/90/180/270, точка для площадки конденсатора на той же
    цепи по кольцам вокруг вывода микросхемы (шаг 0,25 мм) — ближайшая, где
    рамка не задевает соседей, края платы и углы с отверстиями."""
    for cref, iref, pins in near:
        _blk, _r, fp, off, size, pads = by_ref[cref]
        ipos = placed[iref]
        targets = []
        for pin in pins:
            for dx, dy, r_i in geom[iref][2][pin]:
                rx, ry = rotate(dx, dy, ipos[2])
                targets.append((ipos[0] + rx, ipos[1] + ry, r_i, pin_net[(iref, pin)]))
        best = None
        for tx, ty, r_i, net in targets:
            cpin = next(n for (r, n), v in pin_net.items() if r == cref and v == net)
            for cdx, cdy, r_c in geom[cref][2][cpin]:
                for k in range(0, 61):
                    rr = k * 0.25
                    if best and rr - r_i - r_c >= best[0]:
                        break
                    for a in range(0, 360, 15):
                        cx = tx + rr * math.cos(math.radians(a))
                        cy = ty + rr * math.sin(math.radians(a))
                        for rot in (0, 90, 180, 270):
                            ox, oy = rotate(cdx, cdy, rot)
                            px, py = cx - ox, cy - oy
                            l, t, w, h = rot_box(off, size, rot)
                            box = (px + l, py + t, px + l + w, py + t + h)
                            if not fits(box, rects, keep):
                                continue
                            gap = rr - r_i - r_c
                            if best is None or gap < best[0]:
                                best = (gap, px, py, rot, box)
        if best is None:
            print(f"{cref}: места у {iref} нет")
            continue
        gap, px, py, rot, box = best
        fp.SetOrientationDegrees(rot)
        fp.SetPosition(pcbnew.VECTOR2I(mm(px), mm(py)))
        placed[cref] = (px, py, rot)
        rects[cref] = box
        on_board.append(cref)
        attach(cref, fp, pads)
        print(f"{cref} у {iref}: между площадками ~{max(gap, 0):.1f} мм")


def fits(box, rects, keep):
    x0, y0, x1, y1 = box
    e = 1.0
    if x0 < X0 + e or y0 < Y0 + e or x1 > X0 + W - e or y1 > Y0 + H - e:
        return False
    for cx in (X0, X0 + W - keep):
        for cy in (Y0, Y0 + H - keep):
            if x0 < cx + keep and x1 > cx and y0 < cy + keep and y1 > cy:
                return False
    g = NEAR_GAP
    return all(x1 + g <= a0 or x0 - g >= a1 or y1 + g <= b0 or y0 - g >= b1
               for a0, b0, a1, b1 in rects.values())


if __name__ == "__main__":
    if "--refs" in sys.argv:
        apply_refs(sys.argv[sys.argv.index("--refs") + 1])
    else:
        sys.exit(main())
