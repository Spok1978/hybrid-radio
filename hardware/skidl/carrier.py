"""Плата-носитель модулей IP-Radio — принципиальная схема на SKiDL.

Этот файл — источник истины для схемы платы. Схема KiCad, нетлист и
проверки генерируются из него; правится он, а не .kicad_sch.

Решения и обоснования — docs/33-carrier-board.md. Номера контактов
гребёнки J3 платы P4 сверены по четырём источникам (docs/33, §2).

Запуск (из корня репозитория):
    .venv/Scripts/python hardware/skidl/make_symbols.py
    .venv/Scripts/python hardware/skidl/carrier.py
"""

import builtins
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
os.chdir(HERE)   # служебные .erc/.log/_sklib.py SKiDL пишет в текущий каталог
KICAD_DIR = HERE.parent / "kicad"
KICAD_SHARE = Path(os.environ["LOCALAPPDATA"]) / "Programs/KiCad/10.0/share/kicad"
os.environ.setdefault("KICAD10_SYMBOL_DIR", str(KICAD_SHARE / "symbols"))
os.environ.setdefault("KICAD10_FOOTPRINT_DIR", str(KICAD_SHARE / "footprints"))

import skidl  # noqa: E402
from skidl import (KICAD10, ERC, Net, Part, generate_netlist, generate_schematic,  # noqa: E402
                   lib_search_paths, set_default_tool, subcircuit)

set_default_tool(KICAD10)
_default_fp_handler = skidl.empty_footprint_handler


def _fp_handler(part):
    # У флагов питания (#FLG) посадочного места нет и быть не должно.
    if not part.ref.startswith("#"):
        _default_fp_handler(part)


skidl.empty_footprint_handler = _fp_handler
NC = builtins.NC   # SKiDL кладёт сеть «не подключено» в builtins
lib_search_paths[KICAD10].append(str(KICAD_DIR))

# ---------------------------------------------------------------- посадочные места
FP_R = "Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P10.16mm_Horizontal"
FP_C = "Capacitor_THT:C_Disc_D5.0mm_W2.5mm_P5.00mm"
FP_CP_10U = "Capacitor_THT:CP_Radial_D5.0mm_P2.00mm"
FP_CP_470U_16 = "Capacitor_THT:CP_Radial_D8.0mm_P3.50mm"
FP_CP_470U_25 = "Capacitor_THT:CP_Radial_D10.0mm_P5.00mm"
FP_CP_1000U = "Capacitor_THT:CP_Radial_D12.5mm_P5.00mm"
# Плёночный 3,3 мкФ 63 В куплен, размер не измерен — заменить по замеру.
FP_C_FILM = "Capacitor_THT:C_Rect_L18.0mm_W8.0mm_P15.00mm_FKS3_FKP3"
FP_BEAD = "Inductor_THT:L_Axial_L5.3mm_D2.2mm_P10.16mm_Horizontal_Vishay_IM-1"
FP_CHOKE = "Inductor_THT:L_Radial_D10.0mm_P5.00mm_Fastron_07P"
FP_TERM2 = "TerminalBlock_Phoenix:TerminalBlock_Phoenix_MKDS-1,5-2-5.08_1x02_P5.08mm_Horizontal"
FP_TRIM = "Potentiometer_THT:Potentiometer_Bourns_3296W_Vertical"


def xh(n):
    return f"Connector_JST:JST_XH_B{n}B-XH-A_1x{n:02d}_P2.50mm_Vertical"


def solder(n):
    """Отверстия под пайку провода 0,25 мм² (AWG 24), шаг 4,2 мм — для
    органов на корпусе: провода паяются в плату (решение 2026-10-07).
    В библиотеке KiCad эта серия есть на 1-6 контактов."""
    return f"Connector_Wire:SolderWire-0.25sqmm_1x{n:02d}_P4.2mm_D0.65mm_OD1.7mm"


