# -*- coding: utf-8 -*-
"""Урезание DejaVu Sans до знаков, которые реально появляются на экране.

Зачем это нужно
---------------
Встроенные шрифты LVGL (Montserrat) собраны только под ASCII: кириллицы
в них нет вообще. Достроить её обычным путём мешает то, что штатный
конвертер шрифтов LVGL — `lv_font_conv` — написан на Node.js, которого
на машине сборки нет.

Обходимся без него: LVGL умеет рендерить TTF прямо на плате
(`lv_tiny_ttf`, сборка на stb_truetype). Значит шрифт нужен не в виде
сгенерированного C-массива, а просто файлом в образе.

Полный DejaVu Sans — 756 КБ. Класть их в образ незачем: кириллица
и латиница занимают малую часть, остальное — письменности, которых
у нас не будет никогда. Этот скрипт оставляет только нужное.

Почему DejaVu
-------------
Лицензия Bitstream Vera плюс Arev — встраивать и распространять
разрешено, в отличие от системных Arial и Calibri. Файл уже лежит
на диске, его кладёт matplotlib; ничего скачивать не пришлось.
Лицензия рядом: firmware/main/assets/DejaVu-LICENSE.txt

Запуск
------
    python tools/subset_font.py

Результат — firmware/main/assets/dejavu_ru.ttf и dejavu_ru_bold.ttf.
Они лежат в репозитории, так что обычной сборке скрипт не нужен:
он понадобится, только если менять состав знаков.
"""
import os
from fontTools import subset

SRC = os.path.join(
    os.environ.get('LOCALAPPDATA', ''),
    r'Programs\Python\Python314\Lib\site-packages\matplotlib\mpl-data\fonts\ttf')

DST = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   '..', 'firmware', 'main', 'assets')

# Что оставляем.
#
#   U+0020-007E   латиница, цифры, знаки препинания — названия станций,
#                 частоты, адреса потоков
#   U+00A0        неразрывный пробел: «101.2 МГц» не должно рваться
#   U+00B0        градус — на экране настроек
#   U+00AB, U+00BB  русские кавычки-ёлочки: названия станций
#   U+0400-045F   кириллица целиком, включая Ё и ё
#   U+2010-2015   тире всех видов, в том числе длинное
#   U+2018-201F   типографские кавычки: приходят в названиях станций
#                 из каталога radio-browser и ломали бы вёрстку
#   U+2022        маркер списка
#   U+2026        многоточие одним знаком — LVGL им обрезает длинное
#   U+2190-2193   стрелки: подсказки органов управления
#   U+25B6        треугольник «играет»
RANGES = ('U+0020-007E,U+00A0,U+00B0,U+00AB,U+00BB,U+0400-045F,'
          'U+2010-2015,U+2018-201F,U+2022,U+2026,U+2190-2193,U+25B6')

PAIRS = (('DejaVuSans.ttf', 'dejavu_ru.ttf'),
         ('DejaVuSans-Bold.ttf', 'dejavu_ru_bold.ttf'))


def main():
    for src_name, dst_name in PAIRS:
        src_path = os.path.join(SRC, src_name)
        if not os.path.exists(src_path):
            raise SystemExit('нет исходного шрифта: %s\n'
                             'Он приходит вместе с matplotlib; поставить: '
                             'pip install matplotlib' % src_path)

        opts = subset.Options()
        opts.layout_features = ['*']
        opts.name_IDs = ['*']        # имя и лицензия остаются в файле
        opts.notdef_outline = True   # видимый прямоугольник вместо пустоты,
                                     # если знак всё же не найдётся
        opts.recalc_bounds = True
        opts.drop_tables += ['DSIG']

        font = subset.load_font(src_path, opts)
        sub = subset.Subsetter(options=opts)
        sub.populate(unicodes=subset.parse_unicodes(RANGES))
        sub.subset(font)

        out = os.path.normpath(os.path.join(DST, dst_name))
        subset.save_font(font, out, opts)
        font.close()

        print('%-20s %7d -> %6d байт' % (dst_name,
                                         os.path.getsize(src_path),
                                         os.path.getsize(out)))


if __name__ == '__main__':
    main()
