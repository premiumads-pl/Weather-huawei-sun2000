#!/usr/bin/env python3
"""Pobiera zrzuty wszystkich ekranow urzadzenia i sklada docs/screens.gif
oraz contact sheet do README.

Urzadzenie udostepnia:
  POST /api/view?i=N  -> przelacza/przypina ekran (N: 0..12, -1 = powrot do rotacji)
                         (od fw v154 mutacja wymaga POST; GET tylko odczytuje stan)
  GET /api/screen     -> biezacy ekran jako BMP 320x240 24-bit (pobranie ~1 s)

Uzycie:
    python3 tools/capture_screens.py [http://<ip-urzadzenia>]

Domyslny adres to ten z panelu WWW urzadzenia w chwili pisania skryptu;
podaj wlasny adres jako argument, jesli Twoje urzadzenie ma inny IP.

WAZNE: na koniec skrypt ZAWSZE przywraca automatyczna rotacje (i=-1),
nawet jesli po drodze wystapi blad.
"""
import sys
import time
import urllib.request
from io import BytesIO
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

DEFAULT_BASE = "http://192.168.40.116"
SCALE = 2
ENTER_ANIM_WAIT_S = 2.0
SCREEN_FETCH_TIMEOUT_S = 5
GIF_FRAME_MS = 2500

# (index, slug, etykieta PL, etykieta EN) — kolejnosc jak w rotacji urzadzenia.
#
# !!! TE NUMERY SA KONTRAKTEM Z Config.h (cfg::VIEW_*) !!!
# Trafiaja wprost do POST /api/view?i=N, a urzadzenie interpretuje je przez
# cfg::VIEW_* — zrodlem prawdy jest Config.h, NIE ta lista. Przy dodaniu,
# usunieciu albo przenumerowaniu ekranu trzeba poprawic OBA miejsca; nic tego
# nie sprawdza automatycznie i nic o tym nie krzyczy — skrypt po prostu
# pobierze i podpisze nie te ekrany.
#
# Dokladnie to sie stalo: lista stala na ukladzie sprzed wprowadzenia VIEW_RETRO
# (0..5: now/hours/5days/pv/flights/stats), a Config.h ma dzis VIEW_COUNT = 13
# i wszystko przesuniete o +1 wzgledem tamtego ukladu. Sprawdzone na zywo
# 16.08.2026: przypiecie i=2 daje ekran GODZINY, a nie "5 dni". docs/screens.gif
# byl przez to skladany z ekranow podpisanych cudzymi nazwami.
#
# Ekrany pomijane przez ROTACJE (radar bez opadu, dom bez czujnikow BLE, piec bez
# autoryzacji, powietrze bez danych) i tak daja sie PRZYPIAC przez /api/view,
# wiec sa na liscie normalnie. Jesli akurat nie maja danych, zrzut pokaze ich
# stan pusty — i to tez jest prawda o urzadzeniu.
VIEWS = [
    (0, "retro", "Retro", "Retro"),
    (1, "now", "Teraz", "Now"),
    (2, "hours", "Godziny", "Hours"),
    (3, "radar", "Radar", "Radar"),
    (4, "5days", "5 dni", "5 days"),
    (5, "home", "W domu", "At home"),
    (6, "boiler", "Piec", "Boiler"),
    (7, "pv", "Fotowoltaika", "PV"),
    (8, "flights", "Samoloty", "Flights"),
    (9, "air", "Powietrze", "Air"),
    (10, "mem", "Pamiec", "Memory"),
    (11, "motion", "Ruch", "Motion"),
    (12, "stats", "Statystyki", "Stats"),
]

DOCS_DIR = Path(__file__).resolve().parent.parent / "docs"


def http_get(url: str, timeout: float) -> bytes:
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return r.read()


def pin_view(base: str, i: int) -> None:
    # POST, nie GET: od fw v154 /api/view MUTUJE (przypina ekran) tylko przy metodzie
    # POST — GET zostawiono jako czysty odczyt stanu, zeby obca strona nie mogla przez
    # <img src=".../api/view?i=3"> przestawiac ekranu (CSRF). Puste cialo wystarcza.
    req = urllib.request.Request(f"{base}/api/view?i={i}", data=b"", method="POST")
    with urllib.request.urlopen(req, timeout=5) as r:
        body = r.read()
    print(f"  view={i} -> {body.decode('utf-8', 'replace')}")