def R(value, a, b):
    r = Part("Device", "R", value=value, footprint=FP_R)
    r[1] += a
    r[2] += b
    return r


def C(value, a, b, footprint=FP_C):
    c = Part("Device", "C", value=value, footprint=footprint)
    c[1] += a
    c[2] += b
    return c


def CP(value, plus, minus, footprint):
    c = Part("Device", "C_Polarized", value=value, footprint=footprint)
    c[1] += plus
    c[2] += minus
    return c


def module(name):
    return Part("hybrid-radio", name, footprint=f"hybrid-radio:{name}")


# ---------------------------------------------------------------- цепи
gnd = Net("GND")
v12 = Net("+12V")
v5 = Net("+5V")
v3_esp = Net("+3V3_ESP")      # шина ESP_3V3 платы P4, приходит по шлейфу
v3_fm = Net("+3V3_FM")        # своё питание тюнера

i2c_sda, i2c_scl = Net("I2C_SDA"), Net("I2C_SCL")
i2s_bck, i2s_lrck, i2s_din = Net("I2S_BCK"), Net("I2S_LRCK"), Net("I2S_DIN")
amp_sd_gpio = Net("AMP_SD_GPIO49")
pwr_sense = Net("PWR_SENSE")
exp_int = Net("EXP_INT")
pwr_btn = Net("BTN_PWR")
enc1_a, enc1_b, enc1_sw = Net("ENC1_A"), Net("ENC1_B"), Net("ENC1_SW")
enc2_a, enc2_b, enc2_sw = Net("ENC2_A"), Net("ENC2_B"), Net("ENC2_SW")
spare = {g: Net(f"SPARE_GPIO{g}") for g in (4, 5, 51, 52)}

tuner_l, tuner_r = Net("TUNER_L"), Net("TUNER_R")
dac_l, dac_r = Net("DAC_L"), Net("DAC_R")
amp_sd = Net("AMP_SD")


# ---------------------------------------------------------------- блоки
@subcircuit
def power_input():
    """Вход 12 В: клеммник, предохранитель 3 А, защита от переполюсовки,
    TVS, накопительные конденсаторы, делитель признака питания.
    Номиналы — docs/09 и docs/16 (сборка на Pi, три внешних разбора)."""
    v12_in, v12_fused, gate = Net("+12V_IN"), Net("12V_FUSED"), Net("Q1_GATE")

    j = Part("Connector", "Screw_Terminal_01x02", ref="J1", value="12V IN", footprint=FP_TERM2)
    j[1] += v12_in
    j[2] += gnd

    # T5A под БП 12 В / 5 А: два канала TPA3118 на 4 Ом при 12 В — до
    # 2 x 18 Вт (U^2/2R), это ~3,3 А плюс плата P4 (решение 2026-10-02).
    f = Part("Device", "Fuse", ref="F1", value="T5A",
             footprint="Fuse:Fuseholder_Clip-5x20mm_Bel_FC-203-22_Lateral_P17.80x5.00mm_D1.17mm_Horizontal")
    f[1] += v12_in
    f[2] += v12_fused

    # P-канальный MOSFET в плюсовом проводе, сток к входу. При верной
    # полярности сначала проводит его собственный диод, затем затвор
    # уходит на землю через 100 кОм и канал открывается. При обратной
    # полярности затвор выше истока — транзистор закрыт.
    q = Part("Transistor_FET", "IRF4905", ref="Q1", footprint="Package_TO_SOT_THT:TO-220-3_Vertical")
    q["D"] += v12_fused
    q["S"] += v12
    q["G"] += gate
    R("100k", gate, gnd)
    dz = Part("Device", "D_Zener", value="1N4744A 15V", footprint="Diode_THT:D_DO-41_SOD81_P10.16mm_Horizontal")
    dz["K"] += v12      # ограничивает Uзи: катод на истоке, анод на затворе
    dz["A"] += gate

    tvs = Part("Device", "D_Zener", value="P6KE16A TVS", footprint="Diode_THT:D_DO-15_P12.70mm_Horizontal")
    tvs["K"] += v12
    tvs["A"] += gnd

    CP("470u 25V", v12, gnd, FP_CP_470U_25)
    C("100n", v12, gnd)

    # Признак внешнего питания на GPIO50: 12 В -> 2,82 В (docs/33, §5).
    R("39k", v12, pwr_sense)
    R("12k", pwr_sense, gnd)
    C("100n", pwr_sense, gnd)


