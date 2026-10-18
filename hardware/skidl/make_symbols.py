"""Символы купленных модулей для KiCad -> hardware/kicad/hybrid-radio.kicad_sym.

Стандартных символов для этих модулей в библиотеках KiCad нет. Номера
выводов — наши, по порядку площадок на плате модуля; имена — по
шелкографии модуля или, где её нет, по официальной библиотеке
JLCEDA/EasyEDA. Откуда взят каждый модуль — docs/33-carrier-board.md, §6.

Описания внутри символов — только ASCII: SKiDL читает библиотеку как
latin-1 (skidl/tools/kicad10/lib.py), русский текст в нём портится.

Все выводы модулей объявлены пассивными: модуль для схемы — это разъём,
а не микросхема, и проверку электрических типов на нём делать не с чем.

Запуск:  .venv/Scripts/python hardware/skidl/make_symbols.py
"""

import sys
from pathlib import Path

OUT = Path(__file__).resolve().parents[1] / "kicad" / "hybrid-radio.kicad_sym"

# (имя символа, обозначение, описание, левые выводы, правые выводы)
# вывод = (номер, имя)
SYMBOLS = [
    ("RRD-102", "U", "FM tuner RDA5807M, RRD-102 V2.0 module, castellated 2.0 mm pitch. "
     "Pinout: JLCEDA/EasyEDA official library, LCSC C9900205627",
     # 3 и 9 — NC: прозвонено заказчиком 2026-10-18 на своём модуле, на землю
     # не звонятся (в библиотеке JLCEDA они GND); земля только вывод 6.
     [(5, "VCC"), (1, "SDA"), (2, "SCL"), (4, "NC"), (3, "NC"), (6, "GND"), (9, "NC")],
     [(10, "ANT"), (7, "LOUT"), (8, "ROUT")]),

    ("GY-PCM5102A", "U", "DAC PCM5102A, GY-PCM5102A module (TZT). Pins 1-6: I2S row, "
     "7-15: output/config row; names 11-15 read from the back side",
     [(1, "VIN"), (2, "GND"), (3, "LCK"), (4, "DIN"), (5, "BCK"), (6, "SCK"),
      (15, "FLT"), (14, "DEMP"), (12, "FMT")],
     [(7, "L"), (9, "R"), (8, "AGND"), (10, "AGND"), (11, "A3V3"), (13, "XSMT")]),

    ("HW-327", "A", "Amplifier TPA3118, HW-327 module, mono PBTL. "
     "Which MUTE pad is ground: verify with a meter",
     [(5, "IN+"), (6, "IN-"), (7, "MUTE_SD"), (8, "MUTE_GND")],
     [(1, "PWR+"), (2, "PWR-"), (3, "OUT+"), (4, "OUT-")]),

    ("WeAct-SY8205", "U", "Synchronous buck WeAct SY8205, 7-30 V in, 5 V 5 A out",
     [(1, "IN+"), (2, "IN-"), (5, "EN")],
     [(3, "OUT+"), (4, "OUT-")]),

    ("MCP23017-CJMCU", "U", "I/O expander MCP23017, CJMCU module. 1-10: single row, "
     "11-20: column B of the double row, 21-30: column A. "
     "Which column is at the edge: verify on the module",
     [(10, "VCC"), (11, "VCC"), (9, "GND"), (21, "GND"), (7, "SDA"), (8, "SCL"),
      (3, "A0"), (2, "A1"), (1, "A2"), (4, "~{RESET}"), (5, "NC_SO"), (6, "NC_CS"),
      (22, "INTA"), (12, "INTB")],
     [(23, "GPA0"), (24, "GPA1"), (25, "GPA2"), (26, "GPA3"), (27, "GPA4"),
      (28, "GPA5"), (29, "GPA6"), (30, "GPA7"),
      (13, "GPB0"), (14, "GPB1"), (15, "GPB2"), (16, "GPB3"), (17, "GPB4"),
      (18, "GPB5"), (19, "GPB6"), (20, "GPB7")]),

    ("HT7333-A", "U", "LDO 3.3 V 250 mA, TO-92. Pins per Holtek HT73XX datasheet "
     "rev. 1.30, Pin Assignment, TO-92 front view: 1 GND, 2 VIN, 3 VOUT",
     [(2, "VIN")],
     [(3, "VOUT"), (1, "GND")]),

    # Плата Waveshare ESP32-P4-WIFI6-Touch-LCD-5 как внешнее устройство — только
    # для листа макетирования (make_carriers_sheet.py), на платы не ставится.
    # 1-40 — гребёнка J3 платы P4, имена по docs/33 §2 (сверено со схемой
    # Waveshare, шелкографией и таблицей производителя); 41-42 — USB-C, куда
    # по кабелю приходит 5 В с J2.
    ("ESP32-P4-Board", "P", "Waveshare ESP32-P4-WIFI6-Touch-LCD-5 as external device: "
     "J3 header 2x20 (pins 1-40) and USB-C power input (41-42)",
     [(1, "VCC_5V"), (3, "VCC_5V"), (5, "GND"), (7, "GPIO37"), (9, "GPIO38"), (11, "GPIO5"),
      (13, "GND"), (15, "GPIO21"), (17, "GPIO22"), (19, "GND"), (21, "GPIO24"), (23, "GPIO25"),
      (25, "USBD_N"), (27, "USBD_P"), (29, "GND"), (31, "GPIO32"), (33, "GND"), (35, "GPIO46"),
      (37, "GPIO47"), (39, "GPIO48"), (41, "USB-C_VBUS")],
     [(2, "ESP_3V3"), (4, "GPIO7/SDA"), (6, "GPIO8/SCL"), (8, "GPIO2"), (10, "GND"), (12, "GPIO3"),
      (14, "GPIO4"), (16, "GPIO28"), (18, "ESP_3V3"), (20, "GPIO29"), (22, "GPIO30"), (24, "GPIO31"),
      (26, "GND"), (28, "GPIO34"), (30, "GPIO35"), (32, "GPIO49"), (34, "GPIO50"), (36, "GPIO51"),
      (38, "GPIO52"), (40, "GND"), (42, "USB-C_GND")]),
]

