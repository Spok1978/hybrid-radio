"""Контур платы-носителя и крепёжные отверстия -> hardware/kicad/Hybryd Radio.kicad_pcb.

Запускать ТОЛЬКО питоном из комплекта KiCad (в нём есть модуль pcbnew):
    "%LOCALAPPDATA%/Programs/KiCad/10.0/bin/python.exe" hardware/skidl/make_board.py signal

Плата — аргумент: power, signal или amp; размер берётся из boards.py.
Отверстия M3 по углам, в 4 мм от краёв.

Отверстия помечены «только на плате» (board only): «Обновить плату из
схемы» не должно их удалять, у них нет символов в схеме.

Скрипт трогает только контур (слой Edge.Cuts) и отверстия H1-H4; детали,
уже перенесённые из схемы, и дорожки остаются как есть.
"""

import sys
from pathlib import Path

import pcbnew

sys.path.insert(0, str(Path(__file__).parent))
from boards import BOARDS  # noqa: E402

BOARD = sys.argv[1] if len(sys.argv) > 1 else "signal"     # power / signal / amp
PCB = Path(__file__).resolve().parents[1] / "kicad" / BOARD / f"{BOARD}.kicad_pcb"
FP_LIB = Path(__file__).resolve().parents[1] / "kicad" / "hybrid-radio.pretty"

W, H = BOARDS[BOARD]["size"]  # мм, решение 2026-10-08 (docs/33, §15)
X0, Y0 = 50.0, 50.0         # левый верхний угол на листе
INSET = 4.0                 # центр отверстия от края
HOLE_FP = "MountingHole_M3_FabRef"   # надпись на F.Fab, не на шелкографии (make_footprints.py)


def mm(v):
    return pcbnew.FromMM(v)


def main():
    # Отверстия грузим ДО загрузки платы и одним плагином: pcbnew.FootprintLoad
    # каждый раз создаёт плагин заново, и на повторных запусках он у нас
    # возвращал пустой SwigPyObject (AttributeError, 2026-10-07).
    plug = pcbnew.PCB_IO_MGR.FindPlugin(pcbnew.PCB_IO_MGR.KICAD_SEXP)
    holes = [plug.FootprintLoad(str(FP_LIB), HOLE_FP) for _ in range(4)]
    board = pcbnew.LoadBoard(str(PCB))

    for d in list(board.GetDrawings()):
        if d.GetLayer() == pcbnew.Edge_Cuts:
            board.Remove(d)
    for fp in list(board.GetFootprints()):
        if fp.GetReference() in ("H1", "H2", "H3", "H4"):
            board.Remove(fp)

    rect = pcbnew.PCB_SHAPE(board)
    rect.SetShape(pcbnew.SHAPE_T_RECT)
    rect.SetStart(pcbnew.VECTOR2I(mm(X0), mm(Y0)))
    rect.SetEnd(pcbnew.VECTOR2I(mm(X0 + W), mm(Y0 + H)))
    rect.SetLayer(pcbnew.Edge_Cuts)
    rect.SetWidth(mm(0.1))
    board.Add(rect)

    corners = [(X0 + INSET, Y0 + INSET), (X0 + W - INSET, Y0 + INSET),
               (X0 + INSET, Y0 + H - INSET), (X0 + W - INSET, Y0 + H - INSET)]
    for i, ((x, y), fp) in enumerate(zip(corners, holes), 1):
        fp.SetReference(f"H{i}")
        fp.SetPosition(pcbnew.VECTOR2I(mm(x), mm(y)))
        fp.SetBoardOnly(True)
        board.Add(fp)

    board.Save(str(PCB))
    # Объекты pcbnew после Save в этой сборке иногда приходят «пустыми»
    # (SwigPyObject без методов) — итог печатаем из своих констант, а
    # проверку делает DRC: kicad-cli pcb drc.
    print(f"{PCB}: контур {W:g} x {H:g} мм, отверстия M3: 4")


if __name__ == "__main__":
    sys.exit(main())