@subcircuit
def supply_5v():
    """5 В от синхронного DC-DC WeAct SY8205; отдаются в USB-A, оттуда
    кабелем в USB-C платы P4 — так заряжается её батарея (docs/33, §3)."""
    u = module("WeAct-SY8205")
    u["IN+"] += v12
    u["IN-"] += gnd
    u["OUT+"] += v5
    u["OUT-"] += gnd
    u["EN"] += NC   # поведение по умолчанию — включён (сверить на модуле)

    CP("470u 16V", v5, gnd, FP_CP_470U_16)
    C("100n", v5, gnd)

    usb = Part("Connector", "USB_A", ref="J2", value="5V to P4 USB-C",
               footprint="Connector_USB:USB_A_Molex_67643_Horizontal")
    usb["VBUS"] += v5
    usb["GND"] += gnd
    usb["Shield"] += gnd
    usb["D-", "D+"] += NC

    # Питание приходит с выводов модуля, а они пассивные; флаги говорят
    # проверке ERC, что +5V и GND действительно чем-то питаются.
    for net in (v5, gnd):
        Part("power", "PWR_FLAG")[1] += net


@subcircuit
def supply_3v3_tuner():
    """Отдельные 3,3 В тюнеру: дроссель, бусины и LDO HT7333-A,
    как в сборке на Pi (docs/16)."""
    v5_l, ldo_in, ldo_out = Net("+5V_FILT"), Net("LDO_IN"), Net("LDO_OUT")
    l1 = Part("Device", "L", value="10uH 3A", footprint=FP_CHOKE)
    l1[1] += v5
    l1[2] += v5_l
    fb_in = Part("Device", "FerriteBead", value="bead", footprint=FP_BEAD)
    fb_in[1] += v5_l
    fb_in[2] += ldo_in
    CP("10u", ldo_in, gnd, FP_CP_10U)
    C("100n", ldo_in, gnd)

    ldo = module("HT7333-A")
    ldo["VIN"] += ldo_in
    ldo["VOUT"] += ldo_out
    ldo["GND"] += gnd
    ldo.footprint = "Package_TO_SOT_THT:TO-92_Inline"

    CP("10u", ldo_out, gnd, FP_CP_10U)
    C("100n", ldo_out, gnd)
    fb_out = Part("Device", "FerriteBead", value="bead", footprint=FP_BEAD)
    fb_out[1] += ldo_out
    fb_out[2] += v3_fm


@subcircuit
def tuner():
    """FM-тюнер RRD-102 и вход антенны.

    Выходы LOUT/ROUT у RDA5807M сидят на постоянном напряжении 1,0-1,1 В
    (даташит RDA5807M V1.1, с. 7, «Audio Output Common Mode Voltage»), и в
    типовой схеме производителя (рис. 8-1) стоят разделительные C3/C4
    4,7 мкФ. На модуле RRD-102 их по фото нет — ставим свои. Плюс
    электролита — к тюнеру: с той стороны +1,05 В, с другой — 0 В через
    резисторы микшера. Без них постоянка тюнера уходила в узел микшера
    и через 10 кОм в выход ЦАП (ошибка найдена 2026-10-05).

    На антенне — последовательный 100 пФ, как C7 в той же типовой схеме.
    Если такой же уже стоит на модуле, два последовательно дадут 50 пФ —
    на 100 МГц это 32 Ом, не помеха."""
    ant, ant_in = Net("ANT"), Net("ANT_IN")
    u = module("RRD-102")
    u["VCC"] += v3_fm
    u["GND"] += gnd
    u["SDA"] += i2c_sda
    u["SCL"] += i2c_scl
    u["ANT"] += ant
    for pin, out in (("LOUT", tuner_l), ("ROUT", tuner_r)):
        raw = Net(f"RDA_{pin}")
        raw += u[pin]
        CP("4u7 25V", raw, out, FP_CP_10U)

    C("100p C0G", ant_in, ant)
    j = Part("Connector", "Screw_Terminal_01x02", ref="J4", value="ANT", footprint=FP_TERM2)
    j[1] += ant_in
    j[2] += gnd


