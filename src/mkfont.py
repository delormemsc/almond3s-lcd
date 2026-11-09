#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Сглаженный шрифт для экрана Almond: TTF -> font_aa.h (4 бита прозрачности на пиксель).

Шрифт JetBrains Mono (SIL OFL 1.1, https://github.com/JetBrains/JetBrainsMono).
Запуск: python3 mkfont.py <папка с JetBrainsMono-*.ttf> > font_aa.h

Каждый размер моноширинный: ячейка adv x h, глиф хранится на 2 пикселя шире
(по одному слева и справа), чтобы не обрезать выступающие края букв.
Коды 0x80.. - знаки вне ASCII (стрелки, градус, точка по центру и т.п.).
Размеры, которые программа не использует, в бинарник не попадают.
"""
import os
import sys
from PIL import Image, ImageDraw, ImageFont

EXTRA = ["↓", "↑", "°", "·", "…", "‹", "›"]	# 0x80..
ASCII = "".join(chr(c) for c in range(32, 127))
DIGITS = " 0123456789%:.-"

# имя, начертание, кегль в пикселях, набор знаков
SIZES = [
	("s", "Regular", 11, ASCII + "".join(EXTRA)),
	("m", "Regular", 13, ASCII + "".join(EXTRA)),
	("l", "SemiBold", 16, ASCII + "".join(EXTRA)),
	("xl", "SemiBold", 24, DIGITS),
	# экран версии 1: ячейка 8x16 как у прежнего растрового шрифта и крупные цифры 16 в ширину
	("mm", "Medium", 13, ASCII + "".join(EXTRA)),
	("xxl", "SemiBold", 27, DIGITS),
]


def code(ch):
	return ord(ch) if ord(ch) < 128 else 0x80 + EXTRA.index(ch)


def build(name, path, px, chars):
	f = ImageFont.truetype(path, px)
	asc, desc = f.getmetrics()
	adv = round(f.getlength("M"))
	h = asc + desc
	sw = adv + 2
	ims = {}
	for ch in chars:
		im = Image.new("L", (sw, h), 0)
		ImageDraw.Draw(im).text((1, asc), ch, font=f, fill=255, anchor="ls")
		ims[ch] = im
	# пустые строки сверху и снизу, общие для всех глифов, не храним
	top = min(im.getbbox()[1] for im in ims.values() if im.getbbox())
	bot = max(im.getbbox()[3] for im in ims.values() if im.getbbox())
	h, asc = bot - top, asc - top
	glyphs = {}
	for ch, im in ims.items():
		im = im.crop((0, top, sw, bot))
		px4 = [(v * 15 + 127) // 255 for v in im.getdata()]
		if len(px4) % 2:
			px4.append(0)
		glyphs[code(ch)] = bytes(px4[i] << 4 | px4[i + 1] for i in range(0, len(px4), 2))
	return adv, h, asc, sw, glyphs


def main():
	d = sys.argv[1]
	out = ["/* Сгенерировано mkfont.py из JetBrains Mono (SIL OFL 1.1), не править руками. */",
	       "#include <stdint.h>", "",
	       "struct aafont { uint8_t adv, h, base, sw; uint8_t first, last; const uint16_t *off; const uint8_t *data; };",
	       ""]
	for name, weight, px, chars in SIZES:
		adv, h, asc, sw, g = build(name, os.path.join(d, "JetBrainsMono-%s.ttf" % weight), px, chars)
		lo, hi = min(g), max(g)
		blob, offs = bytearray(), []
		empty = None
		for c in range(lo, hi + 1):
			if c in g:
				offs.append(len(blob))
				blob += g[c]
			else:
				if empty is None:		# пустой глиф для отсутствующих кодов
					empty = len(blob)
					blob += bytes((sw * h + 1) // 2)
				offs.append(empty)
		out.append("static const uint16_t font_%s_off[] = {" % name)
		out += ["\t" + ", ".join(str(o) for o in offs[i:i + 16]) + "," for i in range(0, len(offs), 16)]
		out.append("};")
		out.append("static const uint8_t font_%s_data[] = {" % name)
		out += ["\t" + ", ".join("0x%02x" % b for b in blob[i:i + 16]) + "," for i in range(0, len(blob), 16)]
		out.append("};")
		out.append("static const struct aafont font_%s __attribute__((unused)) = "
			   "{ %d, %d, %d, %d, %d, %d, font_%s_off, font_%s_data };"
			   % (name, adv, h, asc, sw, lo, hi, name, name))
		out.append("")
	sys.stdout.write("\n".join(out))


main()
