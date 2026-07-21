#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Generator fontow bitmapowych dla motywu V3 "Pasmowy".

ZRODLO PRAWDY dla plikow PlexNN.h. Nie edytuj tych naglowkow recznie — zmien tu
i uruchom ponownie.

Rasteryzuje krój z pliku TTF do formatu, ktory rozumie PlText.h (pltxt::FontSet):
  * PlexNNBitmaps[]    — bity 1-bpp, MSB-first, rowBytes = (w+7)/8 na wiersz
  * PlexNNGlyphs[]     — GFXglyph {bitmapOffset, width, height, xAdvance, xOffset, yOffset}
  * PlexNNCodepoints[] — uint16_t Unicode kazdego glifu (wyszukiwanie liniowe w PlText.h)
  * PlexNNCount, PlexNNAscent

To DOKLADNIE ten sam uklad, co maja PlFont10/14/18 — V3 nie wprowadza nowej
sciezki rysowania, tylko nowe rozmiary w istniejacej.

--- FONT ZRODLOWY ---
Specyfikacja projektu mowi "IBM Plex Sans Condensed". W srodowisku budujacym tego
pliku NIE MA i nie da sie go pobrac (restrykcje sieci na binaria). Uzywamy wiec
DejaVu Sans Condensed — tez kondensowany humanistyczny grotesk, z KOMPLETEM
polskich znakow oraz ° µ ³ − → · ✓, wolny do osadzenia. Roznica przy 10–24 px z
2 m jest znikoma; widac ja tylko na wyrozniku 52 px.