PITCH = 2.54
PIN_LEN = 2.54
HALF_W = 10.16


def effects(hide=False):
    return f'(effects (font (size 1.27 1.27)){" (hide yes)" if hide else ""})'


def pin(num, name, x, y, angle):
    pin_type = "no_connect" if name == "NC" else "passive"
    return (f'\t\t\t(pin {pin_type} line (at {x:.2f} {y:.2f} {angle}) (length {PIN_LEN})\n'
            f'\t\t\t\t(name "{name}" {effects()})\n'
            f'\t\t\t\t(number "{num}" {effects()})\n\t\t\t)')


def symbol(name, ref, desc, left, right):
    rows = max(len(left), len(right))
    top = (rows - 1) * PITCH / 2
    box_top, box_bot = top + PITCH, -top - PITCH
    # Ширина корпуса — по самым длинным именам слева и справа (~1 мм на знак
    # шрифта 1,27 + отступы), кратно шагу 2,54, не меньше HALF_W: иначе имена
    # выводов P4 (USB-C_VBUS / USB-C_GND) наезжают друг на друга.
    longest = max((len(x[1]) for x in left), default=0) + max((len(x[1]) for x in right), default=0)
    half = max(HALF_W, -(-(longest * 1.3 + 4.0) / 2 // PITCH) * PITCH)
    pins = [pin(n, s, -half - PIN_LEN, top - i * PITCH, 0) for i, (n, s) in enumerate(left)]
    pins += [pin(n, s, half + PIN_LEN, top - i * PITCH, 180) for i, (n, s) in enumerate(right)]
    return f'''\t(symbol "{name}"
\t\t(pin_names (offset 1.016))
\t\t(exclude_from_sim no) (in_bom yes) (on_board yes)
\t\t(property "Reference" "{ref}" (at 0 {box_top + 1.27:.2f} 0) {effects()})
\t\t(property "Value" "{name}" (at 0 {box_bot - 1.27:.2f} 0) {effects()})
\t\t(property "Footprint" "hybrid-radio:{name}" (at 0 0 0) {effects(True)})
\t\t(property "Datasheet" "" (at 0 0 0) {effects(True)})
\t\t(property "Description" "{desc}" (at 0 0 0) {effects(True)})
\t\t(symbol "{name}_0_1"
\t\t\t(rectangle (start {-half} {box_top:.2f}) (end {half} {box_bot:.2f})
\t\t\t\t(stroke (width 0.254) (type default)) (fill (type background)))
\t\t)
\t\t(symbol "{name}_1_1"
{chr(10).join(pins)}
\t\t)
\t)'''


def main():
    sys.stdout.reconfigure(encoding="utf-8")  # консоль Windows по умолчанию cp1252
    body = "\n".join(symbol(*s) for s in SYMBOLS)
    OUT.write_text("(kicad_symbol_lib\n\t(version 20251024)\n\t(generator \"make_symbols.py\")\n"
                   f"{body}\n)\n", encoding="utf-8")
    print(f"{OUT}: {len(SYMBOLS)} символов")


if __name__ == "__main__":
    main()
