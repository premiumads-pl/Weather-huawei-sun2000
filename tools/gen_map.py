#!/usr/bin/env python3
"""Generator rastra ladu (skanliniowe "spany") dla MapData*.h.

PO CO TEN PLIK ISTNIEJE
------------------------
MapDataWide.h zaczyna sie od "Wygenerowane automatycznie (Natural Earth 10m
land, rasteryzacja skanliniowa)" - ale generatora nigdy nie bylo w repo. Powstal
kiedys w brudnopisie sesji i przepadl. Zostaly same liczby: dane wyprowadzone
bez mozliwosci odtworzenia, czyli dokladnie ten rodzaj bledu, ktory audyt
zrodel prawdy (audyt/A-zrodla-prawdy.md) opisuje dla innych czesci repo.

Ten skrypt jest naprawa: kazdy naglowek, ktory produkuje, zaczyna sie od
komentarza z DOKLADNA komenda, ktora go zrobila (patrz format_header() nizej),
wiec ten sam problem nie moze sie powtorzyc po cichu.

DANE WEJSCIOWE
--------------
Natural Earth, warstwa "10m Physical Vectors -> Land" (ne_10m_land), shapefile,
domena publiczna (CC0, bez wymogu atrybucji):

    strona:    https://www.naturalearthdata.com/downloads/10m-physical-vectors/10m-land/
    plik zip:  https://naciscdn.org/naturalearth/10m/physical/ne_10m_land.zip

Rozpakuj tak, zeby .shp/.shx/.dbf/.prj lezaly razem obok siebie, np.:

    tools/ne_10m_land/ne_10m_land.shp   <- domyslna sciezka (patrz DEFAULT_SHAPEFILE)

Ten katalog CELOWO nie wchodzi do repo (patrz .gitignore) - to 7 MB danych
wektorowych, ktore kazdy moze pobrac sam, w minute.

NIE PODMIENIAC na 110m ani 50m "dla wygody" - przy 110m znika Polwysep Helski,
czyli cecha definiujaca Zatoke Gdanska na tej mapie. To udokumentowany blad
danych wejsciowych, nie skrot warty rozwazenia.

ZALEZNOSCI
----------
    pip install pyshp numpy

Celowo bez geopandas/shapely/gdal: land to jedna warstwa bez atrybutow
istotnych dla rasteryzacji (patrz load_edges()), wiec caly ciezki stack
geoprzestrzenny bylby tu balastem, nie narzedziem.

UZYCIE
------
1) Odtworzenie/weryfikacja istniejacej mapy WIDE (test poprawnosci generatora -
   patrz --verify-against nizej i sekcja PROJEKCJA):

    python3 tools/gen_map.py \\
        --lat-min 54.30 --lat-max 54.84 --lon-min 17.735 --lon-max 19.465 \\
        --width 320 --height 172 --namespace gmapw \\
        --out /tmp/MapDataWide_check.h \\
        --verify-against MapDataWide.h

2) Nowa mapa radarowa (~300 km w poziomie, ta sama skala m/px w obu osiach,
   wyliczona z centrum + szerokosci w km - patrz compute_isotropic_bounds()):

    python3 tools/gen_map.py \\
        --center-lat 54.57 --center-lon 18.60 --width-km 300 \\
        --width 320 --height 172 --namespace gmapr \\
        --out MapDataRadar.h

Domyslna sciezka shapefile'a to tools/ne_10m_land/ne_10m_land.shp; inna -
przez --shapefile.

PROJEKCJA - I DLACZEGO TO WAZNE (Mercator kontra rownoprostokatna)
-------------------------------------------------------------------
Ten generator - tak jak rysowanie w WeatherUi.cpp (`x = (lon-LON_MIN)/(LON_MAX-
LON_MIN)*MAP_W`, `y = (LAT_MAX-lat)/(LAT_MAX-LAT_MIN)*MAP_H`) - uzywa NAJPROSTSZEGO
rzutowania rownoprostokatnego: x liniowo z dlugosci geogr., y liniowo z szerokosci
geogr. Skala pozioma i pionowa jest dobierana RAZ, przy wyborze granic (patrz
compute_isotropic_bounds: dlugosc geogr. mnozy sie przez cos(lat_srodka), zeby
"metry na piksel" wyszly takie same w obu osiach) - sam wzor projekcji nie zna
zadnego cos(lat) per piksel.

RainViewer serwuje kafelki w Web Mercatorze, gdzie y jest NIELINIOWA funkcja
szerokosci geograficznej (y = (1 - asinh(tan(lat))/pi)/2). RadarMap.cpp juz dzis
"oszukuje": liczy Mercatorowy y tylko na GORNEJ i DOLNEJ krawedzi naszej mapy
(computeGeometry()), a potem miedzy nimi interpoluje LINIOWO po numerze wiersza
(resample()) - czyli udaje, ze Mercator jest na tym odcinku linia prosta.

Na 60 km wysokosci (0,54 st. szerokosci - obecna gmapw) blad tego uproszczenia
policzony numerycznie to ok. 0,28 piksela naszej mapy - niewidoczne. Przy mapie
radaru z tego skryptu (domyslne --center-lat 54.57 --width-km 300, MAP_H=172,
co daje ok. 161 km WYSOKOSCI, bo szerokosc i wysokosc mapy to nie to samo -
patrz nizej) blad rosnie do ok. 0,76 piksela - dalej PONIZEJ 1 piksela, czyli
wciaz bezpieczne. Gdyby ktos kiedys zrobil mape 300 km WYSOKOSCI (nie tylko
szerokosci), blad urosnie do ok. 1,4 piksela - to juz widoczny rozjazd
wybrzeza z opadem i wtedy TA metoda (liniowa interpolacja Mercatora w
RadarMap.cpp) przestaje wystarczac; trzeba by albo generowac raster ladu
wprost w Mercatorze, albo reprojektowac kafelek na siatke rownoprostokatna
przed rysowaniem. Oba te wnioski i liczby sa wyprowadzone i sprawdzone -
metoda w scratchu tej sesji, nie zgadywane - patrz raport w opisie zadania.

WAZNE: os DLUGOSCI GEOGRAFICZNEJ (x) nie ma tego problemu NIGDY, przy zadnej
szerokosci mapy - w Web Mercatorze x jest ZAWSZE dokladnie liniowe wzgledem
dlugosci geogr. Caly blad opisany wyzej jest zjawiskiem wylacznie pionowym
(szerokosc geogr.) i zalezy od ROZPIETOSCI W STOPNIACH SZEROKOSCI (czyli od
wysokosci mapy w km), nie od jej szerokosci w km ani od zoomu kafelka
RainViewera (sprawdzone: blad w NASZYCH pikselach wychodzi taki sam dla
zoom 6 i zoom 7 - zoom zmienia tylko rozdzielczosc zrodla, nie krzywizne).

Ten skrypt generuje TYLKO raster ladu - NIE rusza RadarMap.cpp ani sposobu,
w jaki radar jest dzis nakladany na mapy. To celowe (patrz opis zadania:
"Nie ruszac MapDataWide.h ani kodu rysujacego") - podmiana i ewentualna
poprawka projekcji radaru to osobne zadanie z BACKLOG-u.

FORMAT WYJSCIOWY (kontrakt z WeatherUi.cpp - PRZECZYTANY, nie zgadywany)
--------------------------------------------------------------------------
LAND_SPANS[i] = {x0, x1}: piksele x0..x1 WLACZNIE sa ladem w danym wierszu
(WeatherUi.cpp rysuje je jako `drawFastHLine(x0, row, x1-x0+1, ...)`, wiec
x1-x0+1 musi byc szerokoscia, nie odstepem). Wiersz moze miec 0, 1 lub wiecej
spanow (rozlaczne fragmenty ladu w tym samym wierszu, np. polwysep + material
staly za zatoka) - sa wypisane od lewej do prawej. LAND_ROW_OFF[row]..
LAND_ROW_OFF[row+1] to zakres indeksow w LAND_SPANS dla danego wiersza (klasyczny
prefix-sum/CSR); wiersz bez ladu ma LAND_ROW_OFF[row] == LAND_ROW_OFF[row+1] (zero
spanow) - petla rysujaca w WeatherUi.cpp obsluguje to poprawnie (zero przebiegow).

CZY TO ODTWARZA MapDataWide.h BIT W BIT? NIE DO KONCA - I TO JEST SWIADOME
----------------------------------------------------------------------------
Ten skrypt reprodukuje dzisiejszy MapDataWide.h (--verify-against) na ok. 90
z 172 wierszy (52%) DOKLADNIE co do piksela; reszta rozjezdza sie o 1-3 piksele
LACZNIE (obie krawedzie razem), skupione niemal wylacznie w dwoch miejscach:
czubku Polwyspu Helskiego (wiersze 0-17) i ujsciu Wisly/Zulawach (wiersze
~150-157) - tam, gdzie linia brzegowa biegnie niemal ROWNOLEGLE do skanlinii,
wiec kazda konwencja zaokraglania na poziomie pojedynczego piksela jest
niestabilna (submilimetrowa roznica we wspolrzednej wierzcholka przesuwa
wynik o cala kolumne). Sprawdzilem systematycznie: przesuniecie wiersza
(gora/srodek/dol), 6 wariantow zaokraglania kazdej krawedzi (floor/ceil/round
x2), globalne przesuniecie ukladu wspolrzednych o -0,6..+0,6 piksela i
nadprobkowanie w pionie (2x-16x) - najlepszy z ~50 przetestowanych wariantow
jest ten, ktory jest tu zaszyty (patrz pixel_x0()/pixel_x1() nizej). Zaden
wariant nie trafia 260/260 idealnie. Wniosek: brakujacy oryginalny skrypt
najpewniej uzywal innej, drobnej konwencji zaokraglania podpikselowego (albo
lekko innej wersji danych Natural Earth) niz cokolwiek dajace sie odgadnac
z samych liczb - i TO WLASNIE jest cena utraty zrodla prawdy, ktora ten
skrypt ma na przyszlosc wyeliminowac. Roznica jest kosmetyczna (pojedyncze
piksele przy samej granicy ladu), nie topologiczna - ksztalt zatoki, polwysep
i wszystkie kanaly sa tam, gdzie byly.
"""

