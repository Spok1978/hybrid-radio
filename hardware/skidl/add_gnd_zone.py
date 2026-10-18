"""Земляные полигоны по всему контуру платы, сшивка слоёв и заливка.

    "%LOCALAPPDATA%/Programs/KiCad/10.0/bin/python.exe" hardware/skidl/add_gnd_zone.py signal

Односторонняя плата (docs/33, §15): земля — сплошной медью на нижнем слое,
так она соединяет площадки GND, которые не развёл трассировщик, и экономит
травильный раствор. Двусторонняя — полигон GND на обоих слоях (руководство
разводки §7.5). Соединение площадок с полигоном — через тепловые перемычки
(иначе не пропаять, руководство §5.2). Зазор полигона 0,6 мм — для домашнего
травления (ZONE_CLEARANCE); от края платы 1,0 мм.

Сшивка двусторонней платы (руководство §7.5, §8.1) — переходы GND:
- в каждом куске заливки не меньше двух, иначе кусок висит на одной
  площадке, а сторона платы под ним — «мёртвая» для обратного тока;
- сеткой с шагом STITCH_MM по всей плате и вдоль края. Шаг по формуле
  руководства «≤ λ/20 высшей рабочей частоты»: выше всего на плате FM-вход,
  до 108 МГц; в FR-4 (εr ≈ 4,5) λ = 3e8 / 108e6 / √4,5 ≈ 1,31 м, λ/20 ≈ 65 мм.
  Взято 20 мм — запас на гармоники цифровых фронтов (λ/20 при ~350 МГц).
  Сетка 3–5 мм из §8.1 — для плат до ~1 ГГц; при домашнем изготовлении
  каждый переход — отверстие и пропаянная проволочка, так что их число
  держим разумным.
Переход ставится только туда, где круг его площадки целиком лежит в
заливке GND обоих слоёв: заливка и так держит зазор 1,0 мм до чужой меди.
"""

import math
import sys
from pathlib import Path

import pcbnew

sys.path.insert(0, str(Path(__file__).parent))
from boards import layers_of  # noqa: E402

BOARD = sys.argv[1] if len(sys.argv) > 1 else "signal"
PCB = Path(__file__).resolve().parents[1] / "kicad" / BOARD / f"{BOARD}.kicad_pcb"

# Зазор заливки до чужой меди. Было 1,0 — заливка отступала от каждой
# дорожки на 1 мм и обрезала спицы термобарьеров (9 starved_thermal на
# сигнальной, 2026-10-08) и рвала полигон на куски. Руководство §8.1:
# зазор зоны ≥ общему зазору (0,4 мм, make_boards.py); 0,6 — с запасом для
# домашнего травления.
ZONE_CLEARANCE = 0.6
STITCH_MM = 20.0        # шаг сетки сшивки, обоснование — в шапке
VIA_D, VIA_DRILL = 2.0, 0.8     # как «виа» классов цепей (make_boards.py): отверстие под проволоку
# Площадки со сплошным соединением с заливкой вместо термобарьера. DRC
# сигнальной платы 2026-10-08 (starved_thermal, нужно ≥ 2 спиц):
# - J15-2 — земля входа 5 В с платы питания: силовая площадка, по
#   руководству §5.2 ей и положено solid;
# - J9-2, J10-2, C22-2, U5-21 — к ним прилегает мелкий кусок заливки F.Cu,
#   который держится только на этой площадке («spokes connected to isolated
#   island»); меди вокруг мало, пайку solid не затруднит;
# - C24-2 — на F.Cu влезла одна спица; снизу (сторона пайки) термобарьер.
# Список — по результату трассировки; после перетрассировки сверять с DRC.
# Плата питания: силовые площадки GND (руководство §5.2: solid для силовых,
# где важен минимум сопротивления) — вход J1, выходы J13 (12 В на
# усилители, до ~4 А), J14, J2 (USB), земля DC-DC U1, объёмные C1/C4, TVS D2
# (импульсный ток). Паять — паяльником помощнее: медь заливки тянет тепло.
SOLID_PADS = {"signal": ["J15-2", "J9-2", "J10-2", "C22-2", "U5-21", "C24-2"],
              "power": ["J1-2", "J13-2", "J14-2", "J2-4", "U1-2", "U1-4", "C1-2", "C4-2", "D2-2"]}
GRID = 1.0              # шаг перебора точек-кандидатов, мм


def mm(v):
    return pcbnew.FromMM(v)


