"""Сверка нетлиста платы-носителя с прошивкой и с распиновкой платы P4.

Три независимых источника должны сойтись:
  1. firmware/main/board_pins.h — какой GPIO прошивка ждёт для сигнала;
  2. таблица 40PIN Waveshare (docs/33, §2) — на каком контакте J3 этот GPIO;
  3. hardware/kicad/Hybryd Radio.net — какая цепь нашей платы на этом контакте.

Плюс проверки, которые ERC не делает: кто на каких шинах питания,
и что запрещённые контакты J3 свободны.

Запуск: .venv/Scripts/python hardware/skidl/check_carrier.py
Выход 0 — всё сошлось, 1 — есть расхождения.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NETLIST = ROOT / "hardware/kicad/Hybryd Radio.net"
BOARD_PINS = ROOT / "firmware/main/board_pins.h"

# Контакт J3 -> сигнал платы P4. docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-5,
# таблица 40PIN; совпадает со схемой Waveshare и шелкографией (docs/33, §2).
WAVESHARE_J3 = {
    1: "5V", 2: "3V3", 3: "5V", 4: "GPIO7", 5: "GND", 6: "GPIO8", 7: "GPIO37",
    8: "GPIO2", 9: "GPIO38", 10: "GND", 11: "GPIO5", 12: "GPIO3", 13: "GND",
    14: "GPIO4", 15: "GPIO21", 16: "GPIO28", 17: "GPIO22", 18: "3V3", 19: "GND",
    20: "GPIO29", 21: "GPIO24", 22: "GPIO30", 23: "GPIO25", 24: "GPIO31",
    25: "USBD_N", 26: "GND", 27: "USBD_P", 28: "GPIO34", 29: "GND", 30: "GPIO35",
    31: "GPIO32", 32: "GPIO49", 33: "GND", 34: "GPIO50", 35: "GPIO46",
    36: "GPIO51", 37: "GPIO47", 38: "GPIO52", 39: "GPIO48", 40: "GND",
}

# Макрос прошивки -> цепь нашей платы, которая должна на нём оказаться.
FIRMWARE_TO_NET = {
    "PIN_I2C_SDA": "I2C_SDA", "PIN_I2C_SCL": "I2C_SCL",
    "PIN_I2S_BCK": "I2S_BCK", "PIN_I2S_LRCK": "I2S_LRCK", "PIN_I2S_DIN": "I2S_DIN",
    "PIN_AMP_SD": "AMP_SD_GPIO49", "PIN_POWER_BUTTON": "BTN_PWR",
    "PIN_ENC1_A": "ENC1_A", "PIN_ENC1_B": "ENC1_B", "PIN_ENC1_BTN": "ENC1_SW",
    "PIN_ENC2_A": "ENC2_A", "PIN_ENC2_B": "ENC2_B", "PIN_ENC2_BTN": "ENC2_SW",
}
# В прошивке этих макросов ещё нет — назначения из docs/30 (вариант 1) и docs/33.
DOC_ONLY = {"GPIO32": "EXP_INT", "GPIO50": "PWR_SENSE"}

MUST_BE_FREE = {1, 3, 7, 8, 9, 21, 23, 25, 27, 28, 30}

# Цепь питания -> (обозначение, вывод), которые обязаны на ней быть.
POWER = {
    "+3V3_FM": [("U3", "5")],                         # тюнер, VCC
    "+3V3_ESP": [("U5", "10"), ("U5", "11"), ("U5", "4")],  # MCP23017 VCC x2, RESET
    "+5V": [("U4", "1"), ("J2", "1")],                # ЦАП VIN, USB VBUS
    "+12V": [("A1", "1"), ("A2", "1"), ("U1", "1")],  # усилители, DC-DC
}


def parse_sexp(text):
    tokens = re.findall(r'\(|\)|"(?:[^"\\]|\\.)*"|[^\s()]+', text)
    stack, cur = [], []
    for t in tokens:
        if t == "(":
            stack.append(cur)
            cur = []
        elif t == ")":
            done, cur = cur, stack.pop()
            cur.append(done)
        else:
            cur.append(t.strip('"'))
    return cur[0]


def field(node, key):
    for item in node:
        if isinstance(item, list) and item and item[0] == key:
            return item[1] if len(item) > 1 else None
    return None


def parse_nets(text):
    root = parse_sexp(text)
    nets = {}
    for section in root:
        if isinstance(section, list) and section and section[0] == "nets":
            for net in section[1:]:
                nodes = {(field(n, "ref"), field(n, "pin")) for n in net
                         if isinstance(n, list) and n and n[0] == "node"}
                nets[field(net, "name")] = nodes
    return nets


def main():
    sys.stdout.reconfigure(encoding="utf-8")  # консоль Windows по умолчанию cp1252
    nets = parse_nets(NETLIST.read_text(encoding="utf-8"))
    if not nets:
        print("не разобрал нетлист:", NETLIST)
        return 1
    pin_net = {node: name for name, nodes in nets.items() for node in nodes}
    errors = []

    def j3(p):
        return pin_net.get(("J3", str(p)))

    gpio_to_j3 = {sig: p for p, sig in WAVESHARE_J3.items() if sig.startswith("GPIO")}

    defines = dict(re.findall(r"#define\s+(PIN_\w+)\s+(\d+)", BOARD_PINS.read_text(encoding="utf-8")))
    for macro, net in FIRMWARE_TO_NET.items():
        gpio = f"GPIO{defines[macro]}"
        p = gpio_to_j3.get(gpio)
        got = j3(p) if p else None
        ok = got == net
        print(f"{'OK ' if ok else 'ERR'} {macro:18} = {gpio:7} -> J3.{p} -> {got}")
        if not ok:
            errors.append(f"{macro}: ждали {net}, на J3.{p} {got}")
    for gpio, net in DOC_ONLY.items():
        p = gpio_to_j3[gpio]
        ok = j3(p) == net
        print(f"{'OK ' if ok else 'ERR'} {'(docs) ' + net:18} = {gpio:7} -> J3.{p} -> {j3(p)}")
        if not ok:
            errors.append(f"{gpio}: ждали {net}, на J3.{p} {j3(p)}")

    for p, sig in WAVESHARE_J3.items():
        got = j3(p)
        if sig == "GND" and got != "GND":
            errors.append(f"J3.{p} по Waveshare GND, у нас {got}")
        if sig == "3V3" and got != "+3V3_ESP":
            errors.append(f"J3.{p} по Waveshare 3V3, у нас {got}")
        if p in MUST_BE_FREE and got is not None:
            errors.append(f"J3.{p} ({sig}) должен быть свободен, а на нём {got}")

    for net, nodes in POWER.items():
        for node in nodes:
            if node not in nets.get(net, set()):
                errors.append(f"{node[0]}.{node[1]} не на {net} (на {pin_net.get(node)})")
    # GPA7 (U5.30) и GPB7 (U5.20) у MCP23017 — только выходы (Microchip
    # DS20001952E); к кнопкам их подключать нельзя.
    for pin in ("30", "20"):
        if pin_net.get(("U5", pin)) not in (None, "__NOCONNECT"):
            errors.append(f"U5.{pin} (GPA7/GPB7, только выход) подключён к {pin_net[('U5', pin)]}")
    # Тюнер не должен сидеть на 5 В: даташит RDA5807M — 2,7-3,3 В (docs/02).
    if ("U3", "5") in nets.get("+5V", set()):
        errors.append("тюнер на +5V")

    print()
    print(f"цепей: {len(nets)}, узлов: {len(pin_net)}")
    if errors:
        print("РАСХОЖДЕНИЯ:")
        for e in errors:
            print("  -", e)
        return 1
    print("всё сошлось")
    return 0


if __name__ == "__main__":
    sys.exit(main())