import argparse
import math
import re
import sys
from pathlib import Path

import numpy as np

try:
    import shapefile  # pyshp
except ImportError:
    sys.exit(
        "Brak pakietu 'pyshp'. Instalacja: pip install pyshp numpy\n"
        "(numpy jest tez wymagany, ale zwykle jest juz w systemie)"
    )


# Standardowe sferyczne przyblizenie dlugosci stopnia szerokosci geogr.
# (dokladna wartosc WGS84 zmienia sie z szerokoscia geogr. o ulamek procenta -
# bez znaczenia tutaj, bo i tak liczymy "izotropowosc" jako STOSUNEK, a on
# jest niezalezny od tej stalej; patrz compute_isotropic_bounds).
KM_PER_DEG_LAT = 111.32

SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_SHAPEFILE = SCRIPT_DIR / "ne_10m_land" / "ne_10m_land.shp"


# --------------------------------------------------------------------------
# Wczytanie danych wektorowych
# --------------------------------------------------------------------------

def load_edges(shp_path, lat_lo, lat_hi, lon_lo, lon_hi):
    """Zwraca krawedzie (lat1, lon1, lat2, lon2) wszystkich pierscieni
    (obwodow wielokatow), ktorych bbox przecina okno [lat_lo,lat_hi] x
    [lon_lo,lon_hi].

    Filtr jest po bboxie PIERSCIENIA, nie po tym, czy jego PUNKTY leza w
    oknie - kontynent (np. Eurazja) ma jeden pierscien od Portugalii po
    Kamczatke, wiec jego bbox zawsze przecina kazde nasze okno w Europie, a
    to jest WYMAGANE dla poprawnosci: reguly parzystosci (even-odd) przy
    liczeniu przeciec potrzebuje kompletnego pierscienia, nie jego wycinka -
    przyciecie do samej dlugosci geogr. okna zgubiloby informacje o tym, ile
    razy linia brzegowa przecina rownoleznik PRZED dotarciem do naszego okna,
    czyli zepsuloby parzystosc lad/woda w calym oknie.
    """
    r = shapefile.Reader(str(shp_path))
    lat1_all, lon1_all, lat2_all, lon2_all = [], [], [], []

    for shp in r.shapes():
        pts = shp.points
        parts = list(shp.parts) + [len(pts)]
        for i in range(len(parts) - 1):
            ring = pts[parts[i]:parts[i + 1]]
            if len(ring) < 3:
                continue  # zdegenerowany obwod (linia, nie wielokat) - pomijamy
            arr = np.asarray(ring, dtype=np.float64)
            lon, lat = arr[:, 0], arr[:, 1]
            if (lon.max() < lon_lo or lon.min() > lon_hi or
                    lat.max() < lat_lo or lat.min() > lat_hi):
                continue

            # Shapefile Polygon rings SA zamkniete z definicji (spec ESRI), ale
            # dopinamy na wszelki wypadek - inaczej ostatnia krawedz obwodu by
            # zgubila i parzystosc przeciec bylaby przypadkowa.
            if lon[0] != lon[-1] or lat[0] != lat[-1]:
                lon = np.append(lon, lon[0])
                lat = np.append(lat, lat[0])

            lat1_all.append(lat[:-1]); lon1_all.append(lon[:-1])
            lat2_all.append(lat[1:]);  lon2_all.append(lon[1:])

    if not lat1_all:
        sys.exit(
            f"Brak ladu w oknie lat {lat_lo}..{lat_hi}, lon {lon_lo}..{lon_hi} - "
            "sprawdz, czy granice sa w stopniach dziesietnych i we wlasciwej kolejnosci."
        )

    return (np.concatenate(lat1_all), np.concatenate(lon1_all),
            np.concatenate(lat2_all), np.concatenate(lon2_all))


