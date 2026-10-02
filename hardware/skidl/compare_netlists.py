"""Сравнение соединений: нетлист SKiDL против нетлиста, выгруженного KiCad
из сгенерированной схемы. Имена цепей не сравниваются — только то, какие
выводы соединены между собой.

    kicad-cli sch export netlist --format kicadxml -o sch.xml "Hybryd Radio.kicad_sch"
    .venv/Scripts/python hardware/skidl/compare_netlists.py sch.xml
"""

import sys
import xml.etree.ElementTree as ET
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from check_carrier import NETLIST, parse_nets  # noqa: E402


def groups_from_kicad_xml(path):
    root = ET.parse(path).getroot()
    out = {}
    for net in root.iter("net"):
        nodes = frozenset((n.get("ref"), n.get("pin")) for n in net.iter("node"))
        if len(nodes) > 1 or not net.get("name", "").startswith("unconnected"):
            out[net.get("name")] = nodes
    return out


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    # Флаги питания (#FLG) KiCad в нетлист не выгружает — их не сравниваем.
    want = {name: frozenset(x for x in n if not x[0].startswith("#"))
            for name, n in parse_nets(NETLIST.read_text(encoding="utf-8")).items()
            if name != "__NOCONNECT"}
    got = groups_from_kicad_xml(sys.argv[1])

    want_by_node = {node: name for name, nodes in want.items() for node in nodes}
    got_by_node = {node: name for name, nodes in got.items() for node in nodes}

    problems = 0
    for name, nodes in sorted(want.items()):
        got_names = {got_by_node.get(n) for n in nodes}
        if len(got_names) != 1 or None in got_names:
            problems += 1
            print(f"РАЗОРВАНА {name}: в схеме разбита на {sorted(map(str, got_names))}")
            continue
        g = got[got_names.pop()]
        extra = g - nodes
        if extra:
            problems += 1
            merged = sorted({want_by_node.get(n, "?") for n in extra})
            print(f"СЛИТА {name} с {merged}: лишние выводы {sorted(extra)}")
    print(f"\nцепей в SKiDL: {len(want)}, в схеме: {len(got)}, расхождений: {problems}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
