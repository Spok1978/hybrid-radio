"""Проверка списка цепей: разбор YAML, поиск одиночных цепей и опечаток.

Запуск:  python netlist/check_netlist.py
"""

import collections
import pathlib
import sys

import yaml

HERE = pathlib.Path(__file__).parent
DATA = yaml.safe_load((HERE / "ip-radio.yaml").read_text(encoding="utf-8"))

nets = DATA["nets"]
components = DATA["components"]

points = [(net, p) for net, pins in nets.items() for p in pins]
print(f"компонентов:        {len(components)}")
print(f"цепей:              {len(nets)}")
print(f"точек подключения:  {len(points)}")

problems = []

# Цепь из одной точки — почти всегда забытое соединение.
for net, pins in nets.items():
    if len(pins) < 2:
        problems.append(f"цепь {net}: всего {len(pins)} точка")

# Один и тот же вывод в двух цепях — короткое замыкание в описании.
seen = collections.defaultdict(list)
for net, p in points:
    seen[p].append(net)
for pin, in_nets in seen.items():
    if len(in_nets) > 1:
        problems.append(f"вывод {pin} встречается в цепях: {', '.join(in_nets)}")

# Ссылка на компонент, которого нет в списке.
for net, p in points:
    ref = p.split(".")[0]
    if ref not in components:
        problems.append(f"цепь {net}: компонент {ref} не описан")

if problems:
    print("\nНайдено:")
    for line in problems:
        print("  -", line)
    sys.exit(1)

print("\nОдиночных цепей, дублей выводов и неизвестных компонентов нет.")