# --------------------------------------------------------------------------
# Rasteryzacja skanliniowa
# --------------------------------------------------------------------------

def row_crossings(edges, lat_row):
    """Wspolrzedne (dlugosc geogr.) przeciec linii brzegowej ze skanlinia na
    danej szerokosci geogr., posortowane rosnaco. Parzysta liczba (kazdy
    zamkniety obwod przecina kazda skanlinie parzysta liczbe razy) - pary
    (0,1), (2,3), ... sa odcinkami LADU (regula parzystosci/even-odd, ta sama,
    ktorej uzywaja wszystkie standardowe wypelnienia wielokatow z dziurami)."""
    lat1, lon1, lat2, lon2 = edges
    mask = (lat1 <= lat_row) != (lat2 <= lat_row)
    cross = lon1[mask] + (lat_row - lat1[mask]) / (lat2[mask] - lat1[mask]) * (
        lon2[mask] - lon1[mask])
    cross.sort()
    return cross


def pixel_x0(raw):
    """Pierwszy piksel ladu na wejsciu w lad. Empirycznie najlepiej pasujacy
    wariant (patrz docstring modulu, sekcja o odtwarzaniu bit w bit): piksel
    NAJBLIZSZY punktowi wejscia, nie pierwszy piksel z jakimkolwiek pokryciem."""
    return math.floor(raw + 0.5)


