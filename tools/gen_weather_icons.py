#!/usr/bin/env python3
"""KROK 2 (v118, oszczedzanie flasha ikon pogody): generuje WeatherIcons.h
w formacie PALETY INDEKSOWANEJ z pliku w starym formacie (RGB565+alfa).

DLACZEGO TEN FORMAT (zmierzone tools/measure_weather_icons.py, 21.07.2026)
---------------------------------------------------------------------------
    ikona     kolory (widoczne px, alfa>0)   alfa binarna?
    SUN       43                              NIE (0-255, pelny zakres)
    PARTLY    171                             NIE
    CLOUD     96                              NIE
    FOG       91                              NIE
    DRIZZLE   91                              NIE
    RAIN      113                             NIE
    SNOW      91                              NIE
    STORM     111                             NIE
    unia kolorow ze wszystkich 8 ikon: 392

Kazda ikona miesci sie w <=256 unikalnych kolorow -> 8-bitowy indeks bez
ZADNEJ kwantyzacji (0 bledu na widocznym pikselu, patrz raport ponizej).
Alfa NIE jest binarna (antyaliasing na krawedziach ma pelny zakres 0-255),
wiec zostaje 8-bit bez zmian — kwantyzacja alfy do np. 4 bitow zepsulaby
gladkie krawedzie, a to jest dokladnie ten przypadek, w ktorym brief kazal
zostac przy wiekszym formacie zamiast psuc wyglad.
Kolory ze wszystkich 8 ikon razem to 392 > 256, wiec WSPOLNA paleta miedzy
ikonami by sie nie zmiescila — kazda ikona ma WLASNA palete (dokladny
rozmiar = liczba jej unikalnych widocznych kolorow, bez zaokraglania w gore).

Piksele z alfa==0 (tlo) dostaja indeks 0 niezalenie od oryginalnego RGB —
blit() w WeatherIcons.h mnozy kolor przez alfe PRZED usrednieniem (patrz
"premultiply" w kodzie), wiec kolor pod alfa==0 nigdy nie trafia na ekran;
nadanie mu prawdziwego koloru tylko zuzywaloby miejsce w palecie.

WYNIK (flash, PROGMEM, per ikone, przed/po):
    stare: RGB[4096]*2B + A[4096]*1B                    = 12 288 B
    nowe:  PAL[N]*2B + PIX[4096]*1B + A[4096]*1B         = 8192+2N B
Dokladne sumy dla wszystkich 8 ikon drukuje ten skrypt na koncu.

WEJSCIE
-------
WeatherIcons.h w STARYM formacie (ICO_x_RGB[4096] + ICO_x_A[4096] na ikone).
Jesli repo ma juz NOWY format (bo ktos juz przekonwertowal), stary format
jest w historii gita — znajdz commit SPRZED konwersji i wyciagnij z niego:

    git log --oneline -- WeatherIcons.h
    git show <sha-sprzed-konwersji>:WeatherIcons.h > /tmp/WeatherIcons.legacy.h
    python3 tools/gen_weather_icons.py --input /tmp/WeatherIcons.legacy.h

UZYCIE
------
    python3 tools/gen_weather_icons.py                    # WeatherIcons.h -> WeatherIcons.h (w miejscu)
    python3 tools/gen_weather_icons.py --input X --output Y --dry-run

Ten skrypt PODMIENIA TYLKO sekcje danych (preambula + 8x PAL/PIX/A) — od
linii 'enum IconId' do konca pliku kopiuje WEJSCIE 1:1, bez zmian. To
CELOWE: reszta pliku (blit/draw/rgbFor/...) nalezy do KROKU 3 (rysowanie)
i jest edytowana recznie, nie przez ten generator — patrz WeatherIcons.h,
funkcja blit(), po konwersji.
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from weathericons_common import ICON_ORDER, NPIX, SRC, parse_legacy  # noqa: E402

ANCHOR = "enum IconId"


def build_indexed_icon(rgb, alpha):
    """Zwraca (palette, pix, max_visible_error). Bezstratne dla alfa>0."""
    palette = []
    color_to_idx = {}
    pix = [0] * NPIX
    for i in range(NPIX):
        if alpha[i] == 0:
            pix[i] = 0  # niewidoczny piksel — indeks bez znaczenia, patrz naglowek
            continue
        c = rgb[i]
        idx = color_to_idx.get(c)
        if idx is None:
            idx = len(palette)
            color_to_idx[c] = idx
            palette.append(c)
        pix[i] = idx

    if len(palette) == 0:
        palette = [0]  # ikona calkiem przezroczysta (nie powinno sie zdarzyc, ale bez wyjatku)

    assert len(palette) <= 256, f"paleta ma {len(palette)} kolorow, indeks 8-bit nie starcza"

    # Weryfikacja bezstratnosci: odtworz i porownaj z oryginalem NA WIDOCZNYCH pikselach.
    max_err = 0
    for i in range(NPIX):
        if alpha[i] == 0:
            continue
        recon = palette[pix[i]]
        if recon != rgb[i]:
            max_err = max(max_err, abs(recon - rgb[i]))

    return palette, pix, max_err


def fmt_array(values, per_line, fmt):
    lines = []
    for i in range(0, len(values), per_line):
        chunk = values[i:i + per_line]
        lines.append("  " + ",".join(fmt(v) for v in chunk) + ",")
    return "\n".join(lines)


def render_icon_block(name, palette, pix, alpha):
    hexfmt = lambda v: f"0x{v:04X}"
    decfmt = lambda v: str(v)
    out = []
    out.append(f"static const uint16_t ICO_{name}_PAL[{len(palette)}] PROGMEM = {{")
    out.append(fmt_array(palette, 16, hexfmt))
    out.append("};")
    out.append("")
    out.append(f"static const uint8_t ICO_{name}_PIX[{NPIX}] PROGMEM = {{")
    out.append(fmt_array(pix, 24, decfmt))
    out.append("};")
    out.append("")
    out.append(f"static const uint8_t ICO_{name}_A[{NPIX}] PROGMEM = {{")
    out.append(fmt_array(alpha, 24, decfmt))
    out.append("};")
    return "\n".join(out)


PREAMBLE = '''#pragma once

#include <TFT_eSPI.h>
#include <pgmspace.h>

// Ikony 64x64 w PALECIE INDEKSOWANEJ (v118 — patrz audyt/ i BACKLOG.md).
//
// FORMAT (per ikona):
//   ICO_x_PAL[N]    uint16_t RGB565, PROGMEM. N = dokladna liczba unikalnych
//                   WIDOCZNYCH kolorow tej ikony (rozne dla kazdej, patrz
//                   tabela nizej) — bez zaokraglania w gore do 256.
//   ICO_x_PIX[4096] uint8_t, PROGMEM. Indeks 0..N-1 do ICO_x_PAL, jeden na
//                   piksel (64x64, wiersz po wierszu). Piksele niewidoczne
//                   (alfa==0) maja indeks 0 — nieistotne, patrz nizej.
//   ICO_x_A[4096]   uint8_t, PROGMEM. Alfa 0-255, BEZ ZMIAN wzgledem
//                   poprzedniego formatu (patrz "dlaczego alfa zostaje").
//
// SKAD SIE WZIAL TEN FORMAT — zmierzone tools/measure_weather_icons.py:
//   ikona    kolory(widoczne)   alfa binarna?
//   SUN      43                 nie (0-255)
//   PARTLY   171                nie
//   CLOUD    96                 nie
//   FOG      91                 nie
//   DRIZZLE  91                 nie
//   RAIN     113                nie
//   SNOW     91                 nie
//   STORM    111                nie
// Kazda ikona <=256 kolorow -> indeks 8-bit BEZ KWANTYZACJI (0 bledu na
// widocznym pikselu — patrz konsola gen_weather_icons.py przy generowaniu).
// Unia kolorow wszystkich 8 ikon to 392 > 256, wiec paleta jest PER IKONA,
// nie wspolna — wspolna by nie starczyla.
//
// DLACZEGO ALFA ZOSTAJE 8-BIT: alfa NIE jest binarna w zadnej ikonie —
// krawedzie maja antyaliasing (naglowek mowil "supersampling 8x"; alfa
// realnie uzywa calego zakresu 0-255 na krawedziach). Kwantyzacja alfy do
// np. 4 bitow (16 poziomow) zrobilaby widoczne "schodki" na gladkich
// krawedziach kola/chmury — brief wprost zabraniał psuc gradient dla
// oszczednosci, wiec alfa jest nietknieta.
//
// DLACZEGO PALETA JEST W PROGMEM (flash), A NIE W RAM: N kolorow x 2 B na
// ikone to za malo, zeby trzymac wszystkie 8 palet w RAM na stale (patrz
// bariera 76 000 B w CONTRIBUTING.md) — a i nie trzeba. wxico::blit()
// kopiuje palete BIEZACO RYSOWANEJ ikony do malej tablicy NA STOSIE raz na
// wywolanie blit(), a nie raz na piksel — patrz komentarz w blit().
//
// Usrednianie boxem + alpha blending z tlem sprite'a (bez zmian wzgledem
// starego formatu) — patrz funkcje nizej.
//
// REGENERACJA: python3 tools/gen_weather_icons.py
//   Wejscie (stary format RGB565+alfa): patrz naglowek tools/gen_weather_icons.py
//   Weryfikacja pixel-po-pikselu starego kontra nowego: tools/verify_weather_icons.py

#include "Moon.h"

namespace wxico {

constexpr int SRC = 64;

'''


def convert(input_path, output_path, dry_run=False):
    text_in = open(input_path, encoding="utf-8").read()
    anchor_pos = text_in.find(ANCHOR)
    if anchor_pos == -1:
        print(f"BLAD: nie znalazlem kotwicy '{ANCHOR}' w {input_path} — "
              "nie wiem gdzie konczy sie sekcja danych.", file=sys.stderr)
        return 1
    # Kotwica jest w linii "enum IconId : uint8_t {...};" — cofnij sie do
    # poczatku TEJ linii, zeby zachowac ja w calosci w ogonie.
    tail_start = text_in.rfind("\n", 0, anchor_pos) + 1
    tail = text_in[tail_start:]

    icons = parse_legacy(input_path)
    if not icons:
        print(f"BLAD: nie znalazlem ikon w starym formacie w {input_path}.", file=sys.stderr)
        return 1

    blocks = []
    total_old = 0
    total_new = 0
    total_pal_bytes = 0
    worst_err = 0
    print(f"{'ikona':9} {'N kolorow':10} {'stary rozmiar':14} {'nowy rozmiar':13} "
          f"{'oszczedność':12} {'max blad (widoczne px)':22}")
    for name in icons:
        rgb, alpha = icons[name]["rgb"], icons[name]["alpha"]
        palette, pix, max_err = build_indexed_icon(rgb, alpha)
        blocks.append(render_icon_block(name, palette, pix, alpha))

        old_size = NPIX * 2 + NPIX * 1
        new_size = len(palette) * 2 + NPIX * 1 + NPIX * 1
        total_old += old_size
        total_new += new_size
        total_pal_bytes += len(palette) * 2
        worst_err = max(worst_err, max_err)
        print(f"{name:9} {len(palette):<10} {old_size:<14} {new_size:<13} "
              f"{old_size - new_size:<12} {max_err}")

    print(f"\nRAZEM (8 ikon): stary format {total_old} B, nowy format {total_new} B, "
          f"oszczednosc {total_old - total_new} B")
    print(f"  z czego palety (PROGMEM): {total_pal_bytes} B; "
          f"PIX+A (PROGMEM): {total_new - total_pal_bytes} B")
    print(f"Najgorszy blad na widocznym pikselu w calym zestawie: {worst_err} "
          f"({'BEZSTRATNIE' if worst_err == 0 else 'UWAGA: NIE jest bezstratne!'})")

    if worst_err != 0:
        print("STOP: format mial byc bezstratny (kazda ikona <=256 kolorow). "
              "Blad != 0 oznacza bledna implementacje generatora, nie akceptowalny "
              "kompromis — nie zapisuje pliku.", file=sys.stderr)
        return 1

    new_text = PREAMBLE + "\n\n".join(blocks) + "\n\n" + tail

    if dry_run:
        print(f"\n(--dry-run: nie zapisuje {output_path})")
        return 0

    Path(output_path).write_text(new_text, encoding="utf-8")
    print(f"\nZapisano {output_path} ({len(new_text)} B tekstu zrodlowego).")
    print("UWAGA: sekcja od 'enum IconId' w dol skopiowana z wejscia BEZ ZMIAN — "
          "wciaz odwoluje sie do starych rgbFor()/ICO_x_RGB. To KROK 3 "
          "(rysowanie), nie ten skrypt — patrz naglowek WeatherUi.cpp / blit().")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    default_path = Path(__file__).resolve().parent.parent / "WeatherIcons.h"
    ap.add_argument("--input", type=Path, default=default_path,
                     help="WeatherIcons.h w STARYM formacie (domyslnie: WeatherIcons.h w korzeniu repo)")
    ap.add_argument("--output", type=Path, default=default_path,
                     help="gdzie zapisac nowy plik (domyslnie: nadpisz --input)")
    ap.add_argument("--dry-run", action="store_true", help="policz i wypisz, nic nie zapisuj")
    args = ap.parse_args()

    if not args.input.exists():
        print(f"Nie znalazlem {args.input}", file=sys.stderr)
        return 2
    return convert(args.input, args.output, dry_run=args.dry_run)


if __name__ == "__main__":
    sys.exit(main())