def add_zones(board, gnd, layers):
    bb = board.GetBoardEdgesBoundingBox()
    x0, y0, x1, y1 = bb.GetLeft(), bb.GetTop(), bb.GetRight(), bb.GetBottom()
    for layer in layers:
        zone = pcbnew.ZONE(board)
        zone.SetLayer(layer)
        zone.SetNet(gnd)
        zone.SetLocalClearance(mm(ZONE_CLEARANCE))
        zone.SetMinThickness(mm(0.5))
        zone.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL)
        zone.SetThermalReliefGap(mm(0.6))
        zone.SetThermalReliefSpokeWidth(mm(0.8))
        outline = zone.Outline()
        outline.NewOutline()
        for x, y in ((x0, y0), (x1, y0), (x1, y1), (x0, y1)):
            outline.Append(x, y)
        board.Add(zone)


def stitch_points(board):
    """Точки переходов GND по заливке обоих слоёв (после первой заливки)."""
    fills = {}
    for z in board.Zones():
        fills[z.GetLayer()] = z.GetFilledPolysList(z.GetLayer())
    f_fill, b_fill = fills[pcbnew.F_Cu], fills[pcbnew.B_Cu]
    r = VIA_D / 2
    # Отверстия площадок и переходов: не ближе большего из hole_clearance и
    # min_hole_to_hole правил платы (0,5 мм, make_boards.py) плюс 0,1 мм.
    ds = board.GetDesignSettings()
    h2h = pcbnew.ToMM(max(ds.m_HoleToHoleMin, ds.m_HoleClearance)) + 0.1
    holes = [(pcbnew.ToMM(p.GetPosition().x), pcbnew.ToMM(p.GetPosition().y),
              pcbnew.ToMM(max(p.GetDrillSize().x, p.GetDrillSize().y)) / 2)
             for f in board.GetFootprints() for p in f.Pads() if p.GetDrillSize().x > 0]
    holes += [(pcbnew.ToMM(v.GetPosition().x), pcbnew.ToMM(v.GetPosition().y), pcbnew.ToMM(v.GetDrill()) / 2)
              for v in board.GetTracks() if v.GetClass() == "PCB_VIA"]

    def ok(x, y):
        # медь перехода (VIA_D) до края чужого отверстия — по hole_clearance,
        # это строже, чем сверло до сверла (DRC 2026-10-08: 0,4992 у H2)
        if any(math.hypot(x - hx, y - hy) < VIA_D / 2 + hr + h2h for hx, hy, hr in holes):
            return False
        pts = [(x, y)] + [(x + r * math.cos(a * math.pi / 6), y + r * math.sin(a * math.pi / 6))
                          for a in range(12)]
        return all(f.Contains(pcbnew.VECTOR2I(mm(px), mm(py))) for f in (f_fill, b_fill) for px, py in pts)

    bb = board.GetBoardEdgesBoundingBox()
    X0, Y0 = pcbnew.ToMM(bb.GetLeft()), pcbnew.ToMM(bb.GetTop())
    W, H = pcbnew.ToMM(bb.GetWidth()), pcbnew.ToMM(bb.GetHeight())
    cand = [(X0 + i * GRID, Y0 + j * GRID) for j in range(1, int(H / GRID)) for i in range(1, int(W / GRID))]
    cand = [c for c in cand if ok(*c)]

    def frag(fill, x, y):
        pr = probe.get((x, y), 0.0)
        pts = [(x, y)] + [(x + pr * math.cos(a * math.pi / 4), y + pr * math.sin(a * math.pi / 4))
                          for a in range(8)] if pr else [(x, y)]
        for px, py in pts:
            p = pcbnew.VECTOR2I(mm(px), mm(py))
            i = next((i for i in range(fill.OutlineCount()) if fill.Contains(p, i)), None)
            if i is not None:
                return i
        return None

    # Уже стоящие соединения слоёв GND: переходы трассировки и выводные
    # площадки GND — металлизированное отверстие площадки сшивает слои так
    # же, как переход (на самодельной плате — пропаянный с двух сторон вывод).
    existing = [(pcbnew.ToMM(v.GetPosition().x), pcbnew.ToMM(v.GetPosition().y))
                for v in board.GetTracks() if v.GetClass() == "PCB_VIA" and v.GetNetname() == "/GND"]
    # У площадки центр — в вырезе термобарьера, не в заливке: к куску
    # заливки её относим по кольцу точек сразу за тепловым зазором.
    probe = {}
    for f in board.GetFootprints():
        for p in f.Pads():
            if p.GetNetname() == "/GND" and p.GetAttribute() == pcbnew.PAD_ATTRIB_PTH:
                xy = (pcbnew.ToMM(p.GetPosition().x), pcbnew.ToMM(p.GetPosition().y))
                existing.append(xy)
                probe[xy] = pcbnew.ToMM(max(p.GetSize().x, p.GetSize().y)) / 2 + 0.6 + 0.4
    chosen = []

    def far_enough(x, y, d):
        return all(math.hypot(x - a, y - b) >= d for a, b in chosen + existing)

    # 1) каждый кусок заливки на каждом слое — не меньше двух переходов
    for fill in (f_fill, b_fill):
        for i in range(fill.OutlineCount()):
            inside = [c for c in cand if fill.Contains(pcbnew.VECTOR2I(mm(c[0]), mm(c[1])), i)]
            have = [p for p in chosen + existing if frag(fill, *p) == i]
            for c in inside:
                if len(have) >= 2:
                    break
                if all(math.hypot(c[0] - a, c[1] - b) >= 5.0 for a, b in have) and far_enough(*c, 3.0):
                    chosen.append(c)
                    have.append(c)

    # 2) сетка STITCH_MM по всей плате (и вдоль края — первый ряд в 3 мм от него)
    n_x, n_y = int(W // STITCH_MM) + 1, int(H // STITCH_MM) + 1
    for j in range(n_y + 1):
        for i in range(n_x + 1):
            gx = min(X0 + 3 + i * (W - 6) / n_x, X0 + W - 3)
            gy = min(Y0 + 3 + j * (H - 6) / n_y, Y0 + H - 3)
            near = sorted((math.hypot(c[0] - gx, c[1] - gy), c) for c in cand)
            for d, c in near:
                if d > STITCH_MM / 3:
                    break
                if far_enough(*c, STITCH_MM * 0.6):
                    chosen.append(c)
                    break
    # Отчёт: куски заливки, где переходов меньше двух (мелкие — некуда поставить)
    short = []
    for name, fill in (("F.Cu", f_fill), ("B.Cu", b_fill)):
        for i in range(fill.OutlineCount()):
            k = sum(1 for p in chosen + existing if frag(fill, *p) == i)
            if k < 2:
                area = fill.Outline(i).Area() / 1e12
                short.append(f"{name}#{i} {area:.0f} мм²: {k}")
    print("кусков заливки с < 2 переходами:", ", ".join(short) or "нет")
    return chosen


def main():
    # Прежние полигоны и переходы сшивки — текстом до загрузки: после
    # board.Remove следующий вызов pcbnew в этой сборке отдаёт SwigPyObject
    # (2026-10-08). Переход сшивки = переход GND, к которому не подходит ни
    # одна дорожка; переходы трассировки (с дорожками) остаются.
    import re
    from route_board import strip_top
    text = PCB.read_text(encoding="utf-8")
    ends = set(re.findall(r"\((?:start|end) ([-\d.]+ [-\d.]+)\)", text))

    def old(block):
        if block.startswith("(zone"):
            return True
        if block.startswith("(via") and '(net "/GND")' in block:
            at = re.search(r"\(at ([-\d.]+ [-\d.]+)\)", block)
            return at is not None and at.group(1) not in ends
        return False
    strip_top(PCB, old)
    board = pcbnew.LoadBoard(str(PCB))
    # До board.Add — после него объекты pcbnew в этой сборке «пустеют».
    solid = set(SOLID_PADS.get(BOARD, []))
    for f in board.GetFootprints():
        for p in f.Pads():
            if f"{f.GetReference()}-{p.GetNumber()}" in solid:
                p.SetLocalZoneConnection(pcbnew.ZONE_CONNECTION_FULL)
                solid.discard(f"{f.GetReference()}-{p.GetNumber()}")
    if solid:
        sys.exit(f"SOLID_PADS: нет таких площадок {sorted(solid)}")
    gnd = board.FindNet("/GND")
    # Односторонняя — полигон только на B.Cu; двусторонняя — на обоих слоях.
    layers = [pcbnew.B_Cu] if layers_of(BOARD) == 1 else [pcbnew.F_Cu, pcbnew.B_Cu]
    add_zones(board, gnd, layers)
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())

    n = 0
    if len(layers) == 2:
        pts = stitch_points(board)
        for x, y in pts:
            v = pcbnew.PCB_VIA(board)
            v.SetPosition(pcbnew.VECTOR2I(mm(x), mm(y)))
            v.SetViaType(pcbnew.VIATYPE_THROUGH)
            v.SetWidth(mm(VIA_D))
            v.SetDrill(mm(VIA_DRILL))
            v.SetNet(gnd)
            board.Add(v)
        n = len(pts)
    board.Save(str(PCB))
    print(f"{PCB}: полигоны GND, слоёв: {len(layers)}; переходов сшивки: {n} "
          f"(заливку перезаливает DRC с --refill-zones)")


if __name__ == "__main__":
    main()
