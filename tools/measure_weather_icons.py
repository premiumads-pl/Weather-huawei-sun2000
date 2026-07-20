#!/usr/bin/env python3
"""KROK 1 (v118, oszczedzanie flasha ikon pogody): ZMIERZ zanim projektujesz format.

PO CO TEN PLIK ISTNIEJE
------------------------
WeatherIcons.h trzyma 8 ikon pogody jako pelne bitmapy RGB565 + alfa
(64x64, 12 288 B/ikone, ~98 kB lacznie). Zanim ktokolwiek przepisze to na
palete indeksowana, trzeba wiedziec NAPRAWDE ile unikalnych kolorow i
poziomow alfy jest w danych — a nie zakladac "kilkanascie barw" na oko.
Ikony maja antyaliasing (naglowek WeatherIcons.h: "supersampling 8x"),
wiec krawedzie moga (ale nie musza) generowac setki posrednich odcieni.

Ten skrypt tylko CZYTA i LICZY. Nic nie zapisuje, nic nie generuje —
decyzja o formacie (gen_weather_icons.py) ma sie opierac na jego wyniku,
nie na zalozeniach z gory.

UZYCIE
------
    python3 tools/measure_weather_icons.py [WeatherIcons.h]

Bez argumentu szuka WeatherIcons.h w katalogu nadrzednym wzgledem tools/
(czyli w korzeniu repo — dziala z dowolnego miejsca).

Interpretacja kolumn w tabeli — patrz funkcja print_report() nizej.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from weathericons_common import ICON_ORDER, NPIX, parse_legacy, rgb565_to_rgb888  # noqa: E402


def analyze_icon(rgb, alpha):
    """Liczy statystyki jednej ikony. rgb/alpha to listy dlugosci NPIX."""
    visible_idx = [i for i in range(NPIX) if alpha[i] > 0]
    colors_all = set(rgb)
    colors_visible = set(rgb[i] for i in visible_idx)
    alphas = set(alpha)
    edge_alphas = sorted(a for a in alphas if 0 < a < 255)

    return {
        "n_visible_px": len(visible_idx),
        "n_colors_all": len(colors_all),
        "n_colors_visible": len(colors_visible),
        "n_alpha_levels": len(alphas),
        "alpha_binary": alphas <= {0, 255},
        "n_edge_alpha_levels": len(edge_alphas),
        "alpha_min": min(alphas),
        "alpha_max": max(alphas),
    }


def recommend(max_colors_visible, all_alpha_binary):
    """Drzewo decyzyjne z briefu, zastosowane do ZMIERZONYCH liczb."""
    if max_colors_visible <= 16 and all_alpha_binary:
        return ("4 bity indeksu + 1 bit alfy (~0.6 B/px)",
                "NIE dotyczy — alfa jest binarna w kazdej ikonie")
    if max_colors_visible <= 16:
        return ("4 bity indeksu + 4 bity alfy = 1 B/px",
                "alfa kwantyzowana z 256 do 16 poziomow — sprawdz blad w KROK 2")
    if max_colors_visible <= 256:
        return ("8 bitow indeksu + osobna alfa 8-bit = 2 B/px",
                "kolor bez strat (<=256 unikalnych), alfa bez zmian")
    return ("kolorow WIECEJ NIZ 256 w co najmniej jednej ikonie — "
            "8-bit indeks wymaga kwantyzacji kolorow, zmierz blad w KROK 2 "
            "i porownaj z kryterium 'wyglad identyczny' zanim zdecydujesz",
            "alfa bez zmian (8-bit)")


def print_report(path):
    icons = parse_legacy(path)
    if not icons:
        print(f"BRAK ikon w formacie legacy w {path} — nic do zmierzenia.")
        return 1

    print(f"Zrodlo: {path}")
    print(f"Ikony: {len(icons)}, kazda {NPIX} pikseli (64x64)\n")

    rows = []
    for name in icons:
        stats = analyze_icon(icons[name]["rgb"], icons[name]["alpha"])
        rows.append((name, stats))

    # --- tabela glowna ---------------------------------------------------
    hdr = f"{'ikona':9} {'kolory (widoczne)':18} {'kolory (wszystkie)':19} " \
          f"{'poziomy alfy':13} {'alfa binarna?':14} {'zakres alfy':12}"
    print(hdr)
    print("-" * len(hdr))
    for name, s in rows:
        print(f"{name:9} {s['n_colors_visible']:<18} {s['n_colors_all']:<19} "
              f"{s['n_alpha_levels']:<13} {str(s['alpha_binary']):<14} "
              f"{s['alpha_min']}-{s['alpha_max']}")

    print()
    max_colors_visible = max(s["n_colors_visible"] for _, s in rows)
    max_colors_all = max(s["n_colors_all"] for _, s in rows)
    all_alpha_binary = all(s["alpha_binary"] for _, s in rows)
    total_edge_alpha = sum(s["n_edge_alpha_levels"] for _, s in rows)

    print(f"Maks. kolorow (tylko widoczne piksele, alfa>0): {max_colors_visible}")
    print(f"Maks. kolorow (wliczajac tlo, alfa==0):         {max_colors_all}")
    print(f"Alfa binarna (0/255) we WSZYSTKICH ikonach:     {all_alpha_binary}")
    print(f"Suma poziomow alfy 1..254 (krawedzie AA) w calym zestawie: {total_edge_alpha}")
    if not all_alpha_binary:
        print("  -> antyaliasing na krawedziach jest realny (nie tylko w RGB, "
              "tez w alfie) — plynne krawedzie, nie schodki.")

    fmt, note = recommend(max_colors_visible, all_alpha_binary)
    print(f"\nRekomendacja formatu wg zmierzonych liczb: {fmt}")
    print(f"Uwaga: {note}")

    # --- unia kolorow: czy jedna WSPOLNA paleta miedzy ikonami ma sens? --
    union_visible = set()
    for name in icons:
        rgb, alpha = icons[name]["rgb"], icons[name]["alpha"]
        union_visible.update(rgb[i] for i in range(NPIX) if alpha[i] > 0)
    print(f"\nUnia widocznych kolorow ZE WSZYSTKICH 8 ikon razem: {len(union_visible)}")
    if len(union_visible) <= 256:
        print("  -> miesci sie w 256 -> JEDNA wspolna paleta 8-bit dla wszystkich "
              "ikon jest mozliwa (mniej RAM niz 8 osobnych palet).")
    else:
        print("  -> nie miesci sie w jednej palecie 256 kolorow -> palety per-ikona.")

    # --- kilka najczestszych kolorow na probke (podglad, nie decyzja) ---
    print("\nPodglad: 6 najczestszych widocznych kolorow na ikone (RGB565 -> RGB888, # pikseli)")
    for name, s in rows:
        rgb, alpha = icons[name]["rgb"], icons[name]["alpha"]
        counts = {}
        for i in range(NPIX):
            if alpha[i] > 0:
                counts[rgb[i]] = counts.get(rgb[i], 0) + 1
        top = sorted(counts.items(), key=lambda kv: -kv[1])[:6]
        parts = [f"0x{c:04X}{rgb565_to_rgb888(c)}x{n}" for c, n in top]
        print(f"  {name:9} " + "  ".join(parts))

    return 0


if __name__ == "__main__":
    default = Path(__file__).resolve().parent.parent / "WeatherIcons.h"
    p = Path(sys.argv[1]) if len(sys.argv) > 1 else default
    if not p.exists():
        print(f"Nie znalazlem {p}", file=sys.stderr)
        sys.exit(2)
    sys.exit(print_report(p))
