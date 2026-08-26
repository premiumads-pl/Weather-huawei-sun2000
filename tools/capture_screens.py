#!/usr/bin/env python3
"""Pobiera zrzuty wszystkich ekranow urzadzenia i sklada docs/screens.gif
oraz contact sheet do README.

Urzadzenie udostepnia:
  POST /api/view?i=N  -> przelacza/przypina ekran (N: 0..13, -1 = powrot do rotacji)
                         (od fw v154 mutacja wymaga POST; GET tylko odczytuje stan)
  GET /api/screen     -> biezacy ekran jako BMP 320x240 24-bit (pobranie ~1 s)

Uzycie:
    python3 tools/capture_screens.py [http://<ip-urzadzenia>]

Domyslny adres to ten z panelu WWW urzadzenia w chwili pisania skryptu;
podaj wlasny adres jako argument, jesli Twoje urzadzenie ma inny IP.

WAZNE: na koniec skrypt ZAWSZE przywraca automatyczna rotacje (i=-1),
nawet jesli po drodze wystapi blad.
"""
import re
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
# (v162) NUMERY 0 I 2 ZNIKLY Z TEJ LISTY, a reszta ZOSTALA NA SWOICH MIEJSCACH.
# Ekrany RETRO (0) i GODZINY (2) zostaly skasowane, ale pozostalych widokow CELOWO
# nie przenumerowano — sloty 0 i 2 sa w Config.h zarezerwowane, bo numer widoku
# wychodzi na zewnatrz przez /api/view. Dziury w numeracji ponizej sa wiec POPRAWNE
# i maja takie zostac; lista ma 13 pozycji (v181: doszedl ZWROT), a VIEW_COUNT wynosi 15.
# Gdyby ktos mimo to przypial i=0 albo i=2, urzadzenie narysuje ekran GLOWNY
# (galaz `default:` w drawV3) — czyli zrzut byby duplikatem "now", nie czernia.
#
# Ekrany pomijane przez ROTACJE (radar bez opadu, dom bez czujnikow BLE, piec bez
# autoryzacji, powietrze bez danych, auto bez swiezej wiadomosci MQTT) i tak daja sie
# PRZYPIAC przez /api/view,
# wiec sa na liscie normalnie. Jesli akurat nie maja danych, zrzut pokaze ich
# stan pusty — i to tez jest prawda o urzadzeniu.
VIEWS = [
    (1, "now", "Teraz", "Now"),
    (3, "radar", "Radar", "Radar"),
    (4, "5days", "5 dni", "5 days"),
    (5, "home", "W domu", "At home"),
    (6, "boiler", "Piec", "Boiler"),
    (7, "pv", "Fotowoltaika", "PV"),
    (8, "flights", "Samoloty", "Flights"),
    (9, "air", "Powietrze", "Air"),
    (10, "mem", "Pamiec", "Memory"),
    (11, "motion", "Ruch", "Motion"),
    # (v174) AUTO weszlo PRZED STATYSTYKAMI (Config.h: static_assert wymaga, zeby
    # VIEW_STATS byl ostatni), wiec STATYSTYKI przesunely sie z 12 na 13. Kto ma
    # zapisane "i=12" poza tym repozytorium, dostanie teraz ekran samochodu.
    # (v181) TO SAMO RAZ JESZCZE: ZWROT (fotowoltaika, ile kosztu instalacji juz
    # wrocilo) wszedl na 12, wiec AUTO jest dzis 13, a STATYSTYKI 14.
    (12, "payback", "Zwrot", "Payback"),
    (13, "auto", "Samochod", "Car"),
    (14, "stats", "Statystyki", "Stats"),
]

# Slug z VIEWS -> nazwa stalej cfg::VIEW_* w Config.h. Sluzy WYLACZNIE weryfikacji
# nizej; kolejnosc i tresc VIEWS pozostaja recznie utrzymywane.
SLUG_TO_CONST = {
    "now": "NOW",
    "radar": "RADAR",
    "5days": "DAYS",
    "home": "HOME",
    "boiler": "BOILER",
    "pv": "PV",
    "flights": "FLIGHTS",
    "air": "AIR",
    "mem": "MEM",
    "motion": "MOTION",
    "payback": "PAYBACK",
    "auto": "AUTO",
    "stats": "STATS",
}

