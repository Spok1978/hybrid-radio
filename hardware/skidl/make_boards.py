"""Проекты KiCad трёх плат -> hardware/kicad/{power,signal,amp}/.

    .venv/Scripts/python hardware/skidl/make_boards.py

Для каждой платы: <плата>.kicad_pro (классы цепей и правила), таблицы
библиотек со ссылкой на общие hybrid-radio.*, пустой <плата>.kicad_pcb, если
его ещё нет (существующий не трогается). Схемы плат делает make_schematic.py,
контур — make_board.py, расстановку — place_parts.py (питоном KiCad).

Правила — под самостоятельное изготовление односторонней платы
(решение заказчика 2026-10-08): медь только на B.Cu, детали выводные со
стороны F; дорожка ≥ 0,6 мм, зазор ≥ 0,4 мм; переходных отверстий нет —
где не разойтись, проволочная перемычка. Эти минимумы — распространённая
любительская практика для ЛУТ/фоторезиста, а не норма стандарта; под свой
способ травления их можно сузить.
"""

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from boards import BOARDS  # noqa: E402

KICAD = Path(__file__).resolve().parents[1] / "kicad"
TEMPLATE = KICAD / "Hybryd Radio.kicad_pro"

# Ширина силовых — по IPC-2221 с запасом (docs/33, §14); остальные — по
# возможностям домашнего травления.
CLASSES = {"Default": 0.6, "POWER_12V": 2.5, "POWER_5V": 1.5, "SPEAKER": 1.5, "GND": 1.5}
PATTERNS = [("POWER_12V", p) for p in ("/+12V", "/+12V_IN", "/12V_FUSED")] + \
           [("POWER_5V", "/+5V"), ("SPEAKER", "/SPK_*"), ("GND", "/GND")]
CLEARANCE = 0.4      # 0,5 нарушают сами стандартные посадочные места (C P2.00, USB-A)


def project(name):
    d = json.loads(TEMPLATE.read_text(encoding="utf-8"))
    d.setdefault("meta", {})["filename"] = f"{name}.kicad_pro"
    ns = d["net_settings"]
    base = ns["classes"][0]
    classes = []
    for i, (cname, width) in enumerate(CLASSES.items()):
        c = dict(base)
        c.update(name=cname, track_width=width, clearance=CLEARANCE,
                 via_diameter=2.0, via_drill=0.8,     # «виа» = отверстие под перемычку
                 priority=2147483647 if cname == "Default" else i)
        classes.append(c)
    ns["classes"] = classes
    ns["netclass_patterns"] = [{"netclass": c, "pattern": p} for c, p in PATTERNS]
    r = d["board"]["design_settings"]["rules"]
    # Кольцо 0,25: у стандартных гнёзд 2,54 и разъёмов площадка 1,7 при
    # сверле 1,0 — кольцо 0,35; меньше 0,25 травить дома рискованно.
    # Дорожка 0,4: Freerouting сужает дорожку у площадок (до 0,45 у класса 0,6).
    r.update(min_clearance=0.35, min_track_width=0.4, min_via_diameter=1.6,
             min_through_hole_diameter=0.6, min_via_annular_width=0.25,
             min_copper_edge_clearance=1.0, min_hole_clearance=0.5, min_hole_to_hole=0.5)
    return json.dumps(d, indent=2, ensure_ascii=False)


def lib_tables(folder):
    (folder / "sym-lib-table").write_text(
        '(sym_lib_table\n\t(version 7)\n\t(lib (name "hybrid-radio") (type "KiCad") '
        '(uri "${KIPRJMOD}/../hybrid-radio.kicad_sym") (options "") (descr ""))\n)\n', encoding="utf-8")
    (folder / "fp-lib-table").write_text(
        '(fp_lib_table\n\t(version 7)\n\t(lib (name "hybrid-radio") (type "KiCad") '
        '(uri "${KIPRJMOD}/../hybrid-radio.pretty") (options "") (descr ""))\n)\n', encoding="utf-8")


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    for name in BOARDS:
        folder = KICAD / name
        folder.mkdir(exist_ok=True)
        (folder / f"{name}.kicad_pro").write_text(project(name), encoding="utf-8")
        lib_tables(folder)
        pcb = folder / f"{name}.kicad_pcb"
        if not pcb.exists():
            pcb.write_text('(kicad_pcb (version 20260206) (generator "pcbnew") (generator_version "10.0")\n)\n',
                           encoding="utf-8")
        print(f"{folder}: проект, таблицы библиотек{', пустая плата' if pcb.stat().st_size < 200 else ''}")


if __name__ == "__main__":
    main()
