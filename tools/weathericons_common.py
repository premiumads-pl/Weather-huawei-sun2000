#!/usr/bin/env python3
"""Wspolny parser/writer formatu WeatherIcons.h — uzywany przez
measure_weather_icons.py, gen_weather_icons.py i verify_weather_icons.py.

Trzyma sie w jednym miejscu, zeby pomiar (KROK 1) i generator (KROK 2)
zawsze czytaly ten sam plik tak samo — gdyby kazdy skrypt mial wlasny
regex, prędzej czy pozniej rozjechalyby sie po cichu.

Rozumie DWA formaty pliku WeatherIcons.h:
  - "legacy"  - stary format, jedna ikona to dwie tablice PROGMEM:
                ICO_<NAZWA>_RGB[4096] (uint16_t, RGB565) i
                ICO_<NAZWA>_A[4096] (uint8_t, alfa 0-255).
  - "indexed" - nowy format (patrz naglowek WeatherIcons.h po konwersji):
                paleta ICO_<NAZWA>_PAL[N] (uint16_t, RGB565) w RAM,
                dane ICO_<NAZWA>_PIX[4096] (uint8_t, indeks 0..N-1) w PROGMEM,
                alfa ICO_<NAZWA>_A[4096] (uint8_t) w PROGMEM bez zmian.

parse_legacy() i parse_indexed() zwracaja ikony w tej samej postaci
(pelna tablica RGB565 + pelna tablica alfy, oba dlugosci 64*64), zeby
verify_weather_icons.py mogl porownywac obraz-do-obrazu bez wiedzy o
formacie zrodlowym.
"""
import re
from collections import OrderedDict

SRC = 64
NPIX = SRC * SRC

# Kolejnosc jak w WeatherIcons.h / enum IconId w WeatherUi.cpp — nieistotna
# dla parsowania (regex znajduje wszystkie), ale zachowana dla czytelnych
# raportow i stabilnej kolejnosci w wygenerowanym pliku.
ICON_ORDER = ["SUN", "PARTLY", "CLOUD", "FOG", "DRIZZLE", "RAIN", "SNOW", "STORM"]


def _extract_array(text, name):
    """Wyciaga liczby z 'static const ... <name>[...] PROGMEM = { ... };'."""
    m = re.search(
        r"\b" + re.escape(name) + r"\s*\[\s*\d+\s*\]\s*PROGMEM\s*=\s*\{(.*?)\}\s*;",
        text,
        re.DOTALL,
    )
    if not m:
        return None
    body = m.group(1)
    # Liczby sa albo hex (0xRRGG) albo dziesietne — int(x, 0) rozroznia same.
    return [int(tok, 0) for tok in re.findall(r"0[xX][0-9a-fA-F]+|\d+", body)]


def parse_legacy(path):
    """Stary format: ICO_<NAZWA>_RGB[4096] + ICO_<NAZWA>_A[4096]."""
    text = open(path, encoding="utf-8").read()
    icons = OrderedDict()
    names = re.findall(r"ICO_(\w+)_RGB\[", text)
    order = [n for n in ICON_ORDER if n in names] + [n for n in names if n not in ICON_ORDER]
    for name in order:
        rgb = _extract_array(text, f"ICO_{name}_RGB")
        alpha = _extract_array(text, f"ICO_{name}_A")
        if rgb is None or alpha is None:
            continue
        assert len(rgb) == NPIX, f"{name}: RGB ma {len(rgb)} pikseli, oczekiwano {NPIX}"
        assert len(alpha) == NPIX, f"{name}: alfa ma {len(alpha)} pikseli, oczekiwano {NPIX}"
        icons[name] = {"rgb": rgb, "alpha": alpha}
    return icons


def parse_indexed(path):
    """Nowy format: ICO_<NAZWA>_PAL[N] + ICO_<NAZWA>_PIX[4096] + ICO_<NAZWA>_A[4096].

    Zwraca ikony w tej samej postaci co parse_legacy() (rozwinieta tablica
    RGB565 4096 pikseli) — indeks jest tu tylko formatem zapisu, nie zmienia
    tego co skrypt weryfikacyjny widzi.
    """
    text = open(path, encoding="utf-8").read()
    icons = OrderedDict()
    names = re.findall(r"ICO_(\w+)_PIX\[", text)
    order = [n for n in ICON_ORDER if n in names] + [n for n in names if n not in ICON_ORDER]
    for name in order:
        pal = _extract_array(text, f"ICO_{name}_PAL")
        pix = _extract_array(text, f"ICO_{name}_PIX")
        alpha = _extract_array(text, f"ICO_{name}_A")
        if pal is None or pix is None or alpha is None:
            continue
        assert len(pix) == NPIX, f"{name}: PIX ma {len(pix)} pikseli, oczekiwano {NPIX}"
        assert len(alpha) == NPIX, f"{name}: alfa ma {len(alpha)} pikseli, oczekiwano {NPIX}"
        rgb = [pal[i] for i in pix]
        icons[name] = {"rgb": rgb, "alpha": alpha, "palette_size": len(pal)}
    return icons


def parse_auto(path):
    """Rozpoznaje format po tym, ktore tablice sa w pliku."""
    text = open(path, encoding="utf-8").read()
    if re.search(r"ICO_\w+_PIX\[", text):
        return parse_indexed(path)
    return parse_legacy(path)


def rgb565_to_rgb888(v):
    r5 = (v >> 11) & 0x1F
    g6 = (v >> 5) & 0x3F
    b5 = v & 0x1F
    # Rozszerzanie do 8 bitow tak samo jak TFT_eSPI/kazdy typowy konwerter:
    # powielenie najstarszych bitow w wolne miejsce (nie samo <<3/<<2),
    # zeby 0xFFFF dawalo (255,255,255), a nie (248,252,248).
    r8 = (r5 << 3) | (r5 >> 2)
    g8 = (g6 << 2) | (g6 >> 4)
    b8 = (b5 << 3) | (b5 >> 2)
    return r8, g8, b8
