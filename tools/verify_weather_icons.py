#!/usr/bin/env python3
"""WERYFIKACJA (v118, oszczedzanie flasha ikon pogody): rozpakuj STARY i
NOWY format WeatherIcons.h z powrotem do obrazkow i porownaj piksel po
pikselu. Nie ufaj samej kompilacji — to jedyny sposob zeby NAPRAWDE wiedziec,
ze paleta indeksowana daje ten sam obraz co oryginalne RGB565+alfa.

CO SPRAWDZA I CZEGO NIE SPRAWDZA
---------------------------------
Ten skrypt porownuje SUROWE DANE (RGB565 + alfa, 64x64) zrekonstruowane z
obu formatow — to weryfikuje KROK 2 (konwersja danych). NIE uruchamia
C++ (nie ma jak, to nie jest emulator ESP32), wiec NIE weryfikuje KROKU 3
(rysowanie/blit) — tamto trzeba przeczytac w kodzie i/albo sprawdzic na
urzadzeniu. Jesli dane sa identyczne (a bedziesz to widziec ponizej), a
blit() poprawnie odczytuje pal[pix[i]] zamiast rgb[i] bezposrednio, to
razem to daje identyczny wyglad.

Piksele z alfa==0 (tlo, nigdy nie rysowane — patrz premultiply w blit())
MOGA miec inny RGB w nowym formacie (patrz gen_weather_icons.py: dostaja
indeks 0 zamiast oryginalnego koloru). To DZIALANIE ZGODNE Z PROJEKTEM, nie
blad — skrypt liczy je OSOBNO i nie wlicza do "roznic", zeby nie krzyczec
falszywym alarmem o czyms, czego oko nigdy nie zobaczy.

UZYCIE
------
    python3 tools/verify_weather_icons.py
        Porownuje biezacy WeatherIcons.h (nowy format, working tree) z
        wersja z ostatniego commita (git show HEAD:WeatherIcons.h) — czyli
        dokladnie to, co ten skrypt uruchomiony PRZED `git commit` naprawde
        sprawdza: "czy to, co zaraz zacommituje, wyglada tak samo jak to,
        co jest teraz w repo".

    python3 tools/verify_weather_icons.py --old /tmp/WeatherIcons.legacy_backup.h --new WeatherIcons.h
        Jawne sciezki — przydatne gdy stary format juz nie jest w HEAD
        (np. konwersja byla dawno temu i trzeba cofnac sie dalej w historii,
        patrz naglowek gen_weather_icons.py).

Obrazy PNG (stary, nowy, side-by-side) dla kazdej ikony ladują do /tmp/wxico_verify/.
"""
import argparse
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from weathericons_common import NPIX, SRC, parse_auto, rgb565_to_rgb888  # noqa: E402

try:
    from PIL import Image
except ImportError:
    print("Ten skrypt potrzebuje Pillow: pip install pillow", file=sys.stderr)
    sys.exit(2)


def load_from_git_head(repo_root, rel_path, tmp_dir):
    """git show HEAD:<rel_path> -> plik tymczasowy (stary format sprzed edycji w working tree)."""
    out = subprocess.run(
        ["git", "-C", str(repo_root), "show", f"HEAD:{rel_path}"],
        capture_output=True, text=True, check=True,
    ).stdout
    p = tmp_dir / "WeatherIcons.HEAD.h"
    p.write_text(out, encoding="utf-8")
    return p


def icon_to_rgba(rgb, alpha):
    img = Image.new("RGBA", (SRC, SRC))
    px = img.load()
    for i in range(NPIX):
        x, y = i % SRC, i // SRC
        r, g, b = rgb565_to_rgb888(rgb[i])
        px[x, y] = (r, g, b, alpha[i])
    return img