@subcircuit
def dac():
    """ЦАП GY-PCM5102A. Настройка выводами, как в docs/16: SCK, FLT, DEMP,
    FMT — на землю, XSMT — на A3V3 модуля. Перемычки H1-H4 на обороте
    модуля должны быть разомкнуты или стоять так же."""
    u = module("GY-PCM5102A")
    u["VIN"] += v5
    u["GND"] += gnd
    u["AGND"] += gnd
    for name in ("SCK", "FLT", "DEMP", "FMT"):
        u[name] += gnd
    Net("DAC_A3V3").connect(u["XSMT"], u["A3V3"])
    u["L"] += dac_l
    u["R"] += dac_r

    # 47 Ом в I2S: по умолчанию впаять перемычки, резисторы — если на
    # осциллографе виден звон (docs/33, ответ от 2026-10-01).
    for net, pin in ((i2s_bck, "BCK"), (i2s_lrck, "LCK"), (i2s_din, "DIN")):
        R("47", net, Net(f"DAC_{pin}").connect(u[pin]))
    C("100n", v5, gnd)


@subcircuit
def mixer_channel(tuner_out, dac_out, amp_inp, amp_inn, ch):
    """Пассивный сумматор одного канала (docs/04): тюнер через 1 кОм,
    ЦАП через 10 кОм, узел 100 кОм на землю, подстроечник 10 кОм,
    плёночный 3,3 мкФ (при 26 дБ вход TPA3118 30 кОм, docs/17)."""
    s, mix = Net(f"SUM_{ch}"), Net(f"MIX_{ch}")
    R("1k", tuner_out, s)
    R("10k", dac_out, s)
    R("100k", s, gnd)
    rv = Part("Device", "R_Potentiometer_Trim", value="10k 3296W", footprint=FP_TRIM)
    rv[1] += s
    rv[3] += gnd
    rv[2] += mix
    C("3u3 film 63V", mix, amp_inp, FP_C_FILM)
    # Неиспользуемый вход дифференциальной пары — на землю через такой же
    # конденсатор: входы TPA3118 смещены на 3 В (даташит TI).
    C("3u3 film 63V", amp_inn, gnd, FP_C_FILM)


@subcircuit
def amp_channel(ch, inp, inn, spk_ref):
    """Модуль HW-327 на один канал и клеммник динамика."""
    u = module("HW-327")
    u["PWR+"] += v12
    u["PWR-"] += gnd
    u["IN+"] += inp
    u["IN-"] += inn
    u["MUTE_SD"] += amp_sd
    u["MUTE_GND"] += gnd
    CP("1000u 25V", v12, gnd, FP_CP_1000U)
    C("100n", v12, gnd)

    spk = Part("Connector", "Screw_Terminal_01x02", ref=spk_ref, value=f"SPK {ch}", footprint=FP_TERM2)
    Net(f"SPK_{ch}+").connect(u["OUT+"], spk[1])
    Net(f"SPK_{ch}-").connect(u["OUT-"], spk[2])