def pixel_x1(raw):
    """Ostatni piksel ladu na wyjsciu z ladu. Empirycznie najlepiej pasujacy
    wariant: piksel ZAWIERAJACY punkt wyjscia (dowolne, nawet czesciowe,
    pokrycie sie liczy) - w przeciwienstwie do pixel_x0 powyzej. Ta asymetria
    nie ma czystego geometrycznego uzasadnienia - to jest wynik przeszukania,
    nie zasada; patrz docstring modulu."""
    return math.floor(raw)


def spans_for_row(edges, lat_row, lon_min, lon_max, w):
    """Spany ladu (piksele x0..x1 WLACZNIE) dla jednego wiersza."""
    cross = row_crossings(edges, lat_row)
    raw = []
    for i in range(0, len(cross), 2):
        lon_a, lon_b = cross[i], cross[i + 1]
        raw0 = (lon_a - lon_min) / (lon_max - lon_min) * w
        raw1 = (lon_b - lon_min) / (lon_max - lon_min) * w
        if raw1 <= 0 or raw0 >= w:
            continue  # cala dzialka ladu poza oknem
        x0 = max(pixel_x0(raw0), 0)
        x1 = min(pixel_x1(raw1), w - 1)
        if x1 >= x0:
            raw.append((x0, x1))

    # Scalenie stykajacych/nachodzacych sie spanow. Realny przypadek: dwa
    # sasiednie pierscienie (np. material staly i wyspa dzielaca z nim
    # krawedz) daja dwa przeciecia w tym samym miejscu po zaokragleniu -
    # bez scalenia wyszlyby dwa spany "stykajace sie" zamiast jednego, co
    # zaburzaloby SPAN_COUNT bez zadnej wizualnej roznicy.
    raw.sort()
    merged = []
    for x0, x1 in raw:
        if merged and x0 <= merged[-1][1] + 1:
            merged[-1] = (merged[-1][0], max(merged[-1][1], x1))
        else:
            merged.append((x0, x1))
    return merged


