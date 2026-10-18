"""Автотрассировка платы Freerouting'ом, только по нижнему слою (B.Cu).

Запускать питоном KiCad:
    "%LOCALAPPDATA%/Programs/KiCad/10.0/bin/python.exe" hardware/skidl/route_board.py power

Плата односторонняя (решение 2026-10-08): медь только снизу. Верхний слой
F.Cu в задании для Freerouting выключен. Что не развелось — остаётся
связью (ratsnest): это места под проволочные перемычки, их видно в KiCad
и в отчёте DRC как «unconnected».

Freerouting 2.5.0 (Java) — C:/Users/telen/tools/freerouting/freerouting-2.5.0.jar
(github.com/freerouting/freerouting, релиз v2.5.0 от 2026-10-05).
"""

import re
import subprocess
import sys
from pathlib import Path

import pcbnew

BOARD = sys.argv[1] if len(sys.argv) > 1 else "power"
PASSES = int(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[2].isdigit() else 30
KICAD = Path(__file__).resolve().parents[1] / "kicad"
PCB = KICAD / BOARD / f"{BOARD}.kicad_pcb"
WORK = KICAD / BOARD / "route"
# Java-версия Freerouting 2.5.0 + переносимая Java 25 (Temurin 25.0.4.1).
# Нативная freerouting-cli.exe того же релиза падает с UnsupportedFeatureError
# (сериализация ComponentOutline в сборке GraalVM) и зависает — не использовать.
JAVA = Path(r"C:\Users\telen\tools\jdk-25.0.4.1+1-jre\bin\java.exe")
FREEROUTING = Path(r"C:\Users\telen\tools\freerouting\freerouting-2.5.0.jar")

sys.path.insert(0, str(Path(__file__).parent))
from boards import layers_of  # noqa: E402

LAYERS = layers_of(BOARD)

# Настройки автотрассировки в формате Specctra, как их пишет сам Freerouting.
# Односторонняя плата: F.Cu выключен, переходы запрещены. Двусторонняя:
# оба слоя, переходы разрешены, верх — горизонтально, низ — вертикально
# (руководство разводки §7.2: соседние слои ортогонально).
if LAYERS == 1:
    AUTOROUTE = """
    (autoroute_settings
      (fanout off) (autoroute on) (postroute on) (vias off)
      (via_costs 1000) (plane_via_costs 1000) (start_ripup_costs 100) (start_pass_no 1)
      (layer_rule F.Cu (active off) (preferred_direction horizontal)
        (preferred_direction_trace_costs 1.0) (against_preferred_direction_trace_costs 2.0))
      (layer_rule B.Cu (active on) (preferred_direction vertical)
        (preferred_direction_trace_costs 1.0) (against_preferred_direction_trace_costs 1.0))
    )"""
else:
    AUTOROUTE = """
    (autoroute_settings
      (fanout off) (autoroute on) (postroute on) (vias on)
      (via_costs 50) (plane_via_costs 5) (start_ripup_costs 100) (start_pass_no 1)
      (layer_rule F.Cu (active on) (preferred_direction horizontal)
        (preferred_direction_trace_costs 1.0) (against_preferred_direction_trace_costs 2.0))
      (layer_rule B.Cu (active on) (preferred_direction vertical)
        (preferred_direction_trace_costs 1.0) (against_preferred_direction_trace_costs 2.0))
    )"""

# Отступ меди от края: в правилах KiCad 1,0 мм, а Freerouting держит от
# контура только общий зазор 0,4 мм (DRC 2026-10-08: 8 дорожек GND у края).
# Контур в задании для Freerouting сжимается на разницу.
EDGE_INSET_UM = 750      # 600 давало 0,87–0,96 мм (DRC сигнальной, 2026-10-08)


def inset_boundary(text):
    m = re.search(r"\(boundary\s*\(path pcb 0([\s\d\-.]+)\)", text)
    if not m:
        sys.exit("не нашёл (boundary (path pcb 0 ...)) в DSN")
    nums = [float(v) for v in m.group(1).split()]
    xs, ys = nums[0::2], nums[1::2]
    cx, cy = (min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2
    new = []
    for x, y in zip(xs, ys):
        new += [x + (EDGE_INSET_UM if x < cx else -EDGE_INSET_UM),
                y + (EDGE_INSET_UM if y < cy else -EDGE_INSET_UM)]
    path = " ".join(f"{v:g}" for v in new)
    return text[:m.start(1)] + " " + path + text[m.end(1):]


def strip_copper(pcb):
    """Удалить из .kicad_pcb верхнеуровневые (segment/arc/via/zone ...)."""
    return strip_top(pcb, lambda head: re.match(r"\((segment|arc|via|zone)[\s(]", head))


def strip_top(pcb, match):
    """Удалить из .kicad_pcb верхнеуровневые элементы, для текста которых
    match(...) истинно. Текстом, не через pcbnew."""
    text = pcb.read_text(encoding="utf-8")
    if "\r\n" in text:
        sys.exit(f"{pcb}: переводы строк CRLF, KiCad пишет LF — файл правился вручную?")
    out, i, depth, start, n = [], 0, 0, None, 0
    keep_from = 0
    in_str = False
    while i < len(text):
        c = text[i]
        if in_str:
            if c == "\\":
                i += 1
            elif c == '"':
                in_str = False
        elif c == '"':
            in_str = True
        elif c == "(":
            depth += 1
            if depth == 2:
                start = i
        elif c == ")":
            if depth == 2 and match(text[start:i + 1]):
                ls = text.rfind("\n", 0, start) + 1       # вместе с отступом и переводом строки
                le = i + 1
                if le < len(text) and text[le] == "\n":
                    le += 1
                out.append(text[keep_from:ls])
                keep_from = le
                n += 1
            depth -= 1
        i += 1
    out.append(text[keep_from:])
    pcb.write_text("".join(out), encoding="utf-8")
    return n


def main():
    WORK.mkdir(exist_ok=True)
    dsn, ses = WORK / f"{BOARD}.dsn", WORK / f"{BOARD}.ses"

    # Каждый запуск — с чистого листа: прежние дорожки, переходы и полигоны
    # (их заливает add_gnd_zone.py после трассировки) убрать из файла.
    # Текстом, а не через board.Remove: в этой сборке после Remove следующий
    # вызов Zones()/Tracks() падает (access violation / SwigPyObject, 2026-10-08).
    strip_copper(PCB)
    board = pcbnew.LoadBoard(str(PCB))
    if not pcbnew.ExportSpecctraDSN(board, str(dsn)):
        sys.exit("ExportSpecctraDSN: ошибка")

    text = dsn.read_text(encoding="utf-8")
    # Настройки — ВНУТРИ секции (structure ...), в её конце: после описаний
    # слоёв (layer_rule ссылается на них) и перед закрывающей скобкой.
    # Вне structure Freerouting их молча пропускает (51 отрезок лёг на
    # «выключенный» F.Cu), в начале structure — падает «layer not found»
    # (оба случая проверены 2026-10-08).
    text, n = re.subn(r"\n(\s*)\)\n(\s*)\(placement",
                      lambda m: AUTOROUTE + "\n" + m.group(1) + ")\n" + m.group(2) + "(placement",
                      text, count=1)
    if n != 1:
        sys.exit("не нашёл конец (structure перед (placement в DSN")
    text = inset_boundary(text)
    dsn.write_text(text, encoding="utf-8")

    if ses.exists():
        ses.unlink()
    r = subprocess.run([str(JAVA), "-jar", str(FREEROUTING), "--gui.enabled=false",
                        "-de", str(dsn), "-do", str(ses), "-mp", str(PASSES)],
                       capture_output=True, text=True, encoding="utf-8", errors="replace",
                       timeout=1800)
    (WORK / "freerouting.log").write_text(r.stdout + r.stderr, encoding="utf-8")
    if not ses.exists():
        sys.exit(f"Freerouting не выдал .ses (код {r.returncode}), лог: {WORK / 'freerouting.log'}")

    # Загрузка .ses — в отдельном процессе: в этой сборке объекты pcbnew после
    # Remove/Save в том же процессе «пустеют» (SwigPyObject без Save, 2026-10-08).
    r = subprocess.run([sys.executable, __file__, BOARD, "--import"], capture_output=True,
                       text=True, encoding="utf-8", errors="replace")
    out = (r.stdout + r.stderr).strip()
    if r.returncode or "загружена" not in out:
        sys.exit(f"импорт .ses не удался: {out}")
    r = subprocess.run([sys.executable, __file__, BOARD, "--widen"], capture_output=True,
                       text=True, encoding="utf-8", errors="replace")
    out = (r.stdout + r.stderr).strip()
    print(out.splitlines()[-1] if out else f"--widen: пустой вывод, код {r.returncode}")
    fit_class_widths()


# Нарушения DRC, из-за которых расширенный отрезок возвращается к прежней ширине
WIDEN_BLOCKERS = {"clearance", "copper_edge_clearance", "hole_clearance", "shorting_items",
                  "track_width", "solder_mask_bridge"}


def fit_class_widths():
    """Дорожки уже ширины своего класса цепей — расширить до неё (руководство
    §4.1: ширина по току; Freerouting сужает у площадок, на плате питания
    2026-10-08 +12V шла 25 мм шириной 1,9 вместо 2,5, +5V — 0,9 вместо 1,5).
    Где расширение даёт нарушение DRC — отрезок возвращается к прежней
    ширине; такие места выводятся списком."""
    import json
    import os
    state = WORK / "class_widen.json"
    cli = Path(os.environ["LOCALAPPDATA"]) / "Programs/KiCad/10.0/bin/kicad-cli.exe"
    r = subprocess.run([sys.executable, __file__, BOARD, "--classwiden", str(state)],
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode or not state.exists():
        sys.exit(f"--classwiden: ошибка {r.returncode}: {(r.stderr or '')[-400:]}")
    orig = json.loads(state.read_text(encoding="utf-8"))
    rpt = WORK / "drc_widen.json"
    subprocess.run([str(cli), "pcb", "drc", "--format", "json", "--severity-error", "-o", str(rpt), str(PCB)],
                   capture_output=True)
    bad = set()
    for v in json.loads(rpt.read_text(encoding="utf-8")).get("violations", []):
        if v["type"] in WIDEN_BLOCKERS:
            bad.update(i["uuid"] for i in v["items"] if i["uuid"] in orig)
    if bad:
        (WORK / "class_revert.json").write_text(json.dumps({u: orig[u] for u in bad}), encoding="utf-8")
        subprocess.run([sys.executable, __file__, BOARD, "--revert", str(WORK / "class_revert.json")],
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    print(f"до ширины класса расширено отрезков: {len(orig) - len(bad)}, "
          f"оставлено узкими из-за зазоров: {len(bad)}")


def class_widen(state_path):
    import json
    from make_boards import CLASSES
    board = pcbnew.LoadBoard(str(PCB))
    orig = {}
    for t in board.GetTracks():
        if t.GetClass() != "PCB_TRACK":
            continue
        want = pcbnew.FromMM(CLASSES.get(t.GetNetClassName(), CLASSES["Default"]))
        if t.GetWidth() < want:
            orig[t.m_Uuid.AsString()] = t.GetWidth()
            t.SetWidth(want)
    board.Save(str(PCB))
    Path(state_path).write_text(json.dumps(orig), encoding="utf-8")


def revert_widths(path):
    import json
    back = json.loads(Path(path).read_text(encoding="utf-8"))
    board = pcbnew.LoadBoard(str(PCB))
    for t in board.GetTracks():
        u = t.m_Uuid.AsString()
        if t.GetClass() == "PCB_TRACK" and u in back:
            t.SetWidth(back[u])
    board.Save(str(PCB))


def import_ses():
    ses = WORK / f"{BOARD}.ses"
    board = pcbnew.LoadBoard(str(PCB))
    if not pcbnew.ImportSpecctraSES(board, str(ses)):
        sys.exit("ImportSpecctraSES: ошибка")
    board.Save(str(PCB))
    print(f"{PCB}: трассировка загружена; лог {WORK / 'freerouting.log'}")


def widen():
    """Дорожки уже минимума правил расширить до минимума.

    Freerouting у площадок сужает дорожку (на сигнальной плате 2026-10-08 —
    два отрезка I2C_SCL по 0,3 мм). Отдельным процессом после --import (см.
    про SwigPyObject выше). Зазоры после расширения проверяет DRC.
    """
    board = pcbnew.LoadBoard(str(PCB))
    wmin = board.GetDesignSettings().m_TrackMinWidth
    n = 0
    tracks = [t for t in board.GetTracks() if t.GetClass() == "PCB_TRACK"]
    for t in tracks:
        if t.GetWidth() < wmin:
            t.SetWidth(wmin)
            n += 1
    k = chamfer(board, tracks)
    board.Save(str(PCB))
    print(f"{PCB}: трассировка загружена, расширено дорожек до {pcbnew.ToMM(wmin):g} мм: {n}, "
          f"срезано углов 90° и острее: {k}")


def chamfer(board, tracks):
    """Углы 90° и острее срезать отрезком 45° (руководство §7.2, §11 п.7:
    прямой угол — скачок ширины и «кислотная ловушка» при травлении).

    Угол — точка, где на одном слое сходятся ровно два отрезка одной цепи и
    нет площадки или перехода (там угол закрыт медью площадки). Оба отрезка укорачиваются на c (до 1 мм, не
    больше 45 % длины), концы соединяются новым отрезком той же ширины.
    Сначала все расчёты, потом правки и одно сохранение — объекты pcbnew в
    этой сборке после Add «пустеют» (см. выше)."""
    import math
    ends = {}
    for t in tracks:
        for which in ("s", "e"):
            p = t.GetStart() if which == "s" else t.GetEnd()
            ends.setdefault((p.x, p.y, t.GetLayer(), t.GetNetCode()), []).append((t, which))
    blocked = set()
    for v in board.GetTracks():
        if v.GetClass() == "PCB_VIA":
            blocked.add((v.GetPosition().x, v.GetPosition().y))
    for f in board.GetFootprints():
        for p in f.Pads():
            blocked.add((p.GetPosition().x, p.GetPosition().y))
    jobs, used = [], set()
    for (x, y, layer, net), lst in ends.items():
        if len(lst) != 2 or (x, y) in blocked:
            continue
        (t1, w1), (t2, w2) = lst
        if id(t1) in used or id(t2) in used:
            continue
        def far(t, w):
            q = t.GetEnd() if w == "s" else t.GetStart()
            return q.x - x, q.y - y
        (ax, ay), (bx, by) = far(t1, w1), far(t2, w2)
        la, lb = math.hypot(ax, ay), math.hypot(bx, by)
        if la < pcbnew.FromMM(0.05) or lb < pcbnew.FromMM(0.05):
            continue                  # вырожденный отрезок — угол не определён
        ang = math.degrees(math.acos(max(-1.0, min(1.0, (ax * bx + ay * by) / la / lb))))
        if ang > 91:
            continue
        c = min(pcbnew.FromMM(1.0), 0.45 * la, 0.45 * lb)
        pa = pcbnew.VECTOR2I(int(x + ax / la * c), int(y + ay / la * c))
        pb = pcbnew.VECTOR2I(int(x + bx / lb * c), int(y + by / lb * c))
        # На стыке разной ширины (Freerouting сузил у площадки) — по меньшей
        jobs.append((t1, w1, pa, t2, w2, pb, layer, net, min(t1.GetWidth(), t2.GetWidth())))
        used.update((id(t1), id(t2)))
    for t1, w1, pa, t2, w2, pb, layer, net, width in jobs:
        (t1.SetStart if w1 == "s" else t1.SetEnd)(pa)
        (t2.SetStart if w2 == "s" else t2.SetEnd)(pb)
        nt = pcbnew.PCB_TRACK(board)
        nt.SetStart(pa)
        nt.SetEnd(pb)
        nt.SetLayer(layer)
        nt.SetWidth(width)
        nt.SetNetCode(net)
        board.Add(nt)
    return len(jobs)


if __name__ == "__main__":
    if "--import" in sys.argv:
        import_ses()
    elif "--widen" in sys.argv:
        widen()
    elif "--classwiden" in sys.argv:
        class_widen(sys.argv[sys.argv.index("--classwiden") + 1])
    elif "--revert" in sys.argv:
        revert_widths(sys.argv[sys.argv.index("--revert") + 1])
    else:
        main()