@subcircuit
def amp_mute():
    """Приглушение обоих усилителей с GPIO49.

    У TPA3118 два вывода с обратной логикой (даташит TI SLOS708G, с. 4):
    2 SDZ — LOW выключает выходы; 12 MUTE — HIGH выключает выходы. На
    какой из них идёт площадка MUTE модуля HW-327 — НЕ ИЗВЕСТНО, прозвонить.
    Поэтому места под оба резистора, впаивается один:
      - SDZ: R «вниз» 100 кОм на GND, «вверх» не ставить;
      - MUTE: R «вверх» 100 кОм на +3V3_ESP, «вниз» не ставить,
        и в прошивке инвертировать PIN_AMP_SD.
    В обоих случаях до старта прошивки усилитель молчит."""
    R("1k", amp_sd_gpio, amp_sd)
    R("100k (SDZ)", amp_sd, gnd)
    R("100k (MUTE)", amp_sd, v3_esp)


@subcircuit
def expander():
    """MCP23017, вариант 1 из docs/30: GPA0-6 — пресеты 1-7, GPB0 — MODE,
    GPB1 — пресет 8. Питание от ESP_3V3 — включается и гаснет вместе с P4.

    GPA7 и GPB7 не трогаем: у MCP23017 они только выходы, при работе
    входом может искажаться SDA (даташит Microchip DS20001952E, с. 1 и
    таблица выводов на с. 11)."""
    u = module("MCP23017-CJMCU")
    u["VCC"] += v3_esp
    u["GND"] += gnd
    u["SDA"] += i2c_sda
    u["SCL"] += i2c_scl
    for a in ("A0", "A1", "A2"):
        u[a] += gnd                # адрес 0x20
    u["~{RESET}"] += v3_esp
    u["INTA"] += exp_int
    u["INTB", "NC/SO", "NC/CS", "GPA7"] += NC
    for i in range(2, 8):
        u[f"GPB{i}"] += NC
    C("100n", v3_esp, gnd)

    # Кнопки на корпусе, провода паяются в плату. Серия площадок под пайку
    # в KiCad не длиннее 6 контактов, поэтому две группы, в каждой своя GND:
    # J7 — пресеты 1-4 + GND, J12 — пресеты 5-8, MODE + GND.
    lines = [f"GPA{i}" for i in range(7)] + ["GPB1"]
    nets = [Net(f"BTN_P{i + 1}").connect(u[line]) for i, line in enumerate(lines)]
    nets.append(Net("BTN_MODE").connect(u["GPB0"]))
    for ref, value, group in (("J7", "PRESETS 1-4", nets[:4]), ("J12", "PRESETS 5-8 + MODE", nets[4:])):
        j = Part("Connector_Generic", f"Conn_01x{len(group) + 1:02d}", ref=ref, value=value,
                 footprint=solder(len(group) + 1))
        for i, net in enumerate(group):
            j[i + 1] += net
        j[len(group) + 1] += gnd


@subcircuit
def controls():
    """Энкодеры, кнопка питания, резервные выводы. Внешние 10 кОм + 10 нФ
    на линиях A/B энкодеров: провода к панели длинные, внутренней
    подтяжки ESP32 (~45 кОм) мало."""
    for ref, a, b, sw in (("J9", enc1_a, enc1_b, enc1_sw), ("J10", enc2_a, enc2_b, enc2_sw)):
        j = Part("Connector_Generic", "Conn_01x05", ref=ref, value="EC11", footprint=solder(5))
        j[1] += a
        j[2] += gnd        # C
        j[3] += b
        j[4] += sw
        j[5] += gnd        # вторая ножка кнопки
        for line in (a, b):
            R("10k", line, v3_esp)
            C("10n", line, gnd)

    j = Part("Connector_Generic", "Conn_01x02", ref="J11", value="BTN PWR", footprint=solder(2))
    j[1] += pwr_btn
    j[2] += gnd

    j = Part("Connector_Generic", "Conn_01x06", ref="J8", value="SPARE", footprint=xh(6))
    for i, g in enumerate((4, 5, 51, 52)):
        j[i + 1] += spare[g]
    j[5] += v3_esp
    j[6] += gnd