def rasterize(edges, lat_min, lat_max, lon_min, lon_max, w, h):
    """Zwraca (spans, row_off) w formacie LAND_SPANS/LAND_ROW_OFF.

    Skanlinia kazdego wiersza przechodzi przez jego SRODEK (row+0.5)/h -
    empirycznie najlepszy z 3 sprawdzonych wariantow (gora/srodek/dol wiersza,
    patrz docstring modulu)."""
    spans = []
    row_off = [0]
    for row in range(h):
        lat_row = lat_max - (row + 0.5) / h * (lat_max - lat_min)
        spans.extend(spans_for_row(edges, lat_row, lon_min, lon_max, w))
        row_off.append(len(spans))
    return spans, row_off


# --------------------------------------------------------------------------
# Wybor granic zachowujacych izotropowa skale (ta sama liczba m/px w obu osiach)
# --------------------------------------------------------------------------

def compute_isotropic_bounds(center_lat, center_lon, width_km, w, h):
    """Granice LAT/LON wysrodkowane na (center_lat, center_lon), o zadanej
    szerokosci fizycznej width_km rozlozonej na w pikseli, z wysokoscia
    dobrana tak, zeby m/px w pionie wyszlo TAKIE SAMO jak w poziomie.

    Dlugosc geogr. kurczy sie z cos(lat) - to jest JEDYNE miejsce w tym
    skrypcie, gdzie cos(lat) w ogole wystepuje (patrz PROJEKCJA w docstringu
    modulu: sam wzor rzutowania jest liniowy, cos(lat) wchodzi wylacznie przy
    WYBORZE granic, raz, nie per piksel)."""
    m_per_px = width_km * 1000.0 / w

    lon_span = width_km / (KM_PER_DEG_LAT * math.cos(math.radians(center_lat)))
    lon_min = center_lon - lon_span / 2
    lon_max = center_lon + lon_span / 2

    height_km = m_per_px * h / 1000.0
    lat_span = height_km / KM_PER_DEG_LAT
    lat_min = center_lat - lat_span / 2
    lat_max = center_lat + lat_span / 2

    return lat_min, lat_max, lon_min, lon_max


def actual_m_per_px(lat_min, lat_max, lon_min, lon_max, w, h):
    """m/px rzeczywiscie wynikajace z (zaokraglonych) granic - do raportowania,
    NIE do generowania (te same granice, ktore trafiaja do naglowka)."""
    lat_c = (lat_min + lat_max) / 2
    width_km = (lon_max - lon_min) * KM_PER_DEG_LAT * math.cos(math.radians(lat_c))
    height_km = (lat_max - lat_min) * KM_PER_DEG_LAT
    return width_km * 1000 / w, height_km * 1000 / h


# --------------------------------------------------------------------------
# Formatowanie naglowka C++
# --------------------------------------------------------------------------