Podmiana na prawdziwy IBM Plex = jedna zmienna FONT_SOURCES ponizej (podaj sciezki
do plikow .ttf dla wag 500/600/700) i ponowne uruchomienie. Reszta bez zmian.
"""

import os
import sys

try:
    import freetype
except ImportError:
    sys.exit("Brak modulu freetype-py — zainstaluj: pip3 install freetype-py --break-system-packages")

# --- FONT ZRODLOWY (podmien tu na IBM Plex, gdy bedzie dostepny) ---------------
# Klucz = waga wg CSS (500/600/700). Wartosc = sciezka do pliku TTF.
# DejaVu nie ma wagi 600, wiec Semibold mapujemy na Bold — na tak malych bitmapach
# roznica 600/700 i tak gubi sie w rastrze.
DEJAVU = "/usr/share/fonts/truetype/dejavu"
FONT_SOURCES = {
    500: f"{DEJAVU}/DejaVuSansCondensed.ttf",       # Regular jako "Medium"
    600: f"{DEJAVU}/DejaVuSansCondensed-Bold.ttf",  # brak 600 → Bold
    700: f"{DEJAVU}/DejaVuSansCondensed-Bold.ttf",
}

# --- ROZMIARY I WAGI wg SPECYFIKACJI ------------------------------------------
# (px, waga, nazwa, zestaw_znakow). Rozmiar pikselowy = wysokosc EM freetype.
# Spec: 10 osie / 11 etykiety-wersaliki / 13 tekst / 20 wartosci / 24 zegar / 52 wyroznik.
#
# Zestaw "FULL" = pelny CHARS (ASCII+polskie+specjalne). Zestawy okrojone dla
# najwiekszych fontow to NIE oszczednosc na sile, tylko fakt: wyroznik 52 px pokazuje
# WYLACZNIE liczbe z jednostka stopnia/minusem (temperatura, "3,2"), a zegar 24 px —
# godzine. Pelny alfabet w tych rozmiarach to setki bajtow na glif, ktory nigdy nie
# zostanie narysowany. 52 px pelny = 16 kB; okrojony = ~1.5 kB.
DIGITS   = "0123456789"
BIG_SET  = DIGITS + " ,.:°−-+%"          # wyroznik: liczby, przecinek, stopien, minus, procent
CLOCK_SET= DIGITS + " :.°"               # zegar + ewentualny stopien na wariancie nocnym

FONTS = [
    (10, 500, "Plex10", "FULL"),      # osie wykresow, drobny tekst
    (11, 600, "Plex11", "FULL"),      # ETYKIETY WERSALIKAMI (PRAD, POWIETRZE...)
    (13, 500, "Plex13", "FULL"),      # tekst zdaniowy
    (20, 600, "Plex20", "FULL"),      # wartosci (3,2 kW / 21°) — pelny, bo bywaja jednostki literowe
    (24, 700, "Plex24", CLOCK_SET),   # zegar 14:32
    (52, 700, "Plex52", BIG_SET),     # wielki wyroznik (temperatura na glownym)
]

# --- ZESTAW ZNAKOW ------------------------------------------------------------
# ASCII drukowalne 0x20..0x7E + polskie + znaki specjalne ze specyfikacji.
# ▮ ▯ (pelny/pusty prostokat) celowo POMIJAMY — na urzadzeniu rysujemy je
# prymitywami (fillRect), taniej i ostrzej niz glif. µ ³ − → · ✓ zostaja, bo
# wystepuja w tekscie ("µg/m³", "→ SE", "6 d 4 h", "✓").
CHARS = [chr(c) for c in range(0x20, 0x7F)]
CHARS += list("ąćęłńóśźżĄĆĘŁŃÓŚŹŻ")   # polskie
CHARS += list("°µ³−→·✓")              # stopien, mikro, szescian, minus typogr., strzalka, kropka srodkowa, ptaszek

# codepoints musza zmiescic sie w uint16_t (PlText.h czyta pgm_read_word)
for ch in CHARS:
    assert ord(ch) <= 0xFFFF, f"znak U+{ord(ch):04X} poza uint16_t"


def render_font(px, weight, name, charset):
    path = FONT_SOURCES[weight]
    if not os.path.exists(path):
        sys.exit(f"Brak pliku fontu: {path}")
    face = freetype.Face(path)
    face.set_pixel_sizes(0, px)

    # "FULL" = pelny CHARS; inaczej tylko podane znaki (zachowujac kolejnosc CHARS,
    # zeby codepoints rosly monotonicznie tak jak w reszcie fontow).
    if charset == "FULL":
        chars = CHARS
    else:
        wanted = set(charset)
        chars = [c for c in CHARS if c in wanted]

    bitmaps = bytearray()
    glyphs = []       # (offset, w, h, xadv, xoff, yoff, cp)
    ascent = 0

    for ch in chars:
        cp = ord(ch)
        # FT_LOAD_TARGET_MONO + RENDER = rasteryzacja 1-bpp (bez antyaliasingu),
        # bo docelowy format to 1 bit/piksel. Antyalias nie ma jak sie zmiescic.
        face.load_char(ch, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_MONO)
        g = face.glyph
        bm = g.bitmap
        w, h = bm.width, bm.rows
        xadv = g.advance.x >> 6          # 26.6 fixed → piksele
        xoff = g.bitmap_left
        yoff = -g.bitmap_top             # GFX: yOffset = -(gora glifu nad linia bazowa)

        offset = len(bitmaps)
        rowbytes_out = (w + 7) // 8
        # freetype pakuje wiersz do bm.pitch bajtow, MSB-first — przepakowujemy do
        # ciaslego rowBytes = (w+7)/8, bo tego oczekuje drawGlyph w PlText.h.
        src = bm.buffer
        pitch = bm.pitch
        for row in range(h):
            for rb in range(rowbytes_out):
                byte = 0
                for bit in range(8):
                    x = rb * 8 + bit
                    if x < w:
                        srcbyte = src[row * pitch + (x >> 3)]
                        if srcbyte & (0x80 >> (x & 7)):
                            byte |= (0x80 >> bit)
                bitmaps.append(byte)

        glyphs.append((offset, w, h, xadv, xoff, yoff, cp))
        ascent = max(ascent, g.bitmap_top)

    return bitmaps, glyphs, ascent


def emit_header(px, weight, name, bitmaps, glyphs, ascent):
    lines = []
    A = lines.append
    A("#pragma once")
    A("// WYGENEROWANE przez tools/gen_fonts.py — NIE EDYTUJ RECZNIE.")
    A(f"// Krój źródłowy: DejaVu Sans Condensed (zastępnik IBM Plex — patrz gen_fonts.py).")
    A(f"// Rozmiar {px} px, waga CSS {weight}. Znaków: {len(glyphs)}.")
    A("#include <TFT_eSPI.h>")
    A("")
    # bitmaps
    A(f"static const uint8_t {name}Bitmaps[] PROGMEM = {{")
    row = "  "
    for i, b in enumerate(bitmaps):
        row += f"0x{b:02X},"
        if len(row) >= 96:
            A(row); row = "  "
    if row.strip():
        A(row)
    A("};")
    A("")
    # glyphs
    A(f"static const GFXglyph {name}Glyphs[] PROGMEM = {{")
    for (offset, w, h, xadv, xoff, yoff, cp) in glyphs:
        A(f"  {{ {offset:5d}, {w:3d}, {h:3d}, {xadv:3d}, {xoff:3d}, {yoff:4d} }},  // U+{cp:04X}")
    A("};")
    A("")
    # codepoints
    A(f"static const uint16_t {name}Codepoints[] PROGMEM = {{")
    row = "  "
    for (_, _, _, _, _, _, cp) in glyphs:
        row += f"0x{cp:04X}, "
        if len(row) >= 92:
            A(row); row = "  "
    if row.strip():
        A(row)
    A("};")
    A("")
    A(f"static const uint8_t {name}Count = {len(glyphs)};")
    A(f"static const int {name}Ascent = {ascent};")
    A("")
    return "\n".join(lines) + "\n"


def main():
    outdir = os.path.join(os.path.dirname(__file__), "..")
    total = 0
    summary = []
    for (px, weight, name, charset) in FONTS:
        bitmaps, glyphs, ascent = render_font(px, weight, name, charset)
        header = emit_header(px, weight, name, bitmaps, glyphs, ascent)
        path = os.path.join(outdir, f"{name}.h")
        with open(path, "w", encoding="utf-8") as f:
            f.write(header)
        size = len(bitmaps) + len(glyphs) * 7 + len(glyphs) * 2
        total += size
        summary.append((name, px, len(glyphs), len(bitmaps), ascent, size))
        print(f"{name}: {len(glyphs)} glifow, bitmapy {len(bitmaps)} B, ascent {ascent}, ~{size} B flash")

    print(f"\nRAZEM ~{total} B ({total/1024:.1f} kB) flash na 6 fontow")
    return summary


if __name__ == "__main__":
    main()
