#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Mierzy szerokosc napisu w pikselach w wybranym kroju i WYKRYWA BRAKUJACE ZNAKI.

    python3 tools/szer.py Plex11 "zakup dziś 999,99 zł" "sprzedaż dziś 9,99 zł"

Sumuje xadv dokladnie tak, jak plex::str() na urzadzeniu, wiec liczba stad i
liczba na szkle to ta sama liczba.

PO CO TO ISTNIEJE
-----------------
Mierzylem dotad w glowie i dwa razy sie pomylilem:
  * v183: druga linia opisu pogody wyladowala na y=208, poza sprite'em VIEW_H=206
    (bledny komentarz "<240" przezyl podzial sprite'a) - napis po prostu znikl;
  * "100 %" ze spacja to 36 px, a nie 32 px, przez co element wystawal poza kolumne.
Do tego drawGlyph() POMIJA znak, ktorego nie ma w kroju, BEZ SLOWA - tak zginely
strzalki ◀ i ▸, ktorych Plex w ogole nie zawiera, a ja zobaczylem puste miejsce
i dlugo szukalem bledu w logice zamiast w foncie. Dlatego brak znaku jest tu
bledem krzyczacym, a nie cisza.
"""
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")


def parse(name):
    txt = open(os.path.join(ROOT, name + ".h"), encoding="utf-8").read()
    gl = re.search(name + r"Glyphs\[\] PROGMEM = \{(.*?)\};", txt, re.S).group(1)
    glyphs = [tuple(int(x) for x in m.groups()) for m in
              re.finditer(r"\{\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(-?\d+),\s*(-?\d+)\s*\}", gl)]
    cp = re.search(name + r"Codepoints\[\] PROGMEM = \{(.*?)\};", txt, re.S).group(1)
    cps = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{4})", cp)]
    return glyphs, {c: i for i, c in enumerate(cps)}


def szerokosc(name, s):
    glyphs, idx = parse(name)
    w, brak = 0, []
    for ch in s:
        i = idx.get(ord(ch))
        if i is None:
            brak.append(ch)
            continue
        w += glyphs[i][3]          # xadv
    return w, brak


if __name__ == "__main__":
    font = sys.argv[1]
    zle = False
    for s in sys.argv[2:]:
        w, brak = szerokosc(font, s)
        uwaga = ""
        if brak:
            zle = True
            uwaga = "   <<< BRAK ZNAKOW W KROJU: " + " ".join(
                "%r U+%04X" % (c, ord(c)) for c in brak) + " (zostana POMINIETE)"
        print("%4d px  %-34s %s%s" % (w, font, repr(s), uwaga))
    sys.exit(1 if zle else 0)
