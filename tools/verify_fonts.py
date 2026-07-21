#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Weryfikacja fontow V3: parsuje WYGENEROWANE pliki PlexNN.h i renderuje probki
DOKLADNIE ta sama logika bitowa, co drawGlyph() w PlText.h (MSB-first,
rowBytes=(w+7)/8, kotwica na linii bazowej). Jesli probka wyglada dobrze na PNG,
to wyglada tak samo na urzadzeniu — bo to ten sam algorytm.

Uruchom po gen_fonts.py. Wynik: tools/_fontcheck.png
"""
import os, re, sys
from PIL import Image, ImageDraw

ROOT = os.path.join(os.path.dirname(__file__), "..")

def parse(name):
    txt = open(os.path.join(ROOT, f"{name}.h"), encoding="utf-8").read()
    # bitmaps
    bm = re.search(rf"{name}Bitmaps\[\] PROGMEM = {{(.*?)}};", txt, re.S).group(1)
    bitmaps = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", bm)]
    # glyphs
    gl = re.search(rf"{name}Glyphs\[\] PROGMEM = {{(.*?)}};", txt, re.S).group(1)
    glyphs = []
    for m in re.finditer(r"\{\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(-?\d+),\s*(-?\d+)\s*\}", gl):
        glyphs.append(tuple(int(x) for x in m.groups()))  # off,w,h,xadv,xoff,yoff
    # codepoints
    cp = re.search(rf"{name}Codepoints\[\] PROGMEM = {{(.*?)}};", txt, re.S).group(1)
    cps = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{4})", cp)]
    asc = int(re.search(rf"{name}Ascent = (\d+)", txt).group(1))
    return bitmaps, glyphs, cps, asc

def draw_string(draw, name, s, x, baseline, scale=2):
    bitmaps, glyphs, cps, asc = parse(name)
    idx = {c: i for i, c in enumerate(cps)}
    cx = x
    for ch in s:
        i = idx.get(ord(ch))
        if i is None:
            continue
        off, w, h, xadv, xoff, yoff = glyphs[i]
        rowbytes = (w + 7) // 8
        for yy in range(h):
            for xx in range(w):
                b = bitmaps[off + yy * rowbytes + (xx >> 3)]
                if b & (0x80 >> (xx & 7)):
                    px = (cx + xoff + xx)
                    py = (baseline + yoff + yy)
                    draw.rectangle([px*scale, py*scale, px*scale+scale-1, py*scale+scale-1], fill=0)
        cx += xadv
    return cx

SAMPLES = [
    ("Plex52", "21°", 52),
    ("Plex24", "14:32", 24),
    ("Plex20", "3,2 kW  −8°", 20),
    ("Plex13", "Częściowe zachmurzenie", 13),
    ("Plex11", "POWIETRZE · µg/m³ ✓", 11),
    ("Plex10", "15  18  21  → SE", 10),
]

def main():
    scale = 3
    W, H = 340, 30
    rows = []
    for name, text, px in SAMPLES:
        img = Image.new("L", (W*scale, (px+8)*scale), 255)
        d = ImageDraw.Draw(img)
        draw_string(d, name, text, 6, px, scale)
        rows.append((name, img))
    total_h = sum(r[1].height for r in rows) + 10*len(rows)
    canvas = Image.new("RGB", (W*scale, total_h), (245,245,240))
    y = 5
    for name, img in rows:
        canvas.paste(img.convert("RGB"), (0, y))
        y += img.height + 10
    out = os.path.join(os.path.dirname(__file__), "_fontcheck.png")
    canvas.save(out)
    print("zapisano", out)

if __name__ == "__main__":
    main()