REPO_DIR = Path(__file__).resolve().parent.parent
DOCS_DIR = REPO_DIR / "docs"


def verify_against_config() -> None:
    """Sprawdza, czy VIEWS zgadza sie z cfg::VIEW_* w Config.h — ZANIM cokolwiek
    pobierzemy z urzadzenia.

    Az do v162 ten kontrakt pilnowal wylacznie komentarz ("nic tego nie sprawdza
    automatycznie i nic o tym nie krzyczy") — i raz sie rozjechal: docs/screens.gif
    zostal zlozony z ekranow podpisanych cudzymi nazwami, bo lista tkwila w ukladzie
    sprzed VIEW_RETRO. Teraz krzyczy. Rozjazd przerywa skrypt PRZED przypieciem
    czegokolwiek, wiec nie zostawia urzadzenia w polowie sesji zrzutow.

    Brak Config.h (uruchomienie skryptu spoza repo) NIE jest bledem — wtedy po prostu
    nie ma czego porownac i lecimy dalej z ostrzezeniem.
    """
    cfg_path = REPO_DIR / "Config.h"
    try:
        src = cfg_path.read_text(encoding="utf-8")
    except OSError as e:  # noqa: BLE001
        print(f"UWAGA: nie moge odczytac {cfg_path} ({e}) — pomijam weryfikacje.",
              file=sys.stderr)
        return

    consts = {m.group(1): int(m.group(2))
              for m in re.finditer(r"^constexpr\s+int\s+VIEW_(\w+)\s*=\s*(\d+)\s*;",
                                   src, re.MULTILINE)}
    count = consts.pop("COUNT", None)
    if count is None or not consts:
        raise SystemExit(f"BLAD: nie znalazlem cfg::VIEW_* w {cfg_path} — "
                         "zmienil sie zapis stalych? Popraw ten skrypt.")

    problems: list[str] = []

    # 1) Kazdy wpis VIEWS wskazuje na istniejaca stala i na TEN SAM numer.
    for idx, slug, _pl, _en in VIEWS:
        name = SLUG_TO_CONST.get(slug)
        if name is None:
            problems.append(f"slug '{slug}' nie ma odpowiednika w SLUG_TO_CONST")
        elif name not in consts:
            problems.append(f"cfg::VIEW_{name} (slug '{slug}') nie istnieje juz w Config.h")
        elif consts[name] != idx:
            problems.append(f"slug '{slug}': lista mowi {idx}, "
                            f"a cfg::VIEW_{name} = {consts[name]}")
        if idx >= count:
            problems.append(f"slug '{slug}': numer {idx} >= VIEW_COUNT ({count}) — "
                            "urzadzenie odrzuci go w pinView()")

    # 2) Zaden ZADEKLAROWANY widok nie wypadl po cichu z listy. To jest ta polowa
    #    kontraktu, ktorej brak zabolal poprzednio: dodany do Config.h ekran, o ktorym
    #    skrypt nie wie, po prostu nie trafial do docs/screens.gif.
    listed = {SLUG_TO_CONST.get(slug) for _i, slug, _pl, _en in VIEWS}
    for name, num in sorted(consts.items(), key=lambda kv: kv[1]):
        if name not in listed:
            problems.append(f"cfg::VIEW_{name} = {num} jest w Config.h, "
                            "ale nie ma go w VIEWS — zrzut go pominie")

    if problems:
        raise SystemExit("BLAD: VIEWS rozjechalo sie z Config.h:\n  - "
                         + "\n  - ".join(problems))

    holes = sorted(set(range(count)) - set(consts.values()))
    print(f"VIEWS zgadza sie z Config.h ({len(VIEWS)} ekranow, VIEW_COUNT={count}"
          + (f", sloty zarezerwowane: {holes}" if holes else "") + ")")


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
    # NAJPIERW kontrakt z Config.h, dopiero potem dotykamy urzadzenia — rozjazd ma
    # przerwac skrypt, zanim przypnie pierwszy ekran.
    verify_against_config()
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