def compare_icon(name, old, new, out_dir):
    rgb_o, a_o = old["rgb"], old["alpha"]
    rgb_n, a_n = new["rgb"], new["alpha"]

    alpha_diffs = sum(1 for i in range(NPIX) if a_o[i] != a_n[i])
    visible_rgb_diffs = 0
    invisible_rgb_diffs = 0
    max_channel_err = 0
    for i in range(NPIX):
        if rgb_o[i] == rgb_n[i]:
            continue
        if a_o[i] > 0:
            visible_rgb_diffs += 1
            ro = rgb565_to_rgb888(rgb_o[i])
            rn = rgb565_to_rgb888(rgb_n[i])
            max_channel_err = max(max_channel_err, max(abs(a - b) for a, b in zip(ro, rn)))
        else:
            invisible_rgb_diffs += 1

    img_old = icon_to_rgba(rgb_o, a_o)
    img_new = icon_to_rgba(rgb_n, a_n)
    SCALE = 4
    img_old_big = img_old.resize((SRC * SCALE, SRC * SCALE), Image.NEAREST)
    img_new_big = img_new.resize((SRC * SCALE, SRC * SCALE), Image.NEAREST)

    # Checkerboard pod spod, zeby przezroczystosc bylo widac w zwyklej przegladarce PNG.
    def over_checker(im):
        cb = Image.new("RGB", im.size)
        cs = 8
        for y in range(0, im.size[1], cs):
            for x in range(0, im.size[0], cs):
                c = (200, 200, 200) if ((x // cs) + (y // cs)) % 2 == 0 else (150, 150, 150)
                cb.paste(c, (x, y, x + cs, y + cs))
        return Image.alpha_composite(cb.convert("RGBA"), im).convert("RGB")

    side = Image.new("RGB", (SRC * SCALE * 2 + 20, SRC * SCALE + 30), (30, 30, 30))
    side.paste(over_checker(img_old_big), (0, 30))
    side.paste(over_checker(img_new_big), (SRC * SCALE + 20, 30))
    side.save(out_dir / f"{name}_side_by_side.png")
    img_old.save(out_dir / f"{name}_old.png")
    img_new.save(out_dir / f"{name}_new.png")

    return {
        "alpha_diffs": alpha_diffs,
        "visible_rgb_diffs": visible_rgb_diffs,
        "invisible_rgb_diffs": invisible_rgb_diffs,
        "max_channel_err": max_channel_err,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    repo_root = Path(__file__).resolve().parent.parent
    ap.add_argument("--old", type=Path, default=None,
                     help="stary WeatherIcons.h (domyslnie: git show HEAD:WeatherIcons.h)")
    ap.add_argument("--new", type=Path, default=repo_root / "WeatherIcons.h",
                     help="nowy WeatherIcons.h (domyslnie: working tree)")
    ap.add_argument("--out-dir", type=Path, default=Path("/tmp/wxico_verify"),
                     help="gdzie zapisac PNG-i (domyslnie /tmp/wxico_verify)")
    args = ap.parse_args()

    args.out_dir.mkdir(parents=True, exist_ok=True)

    old_path = args.old
    if old_path is None:
        old_path = load_from_git_head(repo_root, "WeatherIcons.h", args.out_dir)
        print(f"--old nie podane -> wziete z git show HEAD:WeatherIcons.h ({old_path})")

    old_icons = parse_auto(old_path)
    new_icons = parse_auto(args.new)

    common = [n for n in old_icons if n in new_icons]
    missing_old = [n for n in new_icons if n not in old_icons]
    missing_new = [n for n in old_icons if n not in new_icons]
    if missing_old:
        print(f"UWAGA: ikony w nowym ale nie w starym: {missing_old}")
    if missing_new:
        print(f"UWAGA: ikony w starym ale nie w nowym: {missing_new}")

    print(f"\nPorownuje: stary={old_path}  nowy={args.new}")
    print(f"{'ikona':9} {'roznice widoczne (px)':22} {'max blad/kanal':15} "
          f"{'roznice alfy (px)':18} {'roznice w tle, ignorowane':26}")

    any_visible_diff = False
    any_alpha_diff = False
    for name in common:
        r = compare_icon(name, old_icons[name], new_icons[name], args.out_dir)
        print(f"{name:9} {r['visible_rgb_diffs']:<22} {r['max_channel_err']:<15} "
              f"{r['alpha_diffs']:<18} {r['invisible_rgb_diffs']:<26}")
        if r["visible_rgb_diffs"] > 0:
            any_visible_diff = True
        if r["alpha_diffs"] > 0:
            any_alpha_diff = True

    print(f"\nPNG-i (stary, nowy, side-by-side x4) zapisane w {args.out_dir}/")

    if any_visible_diff or any_alpha_diff:
        print("\nBLAD: sa roznice na WIDOCZNYCH pikselach albo w alfie — "
              "wyglad NIE jest identyczny. Zobacz PNG-i wyzej.", file=sys.stderr)
        return 1

    print("\nOK: wszystkie ikony identyczne piksel-po-pikselu na kazdym widocznym "
          "pikselu (RGB) i w calej alfie. Roznice w kolorze tla pod alfa==0 (jesli "
          "sa) sa oczekiwane i nie wplywaja na wyglad — patrz naglowek tego skryptu.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