def format_pairs(spans, per_line=8):
    if not spans:
        return "  "
    w = max(len(str(x)) for pair in spans for x in pair)
    lines = []
    for i in range(0, len(spans), per_line):
        chunk = spans[i:i + per_line]
        lines.append("  " + " ".join(f"{{{x0:{w}d},{x1:{w}d}}}," for x0, x1 in chunk))
    return "\n".join(lines)


def format_ints(values, per_line=12):
    if not values:
        return "  "
    w = max(len(str(v)) for v in values)
    lines = []
    for i in range(0, len(values), per_line):
        chunk = values[i:i + per_line]
        lines.append("  " + " ".join(f"{v:{w}d}," for v in chunk))
    return "\n".join(lines)


def format_header(namespace, lat_min, lat_max, lon_min, lon_max, w, h, spans,
                   row_off, command_line):
    n = len(spans)
    mppx, mppy = actual_m_per_px(lat_min, lat_max, lon_min, lon_max, w, h)
    # LAND_SPANS trzyma wspolrzedne x (0..w-1); LAND_ROW_OFF trzyma liczniki
    # spanow (0..SPAN_COUNT) - to DWIE rozne gornice, nie jedna (h nie ogranicza
    # zadnej z tych wartosci wprost, wiec celowo nie wchodzi do tej decyzji).
    # W dzisiejszym gmapw/gmapr obie i tak wychodza ponad 255 (320 px, 260+
    # spanow), stad uint16_t w obu - ale przy mniejszej mapie te dwie granice
    # moga sie rozjechac, wiec liczymy je osobno.
    span_type = "uint16_t" if w - 1 > 255 else "uint8_t"
    row_off_type = "uint16_t" if n > 255 else "uint8_t"
    return f"""#pragma once

#include <cstdint>
#include <pgmspace.h>

// Wygenerowane automatycznie przez tools/gen_map.py z Natural Earth 10m land
// (rasteryzacja skanliniowa) - NIE EDYTOWAC RECZNIE. Zeby odtworzyc/zmienic:
//
//   {command_line}
//
// Zrodlo: https://www.naturalearthdata.com/downloads/10m-physical-vectors/10m-land/
// Granice dobrane pod izotropowa skale: {mppx:.2f} m/px poziomo, {mppy:.2f} m/px pionowo.
namespace {namespace} {{

constexpr float LAT_MIN = {lat_min:.4f}f;
constexpr float LAT_MAX = {lat_max:.4f}f;
constexpr float LON_MIN = {lon_min:.4f}f;
constexpr float LON_MAX = {lon_max:.4f}f;
constexpr int MAP_W = {w};
constexpr int MAP_H = {h};

constexpr int SPAN_COUNT = {n};
static const {span_type} LAND_SPANS[{n}][2] PROGMEM = {{
{format_pairs(spans)}
}};

static const {row_off_type} LAND_ROW_OFF[{h + 1}] PROGMEM = {{
{format_ints(row_off)}
}};

}}  // namespace {namespace}
"""


# --------------------------------------------------------------------------
# Weryfikacja wzgledem istniejacego pliku (test poprawnosci generatora)
# --------------------------------------------------------------------------

def parse_existing_header(path, namespace):
    """Parsuje istniejacy naglowek (regex, celowo bez kompilatora C++) i
    zwraca (lat_min, lat_max, lon_min, lon_max, w, h, spans, row_off)."""
    txt = Path(path).read_text(encoding="utf-8")
    ns_match = re.search(rf"namespace\s+{namespace}\s*\{{(.*?)\n}}\s*//\s*namespace\s+{namespace}",
                         txt, re.S)
    if not ns_match:
        sys.exit(f"Nie znalazlem 'namespace {namespace} {{ ... }}' w {path}")
    body = ns_match.group(1)

    def num(name):
        m = re.search(rf"{name}\s*=\s*([0-9.]+)f?\s*;", body)
        if not m:
            sys.exit(f"Nie znalazlem stalej {name} w {path}")
        return float(m.group(1))

    lat_min, lat_max = num("LAT_MIN"), num("LAT_MAX")
    lon_min, lon_max = num("LON_MIN"), num("LON_MAX")
    w, h = int(num("MAP_W")), int(num("MAP_H"))

    spans_m = re.search(r"LAND_SPANS\[\d+\]\[2\][^{]*\{(.*?)\n\};", body, re.S)
    spans = [(int(a), int(b)) for a, b in
             re.findall(r"\{\s*(\d+)\s*,\s*(\d+)\s*\}", spans_m.group(1))]

    off_m = re.search(r"LAND_ROW_OFF\[\d+\][^{]*\{(.*?)\n\};", body, re.S)
    row_off = [int(x) for x in re.findall(r"\d+", off_m.group(1))]

    return lat_min, lat_max, lon_min, lon_max, w, h, spans, row_off