@subcircuit
def ribbon():
    """Вилка 2x20 под шлейф к гребёнке J3 платы P4 (docs/33, §2)."""
    j = Part("Connector_Generic", "Conn_02x20_Odd_Even", ref="J3", value="to P4 J3",
             footprint="Connector_IDC:IDC-Header_2x20_P2.54mm_Vertical")
    pins = {
        2: v3_esp, 18: v3_esp,
        4: i2c_sda, 6: i2c_scl,
        12: pwr_btn,
        15: enc1_a, 17: enc1_b, 16: enc1_sw,
        20: enc2_a, 22: enc2_b, 24: enc2_sw,
        31: exp_int, 32: amp_sd_gpio, 34: pwr_sense,
        35: i2s_bck, 37: i2s_lrck, 39: i2s_din,
        14: spare[4], 11: spare[5], 36: spare[51], 38: spare[52],
    }
    for p in (5, 10, 13, 19, 26, 29, 33, 40):
        pins[p] = gnd
    for p, net in pins.items():
        j[p] += net
    for p in (1, 3, 7, 8, 9, 21, 23, 25, 27, 28, 30):
        j[p] += NC
    # Не подключать: 1, 3 (VCC_5V — питание идёт через USB-C), 7, 9 (UART
    # платы), 8 (GPIO2), 21, 23, 25, 27 (USB), 28 (GPIO34, strapping),
    # 30 (GPIO35, автосброс).


@subcircuit
def indication():
    """Светодиоды наличия +12 В и +5 В и контрольные точки (2026-10-02).
    Ток: (12 - 2) / 2,2k = 4,5 мА; (5 - 2) / 1k = 3 мА."""
    for rail, r in ((v12, "2k2"), (v5, "1k")):
        a = Net(f"LED_{rail.name.strip('+')}")
        R(r, rail, a)
        led = Part("Device", "LED", value=f"{rail.name} green", footprint="LED_THT:LED_D3.0mm")
        led["A"] += a
        led["K"] += gnd

    tp_nets = [v12, v5, v3_fm, pwr_sense, Net.get("SUM_L"), Net.get("SUM_R"), gnd, gnd]
    assert all(tp_nets), "контрольная точка без цепи: indication() вызвать после микшера"
    for net in tp_nets:
        tp = Part("Connector", "TestPoint", value=net.name,
                  footprint="TestPoint:TestPoint_THTPad_D2.0mm_Drill1.0mm")
        tp[1] += net


# Крепёжные отверстия M3 ставятся в редакторе платы, а не здесь: деталь без
# выводов роняет раскладку схемы в SKiDL 2.3.0 (schematics/place.py, ветка
# «floating parts» создаёт Pin() без part).


# ---------------------------------------------------------------- сборка
power_input()
supply_5v()
supply_3v3_tuner()
tuner()
dac()
for ch, t, d, ref in (("L", tuner_l, dac_l, "J5"), ("R", tuner_r, dac_r, "J6")):
    inp, inn = Net(f"AMP_{ch}_INP"), Net(f"AMP_{ch}_INN")
    mixer_channel(t, d, inp, inn, ch)
    amp_channel(ch, inp, inn, ref)
amp_mute()
expander()
controls()
ribbon()
indication()

if __name__ == "__main__":
    ERC()
    generate_netlist(file_=str(KICAD_DIR / "Hybryd Radio.net"))
    if "--schematic" in sys.argv:
        generate_schematic(filepath=str(KICAD_DIR), top_name="Hybryd Radio",
                           title="IP-Radio carrier board", auto_stub=True,
                           # Только метки цепей, без проводов: при разводке проводов
                           # SKiDL 2.3.0 укладывает их поверх друг друга и замыкает
                           # цепи (поймано compare_netlists.py, 2026-10-01).
                           auto_stub_fanout=1, auto_stub_max_wire_pins=1)
