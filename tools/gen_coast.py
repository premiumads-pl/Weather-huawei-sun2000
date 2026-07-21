#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Konwersja konturu brzegu (docs/design-v3/coastline-polyline-320x172.txt, format
sciezki SVG M/L/Z) do zwartego naglowka C dla radaru V3.

Uklad pikselowy 320x172 pokrywa sie DOKLADNIE z siatka rastra radaru
(gmapr::MAP_W x MAP_H, Gdynia w centrum, ~300 km) — projektant je zgral, wiec
piksel konturu = piksel rastra opadu. Na urzadzeniu wystarczy przeskalowac 320x172
do prostokata radaru na ekranie i narysowac linie w kolorze brzegu, a opad naniesc
z tego samego ukladu.

Wynik: CoastMap.h (zrodlo prawdy — nie edytuj recznie).
  COAST_XY[]     — pary int16 (x,y) w ukladzie 320x172
  COAST_START[]  — indeks poczatku kazdej podsciezki w COAST_XY (co druga liczba)
  COAST_SUBPATHS, COAST_POINTS
"""
import os, re

ROOT = os.path.join(os.path.dirname(__file__), "..")
SRC = os.path.join(ROOT, "docs/design-v3/coastline-polyline-320x172.txt")

def parse():
    txt = open(SRC, encoding="utf-8").read()
    # tnij na komendy M/L/Z z para liczb (Z bez liczb)
    toks = re.findall(r"([MLZ])\s*(-?\d+)?\s+?(-?\d+)?", txt)
    subs = []      # lista podsciezek, kazda = lista (x,y)
    cur = None
    for cmd, a, b in toks:
        if cmd == "Z":
            continue
        if a is None or b is None:
            continue
        x, y = int(a), int(b)
        if cmd == "M":
            if cur:
                subs.append(cur)
            cur = [(x, y)]
        else:  # L
            if cur is None:
                cur = [(x, y)]
            else:
                cur.append((x, y))
    if cur:
        subs.append(cur)
    # odrzuc podsciezki krotsze niz 2 punkty (nie ma czego rysowac)
    subs = [s for s in subs if len(s) >= 2]
    return subs

def emit(subs):
    xy = []
    starts = []
    for s in subs:
        starts.append(len(xy) // 2)
        for (x, y) in s:
            xy += [x, y]
    L = []
    A = L.append
    A("#pragma once")
    A("// WYGENEROWANE przez tools/gen_coast.py — NIE EDYTUJ RECZNIE.")
    A("// Kontur brzegu 320x172 (Gdynia ±300 km), zgrany z siatka rastra radaru gmapr.")
    A("// Radar V3 skaluje ten uklad do prostokata na ekranie i rysuje linie brzegu.")
    A("#include <cstdint>")
    A("")
    A("namespace coast {")
    A(f"constexpr int SRC_W = 320;")
    A(f"constexpr int SRC_H = 172;")
    A(f"constexpr int POINTS = {len(xy)//2};")
    A(f"constexpr int SUBPATHS = {len(starts)};")
    A("")
    A(f"static const int16_t XY[] PROGMEM = {{")
    row = "  "
    for i in range(0, len(xy), 2):
        row += f"{xy[i]},{xy[i+1]}, "
        if len(row) >= 90:
            A(row); row = "  "
    if row.strip():
        A(row)
    A("};")
    A("")
    # start + dlugosc kazdej podsciezki
    A(f"static const uint16_t START[] PROGMEM = {{")
    row = "  "
    for s in starts:
        row += f"{s}, "
        if len(row) >= 90:
            A(row); row = "  "
    if row.strip():
        A(row)
    A("};")
    lens = []
    for i, st in enumerate(starts):
        end = starts[i+1] if i+1 < len(starts) else len(xy)//2
        lens.append(end - st)
    A(f"static const uint16_t LEN[] PROGMEM = {{")
    row = "  "
    for l in lens:
        row += f"{l}, "
        if len(row) >= 90:
            A(row); row = "  "
    if row.strip():
        A(row)
    A("};")
    A("")
    A("}  // namespace coast")
    return "\n".join(L) + "\n"

def main():
    subs = parse()
    out = emit(subs)
    path = os.path.join(ROOT, "CoastMap.h")
    open(path, "w", encoding="utf-8").write(out)
    pts = sum(len(s) for s in subs)
    print(f"CoastMap.h: {len(subs)} podsciezek, {pts} punktow, ~{pts*4 + len(subs)*4} B flash")

if __name__ == "__main__":
    main()