def verify(namespace, lat_min, lat_max, lon_min, lon_max, w, h, spans, row_off,
           existing_path):
    """Porownuje swiezo wygenerowana mape z istniejacym plikiem, wiersz po
    wierszu. To jest test poprawnosci generatora (patrz opis zadania) - nie
    tylko "czy pliki sa identyczne", ale TEZ "o ile i gdzie sie roznia", zeby
    dalo sie ocenic, czy to szum podpikselowy, czy prawdziwy blad."""
    (e_lat_min, e_lat_max, e_lon_min, e_lon_max, e_w, e_h, e_spans,
     e_row_off) = parse_existing_header(existing_path, namespace)

    print(f"\n=== Weryfikacja wzgledem {existing_path} (namespace {namespace}) ===")
    bounds_match = (abs(lat_min - e_lat_min) < 1e-6 and abs(lat_max - e_lat_max) < 1e-6 and
                    abs(lon_min - e_lon_min) < 1e-6 and abs(lon_max - e_lon_max) < 1e-6 and
                    w == e_w and h == e_h)
    print(f"Granice/rozmiar identyczne: {bounds_match}")
    if not bounds_match:
        print(f"  (tu: LAT {lat_min}..{lat_max}, LON {lon_min}..{lon_max}, {w}x{h})")
        print(f"  (tam: LAT {e_lat_min}..{e_lat_max}, LON {e_lon_min}..{e_lon_max}, {e_w}x{e_h})")
        print("Granice sie roznia - porownanie wierszy pomijam (nie ma sensu).")
        return

    if h != e_h:
        print(f"Rozna liczba wierszy ({h} vs {e_h}) - przerywam.")
        return

    rows_new = [spans[row_off[r]:row_off[r + 1]] for r in range(h)]
    rows_old = [e_spans[e_row_off[r]:e_row_off[r + 1]] for r in range(h)]

    exact = sum(1 for a, b in zip(rows_new, rows_old) if a == b)
    same_count = sum(1 for a, b in zip(rows_new, rows_old) if len(a) == len(b))
    worst = 0
    worst_row = -1
    for r, (a, b) in enumerate(zip(rows_new, rows_old)):
        if len(a) == len(b) and a != b:
            d = max(abs(x0a - x0b) + abs(x1a - x1b) for (x0a, x1a), (x0b, x1b) in zip(a, b))
            if d > worst:
                worst, worst_row = d, r

    print(f"Wiersze identyczne co do piksela: {exact}/{h}")
    print(f"Wiersze o zgodnej LICZBIE spanow: {same_count}/{h}")
    print(f"Liczba spanow: tu {len(spans)}, tam {len(e_spans)}")
    if worst_row >= 0:
        print(f"Najwieksza roznica (przy zgodnej liczbie spanow): {worst} px, wiersz {worst_row}")
    if exact == h and len(spans) == len(e_spans):
        print("BIT W BIT IDENTYCZNE.")
    else:
        print("NIE jest bit w bit identyczne - patrz docstring modulu, sekcja")
        print("'CZY TO ODTWARZA MapDataWide.h BIT W BIT?' po wyjasnienie dlaczego.")


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description="Generator rastra ladu (LAND_SPANS/LAND_ROW_OFF) z Natural Earth 10m land.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument("--shapefile", type=Path, default=DEFAULT_SHAPEFILE,
                     help=f"sciezka do ne_10m_land.shp (domyslnie {DEFAULT_SHAPEFILE})")
    ap.add_argument("--namespace", required=True, help="np. gmapw, gmapr")
    ap.add_argument("--out", required=True, type=Path, help="plik wyjsciowy .h")
    ap.add_argument("--width", type=int, required=True, help="MAP_W w pikselach")
    ap.add_argument("--height", type=int, required=True, help="MAP_H w pikselach")

    g = ap.add_argument_group(
        "granice - albo podaj wszystkie cztery LAT/LON, albo centrum + szerokosc w km")
    g.add_argument("--lat-min", type=float)
    g.add_argument("--lat-max", type=float)
    g.add_argument("--lon-min", type=float)
    g.add_argument("--lon-max", type=float)
    g.add_argument("--center-lat", type=float)
    g.add_argument("--center-lon", type=float)
    g.add_argument("--width-km", type=float,
                    help="szerokosc fizyczna mapy - wysokosc dobierana automatycznie "
                         "z --height, zeby m/px bylo takie samo w obu osiach")

    ap.add_argument("--verify-against", type=Path,
                     help="istniejacy naglowek .h do porownania (test poprawnosci)")

    args = ap.parse_args()

    explicit = None not in (args.lat_min, args.lat_max, args.lon_min, args.lon_max)
    auto = None not in (args.center_lat, args.center_lon, args.width_km)
    if explicit == auto:  # oba albo zadne - dokladnie jedno musi byc podane
        ap.error("podaj albo --lat-min/--lat-max/--lon-min/--lon-max, "
                 "albo --center-lat/--center-lon/--width-km (dokladnie jeden z tych zestawow)")

    if explicit:
        lat_min, lat_max = args.lat_min, args.lat_max
        lon_min, lon_max = args.lon_min, args.lon_max
    else:
        lat_min, lat_max, lon_min, lon_max = compute_isotropic_bounds(
            args.center_lat, args.center_lon, args.width_km, args.width, args.height)
        print(f"Granice wyliczone z centrum ({args.center_lat}, {args.center_lon}) "
              f"i szerokosci {args.width_km} km:")
        print(f"  LAT {lat_min:.4f} .. {lat_max:.4f}")
        print(f"  LON {lon_min:.4f} .. {lon_max:.4f}")
        lat_min, lat_max = round(lat_min, 4), round(lat_max, 4)
        lon_min, lon_max = round(lon_min, 4), round(lon_max, 4)

    if not args.shapefile.exists():
        sys.exit(
            f"Brak pliku {args.shapefile}.\n"
            "Pobierz Natural Earth 10m land i rozpakuj - patrz naglowek tego skryptu "
            "(sekcja DANE WEJSCIOWE) po dokladny adres URL i uklad katalogow."
        )

    edges = load_edges(args.shapefile, lat_min, lat_max, lon_min, lon_max)
    spans, row_off = rasterize(edges, lat_min, lat_max, lon_min, lon_max,
                                args.width, args.height)

    mppx, mppy = actual_m_per_px(lat_min, lat_max, lon_min, lon_max, args.width, args.height)
    n_bytes = len(spans) * 4 + (args.height + 1) * 2
    print(f"\nSPAN_COUNT = {len(spans)}  ({n_bytes} B: {len(spans) * 4} B spanow + "
          f"{(args.height + 1) * 2} B LAND_ROW_OFF)")
    print(f"m/px: {mppx:.2f} poziomo, {mppy:.2f} pionowo")

    command_line = "python3 tools/gen_map.py " + " ".join(sys.argv[1:])
    header = format_header(args.namespace, lat_min, lat_max, lon_min, lon_max,
                            args.width, args.height, spans, row_off, command_line)
    args.out.write_text(header, encoding="utf-8")
    print(f"Zapisano {args.out}")

    if args.verify_against:
        verify(args.namespace, lat_min, lat_max, lon_min, lon_max, args.width,
               args.height, spans, row_off, args.verify_against)


if __name__ == "__main__":
    main()