def fetch_screen(base: str) -> Image.Image:
    raw = http_get(f"{base}/api/screen", timeout=SCREEN_FETCH_TIMEOUT_S)
    return Image.open(BytesIO(raw)).convert("RGB")


def upscale(im: Image.Image, factor: int = SCALE) -> Image.Image:
    return im.resize((im.width * factor, im.height * factor), Image.NEAREST)


def label_font(size: int):
    try:
        return ImageFont.load_default(size=size)
    except TypeError:
        # starsze Pillow bez parametru size
        return ImageFont.load_default()


def build_contact_sheet(frames: list[tuple[str, Image.Image]], out_path: Path) -> None:
    # DRUGIE zalozenie oparte na starej liscie szesciu ekranow: siatka byla zaszyta
    # jako 3 x 2 = dokladnie 6 kratek. Przy 13 ekranach plotno wychodziloby na
    # dwa rzedy, a Image.paste() nie rosnie — po prostu przycina to, co wystaje,
    # wiec siedem ostatnich ekranow zniknieloby bez slowa ostrzezenia.
    # Liczba rzedow jest teraz LICZONA z liczby zrzutow.
    cols = 4
    rows = (len(frames) + cols - 1) // cols

    # Kratki w natywnej rozdzielczosci 320x240, a nie w powiekszonej 2x uzywanej
    # do GIF-a: przy 13 ekranach arkusz 4 x 640 px mialby ~2600 px szerokosci,
    # czyli byloby to zdecydowanie za duzo jak na obrazek osadzony w README.
    # Zmniejszenie NEAREST z powiekszenia NEAREST oddaje dokladnie oryginal.
    pad = 14
    cap_h = 34
    fw, fh = frames[0][1].size
    w, h = fw // SCALE, fh // SCALE
    sheet_w = cols * w + (cols + 1) * pad
    sheet_h = rows * (h + cap_h) + (rows + 1) * pad
    sheet = Image.new("RGB", (sheet_w, sheet_h), (10, 14, 22))
    draw = ImageDraw.Draw(sheet)
    font = label_font(16)

    for idx, (label, im) in enumerate(frames):
        col, row = idx % cols, idx // cols
        x = pad + col * (w + pad)
        y = pad + row * (h + cap_h + pad)
        sheet.paste(im.resize((w, h), Image.NEAREST), (x, y))
        tw = draw.textlength(label, font=font)
        draw.text((x + (w - tw) / 2, y + h + 6), label, fill=(210, 225, 240), font=font)

    sheet.save(out_path)
    print(f"zapisano {out_path} ({sheet_w}x{sheet_h}, siatka {cols}x{rows})")


def main() -> int:
    base = sys.argv[1].rstrip("/") if len(sys.argv) > 1 else DEFAULT_BASE
    DOCS_DIR.mkdir(parents=True, exist_ok=True)
    print(f"Urzadzenie: {base}")

    shots: list[tuple[str, Image.Image]] = []
    try:
        for i, slug, label_pl, label_en in VIEWS:
            print(f"[{i}] {label_pl} ({label_en})")
            pin_view(base, i)
            time.sleep(ENTER_ANIM_WAIT_S)
            im = upscale(fetch_screen(base))
            out = DOCS_DIR / f"screen-{i}-{slug}.png"
            im.save(out)
            print(f"  zapisano {out} ({im.size[0]}x{im.size[1]})")
            shots.append((f"{i}. {label_en}", im))
    finally:
        print("Przywracam automatyczna rotacje (i=-1)...")
        try:
            pin_view(base, -1)
        except Exception as e:  # noqa: BLE001
            print(f"  UWAGA: nie udalo sie przywrocic rotacji: {e}", file=sys.stderr)

    if not shots:
        print("Brak zrzutow — przerywam.", file=sys.stderr)
        return 1

    gif_path = DOCS_DIR / "screens.gif"
    frames = [im for _, im in shots]
    frames[0].save(
        gif_path,
        save_all=True,
        append_images=frames[1:],
        duration=GIF_FRAME_MS,
        loop=0,
        optimize=True,
    )
    print(f"zapisano {gif_path}")

    build_contact_sheet(shots, DOCS_DIR / "screens-contact-sheet.png")
    print("Gotowe.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
