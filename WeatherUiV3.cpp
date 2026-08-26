// Motyw V3 "Pasmowy" - ekrany rdzeniowe (WeatherUiV3.cpp).
//
// DLACZEGO OSOBNY PLIK: dokladnie ten sam powod, co przy WeatherUiV2.cpp -
// WeatherUi.cpp ma ponad 4 tys. linii, a to jest TRZECI, rownolegly komplet
// widokow. Metody drawV3/drawV3Bottom naleza do klasy WeatherUi (maja dostep do
// air_/roomModel_/radarModel_/boiler_/burner_), ale ich definicje moga stac w
// dowolnej jednostce kompilacji. Reszta ekranow to file-static helpery w tym pliku;
// dostep do modeli idzie przez argumenty, wiec naglowek klasy nie puchnie.
//
// UKLAD (patrz ThemeV3.h i docs/design-v3/):
//   * drawV3        rysuje obszar sprite y=0..205 (wspolrzedne GLOBALNE, x=0..319);
//                   ZAWSZE wypelnia wlasne tlo (jasne dwukolumnowe / pelne jasne /
//                   ciemny radar / diag z ciemnym naglowkiem).
//   * drawV3Bottom  rysuje dolny pas y=206..239 WPROST na TFT (poza sprite), bo uklad
//                   V3 siega pelnej wysokosci (POWIETRZE na glownym, osie wykresow).
//
// `ox` jest w V3 zawsze 0 (brak slajdu), `t` (animacja wejscia) celowo pomijamy -
// V3 rysuje ekran wprost, bez przejsc. Zadna funkcja tutaj NIE mutuje diag() ani
// innego globalnego stanu (czytanie wolno) - rysuje sie takze do zrzutu BMP z
// drugiego rdzenia.

#include <Arduino.h>
#include <WiFi.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <pgmspace.h>

#include "WeatherUi.h"
#include "ThemeV3.h"
#include "Format.h"          // (v175) fmt1(), (v180) fmt2() — wspolne z panelem OLED
#include "CostData.h"        // (v180) CostModel — koszt zakupu z sieci w module PRAD
#include "PaybackHist.h"     // (v181) kPaybackHist — historia zwrotu z PV (ekran ZWROT)
#include "PlexText.h"
#include "CoastMap.h"
#include "MapDataRadar.h"   // gmapr:: granice (pozycja Gdyni na radarze)
#include "AirData.h"
#include "AirClient.h"       // airIndexName()
#include "RoomData.h"
#include "RoomHistory.h"   // RoomHistory:: (wspolny wykres temperatur 24 h na ekranie POKOJE)
#include "RadarData.h"
#include "PvData.h"
#include "WeatherData.h"
#include "FlightData.h"
#include "BleSensors.h"
#include "RadarMap.h"
#include "GasMeter.h"        // BurnerHistory
#include "Log.h"             // Diag, gPir, gLdr, diag()
#include "Settings.h"        // settings()
#include "Viessmann.h"       // vi::Model, vi::daysLeft()
#include "WeatherIcons.h"    // wxico::labelForCode()
#include "Config.h"          // cfg::VIEW_*
#include "Version.h"         // FW_VERSION

namespace {

// Skroty na przestrzen V3. NIE "using namespace tv3;" — Colors.h ma wlasny
// namespace 'col' (paleta V1/V2), wiec using-directive czynilby 'col::' dwuznacznym.
// Aliasy to jawne deklaracje i rozstrzygaja nazwe jednoznacznie na palete V3.
namespace col = tv3::col;
namespace grid = tv3::grid;
namespace wx = tv3::wx;
using tv3::Fresh;
using tv3::freshDot;
using tv3::sceneBg;
using tv3::sceneBottom;
using tv3::moduleSep;
using tv3::bar;
using tv3::scale5;

// ============================================================ NARZEDZIA ========

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// --- TRZY STANY SWIEZOSCI (v158) ----------------------------------------------
// Do v157 ekrany mialy tylko DWA stany do dyspozycji: "sa dane" i "nie ma danych",
// przez co "jeszcze nie pobrano" (swiezy start) wygladalo dokladnie tak samo jak
// "mamy dane sprzed godziny". Ponizsza para helperow rozdziela to na trzy:
//   (a) okAt == 0            -> Fresh::UNKNOWN  — nigdy nie pobrano
//   (b) wiek < prog          -> Fresh::OK       — dane biezace
//   (c) wiek >= prog         -> Fresh::STALE    — mamy dane, ale przeterminowane
// Progi ida WYLACZNIE z cfg::*_STALE_MS (Config.h) — zaden ekran nie ma juz prawa
// wpisac wlasnej liczby.
//
// Wiek liczymy ZE ZNAKIEM na int32: znaczniki to millis() pisany przez netTask,
// a uint32 przekreca sie po ~49 dniach pracy. Ujemna roznica (chwila po przekrece
// albo znacznik z przyszlosci przy wyscigu odczytu) ma znaczyc "swieze", a nie
// "starsze niz wszechswiat" — ten sam idiom, co ago() w Portal.cpp.
// Stan (a) i (b)/(c) rozdziela sam WOLAJACY — kazdy ekran ma dla "nigdy nie pobrano"
// wlasny napis ("czekam na dane", "nieodpytywane", "- czekam"), wiec wspolnego
// freshState() zwracajacego Fresh::UNKNOWN NIE MA: bylby funkcja bez ani jednego
// uzytkownika, a takie w tym repo nie zostaja (patrz notatki przy skasowanych stalych
// w Config.h). Tu liczymy wylacznie granice miedzy (b) a (c).
bool freshMs(uint32_t okAt, uint32_t staleMs) {
  if (okAt == 0) return false;
  return static_cast<int32_t>(millis() - okAt) < static_cast<int32_t>(staleMs);
}

// Pogoda "swieza": pobrana i nie starsza niz cfg::WEATHER_STALE_MS (40 min = 2,5 kadencji
// 15-minutowej; regula i wyliczenie stoja przy stalej w Config.h — do v157 bylo tu wpisane
// wprost 2 x WEATHER_REFRESH_MS, czyli 30 min, i byla to JEDYNA definicja swiezosci pogody
// w calym projekcie). Po zaniku sieci netTask NIE nadpisuje gWeather — ostatnia prognoza zostaje z
// ready==true, wiec sam `ready` NIE odroznia swiezej prognozy od godzinami nieaktualnej.
// Trzeba spojrzec na wiek ostatniego udanego pobrania: diag().weatherOkAt (millis) pisze
// netTask, odczyt uint32 jest atomowy, a ageMs liczymy ZE ZNAKIEM (idiom ago() z
// Portal.cpp — po ~49 dniach uint32 sie przekreca, ujemny wiek traktujemy jak "swiezy").
// TEN SAM wzor, co interpolacja chmur radaru w pogoda-gdynia.ino.
bool wxFresh() {
  return freshMs(diag().weatherOkAt, cfg::WEATHER_STALE_MS);
}

// Wiek DOWOLNEGO stempla millis() w SEKUNDACH (0, gdy stempel jest zerowy, czyli
// "nigdy nie bylo udanego pobrania"). Ten sam idiom ze znakiem, co freshMs: roznice
// liczymy na int32, bo uint32 przekreca sie po ~49 dniach pracy, a stemple pisze
// netTask — inny watek niz rysowanie.
// (v161) Wydzielone z wxAgeS(), bo tej samej liczby potrzebuja teraz takze ekrany
// PRAD i OGRZEWANIE (podpis wieku przy starych danych). Jedno miejsce zamiast trzech.
uint32_t okAgeS(uint32_t okAt) {
  if (okAt == 0) return 0;
  const int32_t ageMs = static_cast<int32_t>(millis() - okAt);
  return ageMs > 0 ? static_cast<uint32_t>(ageMs) / 1000u : 0u;
}

// Wiek pogody w SEKUNDACH (0, gdy nigdy nie pobrano) — do dopisku "sprzed X min"
// przy nieswiezej prognozie.
uint32_t wxAgeS() {
  return okAgeS(diag().weatherOkAt);
}

// Stan swiezosci pogody dla kropki freshDot: UNKNOWN gdy nigdy nie pobrano, OK gdy
// swieza, STALE gdy pobrana ale juz nieaktualna. Jedno zrodlo prawdy dla wszystkich
// naglowkow pogodowych V3.
Fresh wxFreshState(const WeatherModel& w) {
  if (!w.ready) return Fresh::UNKNOWN;
  return wxFresh() ? Fresh::OK : Fresh::STALE;
}

// Powietrze ma DWA rozne wieki i mylenie ich jest latwe: wiek NASZEGO pobrania
// (diag().airOkAt, prog cfg::AIR_FETCH_STALE_MS) i wiek PROBKI ze stacji
// (a.sampleEpoch, prog cfg::AIR_SAMPLE_STALE_S). Dla patrzacego na ekran liczy sie
// ten drugi: mozemy odpytywac ARMAAG co kwadrans z pelnym powodzeniem, a stacja i tak
// oddaje ta sama trzygodzinna srednia. Dlatego ekrany pytaja o probke — a pobranie
// widac w /api/diag i na ekranie diagnostyki.
// sampleEpoch to EPOCH (nie millis!), wiec wymaga czasu z NTP; bez niego nie mamy jak
// policzyc wieku i nie udajemy, ze mamy — zwracamy false (czyli "stara"), bo brak
// zegara to takze brak dowodu na swiezosc.
bool airSampleFresh(const AirModel& a) {
  const time_t now = time(nullptr);
  if (now <= 1700000000 || a.sampleEpoch == 0) return false;
  const time_t se = static_cast<time_t>(a.sampleEpoch);
  return se <= now && (now - se) < static_cast<time_t>(cfg::AIR_SAMPLE_STALE_S);
}

// "%.1f" z polskim przecinkiem dziesietnym (mockupy: "3,2 kW", "22,4 st").
// (v175) DEFINICJA PRZENIESIONA DO Format.h — dolaczonego wyzej — bo od tego wydania
// te sama regule potrzebuje TAKZE panel OLED (OledPanel.cpp), a to inny plik i inny
// rdzen rysujacy. Tutaj zostaje sam odsylacz, zeby nikt nie szukal jej w tym pliku.
// Wszystkie ~20 wywolan nizej celuje teraz w te jedna, wspolna funkcje.

// Moc [kW] z DWOMA miejscami po przecinku dla malych wartosci (<10 kW), jednym dla
// wiekszych. Wlasciciel: przy 0,2 kW nie widac, ze wartosc realnie sie zmienia (0,20->
// 0,24 zaokragla sie do "0,2"); w setnych zmiana jest widoczna od razu. Powyzej 10 kW
// setne sa zbedne i zabralyby szerokosc.
void fmtKw(char* b, size_t n, float v) {
  snprintf(b, n, (v < 10.f && v > -10.f) ? "%.2f" : "%.1f", v);
  for (char* p = b; *p; ++p)
    if (*p == '.') { *p = ','; break; }
}

// Grupowanie tysiecy spacja (mockup diagnostyki: "90 100 B").
void groupNum(char* out, size_t n, uint32_t v) {
  char tmp[16];
  snprintf(tmp, sizeof(tmp), "%lu", static_cast<unsigned long>(v));
  const int len = static_cast<int>(strlen(tmp));
  int o = 0;
  for (int i = 0; i < len && o < static_cast<int>(n) - 2; ++i) {
    if (i > 0 && (len - i) % 3 == 0) out[o++] = ' ';
    out[o++] = tmp[i];
  }
  out[o] = 0;
}

// Przycina RUNTIME SSID (do 32 znakow) do budzetu pikseli w foncie f13: gdy cala nazwa
// sie nie miesci, obcina od konca i dokleja "..." (Plex NIE ma glifu wielokropka U+2026,
// wiec trzy zwykle kropki, szer. 12 px). `out` MUSI miec >= 40 B. Repo PUBLICZNE — helper
// operuje na DLUGOSCI, nie na tresci; zadnych nazw sieci ani IP w kodzie. budgetPx <= 0
// => sam wielokropek. O(n^2) po znakach (n <= 32) — bez znaczenia poza sciezka krytyczna.
void fitSsid(char* out, size_t n, const char* ssid, int budgetPx) {
  const pltxt::FontSet f = plex::f13();
  if (plex::width(f, ssid) <= budgetPx) { snprintf(out, n, "%s", ssid); return; }
  const int dots = plex::width(f, "...");
  char tmp[36];
  for (int len = static_cast<int>(strlen(ssid)); len > 0; --len) {
    const int k = len < static_cast<int>(sizeof(tmp)) - 1 ? len : static_cast<int>(sizeof(tmp)) - 1;
    memcpy(tmp, ssid, static_cast<size_t>(k));
    tmp[k] = 0;
    if (plex::width(f, tmp) + dots <= budgetPx) { snprintf(out, n, "%s...", tmp); return; }
  }
  snprintf(out, n, "...");
}

// Miesiace (dopelniacz) i dni tygodnia - DISPLAY, wiec z polskimi znakami.
const char* kMonth[12] = {"stycznia", "lutego", "marca", "kwietnia", "maja", "czerwca",
                          "lipca", "sierpnia", "września", "października", "listopada", "grudnia"};
// SKROTY miesiecy TYLKO dla daty w ciemnej kolumnie ekranu glownego (v3Main). OSOBNA
// tablica, bo kMonth (dopelniacz, pelne nazwy) nadal moze przydac sie gdzie indziej i
// nie chcemy zmieniac jego semantyki. `const char* const` => tablica wskaznikow i
// literaly leza w .rodata (flash), zero statycznego RAM-u. Najszersza data "pon 30 mar"
// = 66 px @ f13 (x=10..76) miesci sie w kolumnie x=0..119 z duzym zapasem.
static const char* const kMonthShort[12] = {"sty", "lut", "mar", "kwi", "maj", "cze",
                                            "lip", "sie", "wrz", "paź", "lis", "gru"};
const char* kDowLo[7] = {"ndz", "pon", "wt", "śr", "czw", "pt", "sob"};       // tm_wday 0=ndz
const char* kDowHi[7] = {"NDZ", "PON", "WT", "ŚR", "CZW", "PT", "SOB"};

// Zegar w kolumnie kontekstu. Bez NTP rysujemy "--:--" (font zegara nie ma myslnika,
// wiec kreski stawiamy recznie prostokatami).
void drawClock(TFT_eSPI& s, int x, int baseline, uint16_t colr) {
  const time_t now = time(nullptr);
  const pltxt::FontSet f = plex::f24();
  if (now < 1700000000) {
    const int dw = plex::width(f, "0");
    int cx = x;
    for (int i = 0; i < 5; ++i) {
      if (i == 2) {
        cx += plex::str(s, f, ":", cx, baseline, colr);
      } else {
        s.fillRect(cx + 1, baseline - 8, dw - 2, 3, colr);
        cx += dw;
      }
    }
    return;
  }
  struct tm tmv{};
  localtime_r(&now, &tmv);
  // Dwukropek MRUGA co sekunde (wlasciciel): widoczny w parzyste sekundy, schowany w
  // nieparzyste. HH i MM stoja na stalych pozycjach (dwukropek zajmuje swoja szerokosc
  // niezaleznie od tego, czy go rysujemy), wiec cyfry nie skacza. Zeby to bylo widac,
  // render() dorzuca sekunde do sygnatury pomijania klatek TYLKO na ekranie glownym
  // (patrz WeatherUi.cpp) — reszta ekranow nie przerysowuje sie co sekunde.
  char hh[4], mm[4];
  snprintf(hh, sizeof(hh), "%02d", tmv.tm_hour);
  snprintf(mm, sizeof(mm), "%02d", tmv.tm_min);
  int cx = x;
  cx += plex::str(s, f, hh, cx, baseline, colr);
  const int cwid = plex::width(f, ":");
  if (tmv.tm_sec % 2 == 0) plex::str(s, f, ":", cx, baseline, colr);
  cx += cwid;
  plex::str(s, f, mm, cx, baseline, colr);
}

// "sprzed X min" / "sprzed X h" - wiek w sekundach na slowa.
void agoWords(char* b, size_t n, uint32_t sec) {
  if (sec < 90) snprintf(b, n, "sprzed %lu s", static_cast<unsigned long>(sec));
  else if (sec < 5400) snprintf(b, n, "sprzed %lu min", static_cast<unsigned long>(sec / 60));
  else if (sec < 172800) snprintf(b, n, "sprzed %lu h", static_cast<unsigned long>(sec / 3600));
  else snprintf(b, n, "sprzed %lu dni", static_cast<unsigned long>(sec / 86400));
}

// Czas pracy urzadzenia na slowa. "0 d 0 h" bylo bez sensu przy krotkim uptime
// (a po kazdej aktualizacji OTA uptime startuje od zera): ponizej 1 h pokazujemy
// minuty, ponizej doby godziny+minuty, dalej dni+godziny.
void fmtUptime(char* b, size_t n, uint32_t sec) {
  const unsigned long d = sec / 86400, h = (sec / 3600) % 24, m = (sec / 60) % 60;
  if (d > 0) snprintf(b, n, "%lu d %lu h", d, h);
  else if (h > 0) snprintf(b, n, "%lu h %lu min", h, m);
  else snprintf(b, n, "%lu min", m);
}

// Zawija `src` do dwoch linii (po spacji), tak by kazda zmiescila sie w `w` px przy
// foncie f. l1/l2 MUSZA miec >= 48 B. l2 zostaje puste, gdy calosc miesci sie w jednej
// linii albo gdy nie ma sensownej spacji do podzialu. Wzorzec jak przy opisie pogody
// w v3Main, wydzielony bo drawV3Alert potrzebuje go dla tytulu i tekstu alertu.
void wrap2(const pltxt::FontSet& f, const char* src, int w, char* l1, char* l2) {
  l1[0] = 0;
  l2[0] = 0;
  if (!src || !src[0]) return;
  if (plex::width(f, src) <= w) { snprintf(l1, 48, "%s", src); return; }
  const int n = static_cast<int>(strlen(src));
  int cut = -1;
  char tmp[48];
  for (int i = 1; i < n && i < 47; ++i) {
    if (src[i] != ' ') continue;
    strncpy(tmp, src, i);
    tmp[i] = 0;
    if (plex::width(f, tmp) <= w) cut = i; else break;
  }
  if (cut <= 0) { snprintf(l1, 48, "%s", src); return; }
  strncpy(l1, src, cut);
  l1[cut] = 0;
  snprintf(l2, 48, "%s", src + cut + 1);
}

// isNightNow() przeniesione z tego anonimowego namespace do METODY WeatherUi (definicja
// tuz przed DISPATCHERAMI nizej). Powod: potrzebuje jej takze render() w WeatherUi.cpp
// (tryb nocny "dotyk budzi ekran"), a file-static z tego pliku nie da sie stamtad wolac.
// Logika bez zmian — call-site w drawV3/drawV3Bottom zostaje `isNightNow(blTarget_)`.

// Kolor temperatury - plaskie odcienie z palety (spec: zero gradientow).
uint16_t tempCol(float c) {
  if (c <= 0.f) return col::RAIN;      // mroz - gleboki niebieski
  if (c <= 8.f) return col::RAIN2;
  if (c <= 16.f) return col::ACCENT;   // chlod - jasny niebieski
  if (c <= 23.f) return col::SUN;      // cieplo - bursztyn
  if (c <= 29.f) return col::PV;       // goraco - pomarancz
  return col::GRID;                    // upal - czerwien
}

// Kolor klasy jakosci powietrza (indeks ARMAAG 1..6).
uint16_t airCol(int idx) {
  switch (idx) {
    case 1: return col::OK;
    case 2: return col::OK;
    case 3: return col::PV;
    case 4: return 0xEC00;   // pomarancz
    case 5: return col::GRID;
    case 6: return 0x9010;   // ciemna czerwien/purpura
    default: return col::MUTE;
  }
}

// Kod IATA -> polska nazwa miasta (mockup samolotow: "Warszawa -> Gdansk").
// Tablica i literaly siedza w .rodata (flash), nie w RAM. Nieznany kod = sam IATA.
const char* cityOf(const char* iata) {
  struct M { const char* k; const char* v; };
  static const M m[] = {
      {"GDN", "Gdańsk"},   {"WAW", "Warszawa"},  {"WMI", "Warszawa"}, {"KRK", "Kraków"},
      {"WRO", "Wrocław"},  {"POZ", "Poznań"},    {"KTW", "Katowice"}, {"RZE", "Rzeszów"},
      {"SZZ", "Szczecin"}, {"BZG", "Bydgoszcz"}, {"CPH", "Kopenhaga"},{"ARN", "Sztokholm"},
      {"NYO", "Sztokholm"},{"OSL", "Oslo"},      {"HEL", "Helsinki"}, {"RIX", "Ryga"},
      {"TLL", "Tallinn"},  {"VNO", "Wilno"},     {"KUN", "Kowno"},    {"BER", "Berlin"},
      {"MUC", "Monachium"},{"FRA", "Frankfurt"}, {"AMS", "Amsterdam"},{"LHR", "Londyn"},
      {"STN", "Londyn"},   {"LTN", "Londyn"},    {"DUB", "Dublin"},   {"BLL", "Billund"},
      {"GOT", "Goteborg"}, {"HAM", "Hamburg"},   {"DTM", "Dortmund"}, {"EIN", "Eindhoven"},
  };
  if (!iata || !iata[0]) return "";
  for (const M& e : m)
    if (strncmp(iata, e.k, 3) == 0) return e.v;
  return iata;
}

// Naglowek ekranu PELNOJASNEGO: etykieta wersalikami z lewej, dopisek z prawej,
// cienka linia pod spodem. Zwraca y linii.
void lightHeader(TFT_eSPI& s, const char* label, const char* right, Fresh fresh = Fresh::UNKNOWN) {
  plex::str(s, plex::f11(), label, grid::MARGIN, 22, col::SECOND);
  int rx = grid::W - grid::MARGIN;
  if (fresh != Fresh::UNKNOWN) {
    freshDot(s, rx - 3, 18, fresh);
    rx -= 12;
  }
  if (right && right[0]) plex::strRight(s, plex::f13(), right, rx, 22, col::MUTE);
  s.drawFastHLine(grid::MARGIN, 30, grid::W - 2 * grid::MARGIN, col::LINE);
}

// Naglowek diagnostyki: CIEMNY pasek 0..28 na cala szerokosc.
void darkHeader(TFT_eSPI& s, const char* label, const char* right) {
  s.fillRect(0, 0, grid::W, 28, col::PANEL);
  plex::str(s, plex::f11(), label, grid::MARGIN, 19, col::ONDARK);
  if (right && right[0]) plex::strRight(s, plex::f13(), right, grid::W - grid::MARGIN, 19, col::ONDARK_DIM);
}

// ============================================================ EKRAN GLOWNY =====
// Makieta 01 (warianty 06/09/17/21). Dwie kolumny: ciemny kontekst + jasne dane.

// Rysuje maly wykres opadu 12 h w kolumnie danych.
// `muted` = prognoza przeterminowana: slupki rysujemy nadal (to wciaz jedyne, co
// wiemy o najblizszych godzinach), ale WSZYSTKIE w kolorze linii pomocniczej, bez
// niebieskiej skali intensywnosci. Kolor niesie tu znaczenie ("mocny opad"), a przy
// starych danych bylby obietnica, ktorej nie mamy czym pokryc — ksztalt zostaje,
// obietnica znika. To ten sam zabieg, co "-" zamiast liczb na ekranie 5 DNI.
void precipChart(TFT_eSPI& s, const WeatherModel& w, int x, int y, int wdt, int hgt,
                 bool muted) {
  const int n = WX_HOURS;
  const int pitch = wdt / n;
  const int bw = pitch - 4 > 3 ? pitch - 4 : 3;
  const int base = y + hgt;
  if (!w.ready) {
    // Placeholder - kreskowana linia bazowa (mockup 21 "jeszcze nie pobrany").
    for (int xx = x; xx < x + wdt; xx += 8) s.drawFastHLine(xx, base, 4, col::LINE);
    return;
  }
  for (int i = 0; i < n; ++i) {
    if (!w.hours[i].valid) continue;
    const auto& h = w.hours[i].data;
    const int prob = h.precipProb;
    const int bx = x + i * pitch;
    const int bh = static_cast<int>(hgt * clampf(prob / 100.f, 0.f, 1.f));
    if (bh <= 0) {
      s.drawFastHLine(bx, base - 1, bw, col::LINE);
    } else {
      const uint16_t cc = muted ? col::LINE
                        : (h.precipMm >= 0.5f ? col::RAIN : (prob >= 50 ? col::RAIN2 : col::RAIN3));
      s.fillRect(bx, base - bh, bw, bh, cc);
    }
  }
  // Os czasu: 5 rownych etykiet co 3 h, od pierwszej prognozowanej godziny do +12 h
  // (np. 10 · 13 · 16 · 19 · 22). Etykiety rozstawione po szerokosci, ostatnia na
  // prawej krawedzi (godzina PO ostatnim slupku) — wlasciciel prosil o "o 1 h dluzej":
  // wczesniej ostatnia etykieta = ostatni slupek (21), teraz = koniec okna (22).
  if (w.hours[0].valid) {
    const int firstH = w.hours[0].hourOfDay;   // +1 h wzgledem teraz (np. 10)
    for (int k = 0; k <= 4; ++k) {
      char hb[4];
      snprintf(hb, sizeof(hb), "%d", (firstH + k * 3) % 24);
      const int lx = x + (k * wdt) / 4;
      plex::strCenter(s, plex::f10(), hb, k == 4 ? lx - 6 : (k == 0 ? lx + 6 : lx),
                      base + 11, col::MUTE);
    }
  }
}

// Modul PRAD na ekranie glownym (stany: produkcja / pobor / spi / lokalny).
// (v180) `cost` moze byc nullptr (warstwa danych jeszcze nic nie podpiela) — wtedy
// wiersz z kosztem po prostu nie powstaje, tak samo jak przy braku wiadomosci MQTT.
void mainPvModule(TFT_eSPI& s, const WeatherModel& w, const PvModel& pv,
                  const CostModel* cost, int top) {
  const int lx = grid::DATA_L;
  // Szerokosc naglowka LICZONA, a nie wpisana: tuz za nim staje plakietka taryfy
  // (nizej), a "PRĄD" w f11 ma dzis 30 px — liczba, ktora zmienia sie razem z fontem.
  const int hdrW = plex::str(s, plex::f11(), "PRĄD", lx, top, col::SECOND);

  // (v161) Te same trzy stany, co na ekranie PRAD (v3Pv) i z tego samego zrodla.
  // ZMIANA ZNACZENIA GALEZI "JUTRO": do v160 wchodzila przy KAZDYM nieudanym
  // odczycie, bo netTask kasowal wtedy gPv — czyli jedna zgubiona ramka Modbus
  // zamieniala modul PRAD w prognoze na jutro i z powrotem. Teraz warunkiem jest
  // `!pv.data.valid`, czyli "nie bylo jeszcze ANI JEDNEGO odczytu" — a to jest
  // dokladnie sytuacja, dla ktorej ta galaz powstala (makieta 17 "minimalna
  // instalacja": urzadzenie bez falownika). Przy chwilowej awarii modul zostaje
  // modulem PRAD, tylko wyciszonym i podpisanym wiekiem.
  const uint32_t pvAge = okAgeS(diag().pvOkAt);
  const bool pvEver = pv.data.valid;
  const bool pvFresh = pv.online && freshMs(diag().pvOkAt, pv.asleep ? cfg::PV_STALE_NIGHT_MS
                                                                    : cfg::PV_STALE_MS);
  // `!pv.asleep` — jak w v3Pv: sen ma wlasna, prawdziwa galaz nizej i nie jest
  // przeterminowaniem.
  const bool pvOld = pvEver && !pvFresh && !pv.asleep;

  if (!pvEver && !pv.asleep) {
    // Brak falownika (mockup 17 "minimalna instalacja") - pokazujemy JUTRO.
    plex::strRight(s, plex::f13(), "prognoza", grid::DATA_R, top, col::MUTE);
    plex::str(s, plex::f11(), "JUTRO", lx, top, col::SECOND);   // nadpisz etykiete
    if (w.ready && w.days[0].valid) {
      const auto& d = w.days[0];
      wx::glyph(s, d.weatherCode, false, lx + 18, top + 34, 15, true);
      char tv[24];
      snprintf(tv, sizeof(tv), "%.0f° / %.0f°", d.tempMax, d.tempMin);
      plex::str(s, plex::f20(), tv, lx + 40, top + 40, col::PANEL);
      char uv[16];
      snprintf(uv, sizeof(uv), "UV %.0f", d.uvMax);
      plex::str(s, plex::f13(), uv, lx + 40, top + 60, col::MUTE);
      if (d.date[0]) plex::strRight(s, plex::f13(), d.date, grid::DATA_R, top + 40, col::SECOND);
    } else {
      plex::str(s, plex::f13(), "brak prognozy", lx, top + 40, col::MUTE);
    }
    return;
  }

  // (v182) PLAKIETKA TARYFY tuz za slowem PRĄD: zielone TANI albo bursztynowe DROGI.
  // TA SAMA FORMA, co plakietka jakosci powietrza w v3MainBottom — zaokraglony
  // prostokat wypelniony kolorem, tekst f11 w col::BG, padding 7 px z kazdej strony
  // (szerokosc tw + 14), wysokosc 17 px, promien 5, gorna krawedz 11 px nad linia
  // bazowa napisu. Przepisane co do piksela, a nie wymyslone od nowa: dwa rozne
  // ksztalty pigulki na jednym ekranie czytalyby sie jak dwa rozne rodzaje
  // informacji, a to jest ten sam rodzaj — jednoslowna klasyfikacja stanu.
  // ZADNEGO NOWEGO KOLORU: OK i WARN sa juz w palecie i znacza w niej dokladnie to,
  // co tutaj (zielony = dobrze, bursztyn = uwaga).
  //
  // PO GALEZI "JUTRO", A PRZED RESZTA — i to jest cala przyczyna, dla ktorej ten blok
  // stoi tu, a nie zaraz przy naglowku wyzej: galaz "minimalnej instalacji" NADPISUJE
  // slowo PRĄD slowem JUTRO, a "JUTRO [DROGI]" nie znaczy nic. Galaz "śpi - noc"
  // plakietke DOSTAJE i to jest jej najwazniejsze wystapienie: falownik spi wlasnie
  // wtedy, gdy zapada decyzja o nocnym ladowaniu auta.
  //
  // DWA STANY, oba jawnie (regula v158):
  //   (a) tariff < 0 ("nie wiem", takze po odrzuceniu smiecia w MqttClient.cpp) ALBO
  //       dane starsze niz cfg::COST_STALE_MS -> NIE RYSUJEMY NIC. To nie jest
  //       ostroznosc na wyrost: wlasciciel podejmuje na podstawie tej plakietki
  //       decyzje o ladowaniu auta, a strefa G12w przelacza sie w ciagu doby kilka
  //       razy — plakietka sprzed pol godziny umie byc juz nieprawdziwa i kazalaby
  //       wlaczyc ladowarke w szczycie. BRAK plakietki kaze sprawdzic; ZLA plakietka
  //       kaze zrobic blad z pelnym przekonaniem. freshMs() lapie takze atMs == 0
  //       ("nigdy nie przyszla zadna wiadomosc"), wiec osobny warunek jest zbedny.
  //   (b) swieze -> plakietka w kolorze strefy.
  //
  // GEOMETRIA POLICZONA (f11, DATA_L = 127, DATA_R = 313): "PRĄD" konczy sie na
  // x = 157, plakietka startuje 6 px dalej (x = 163). Szerszy wariant "DROGI" ma
  // 37 px tekstu, czyli 51 px pigulki -> prawa krawedz x = 214. Najszerszy prawy
  // napis tego wiersza to "dziś 100,0 kWh" = 84 px w f13, wyrownany do DATA_R, czyli
  // lewa krawedz x = 229 (wariant z wiekiem "sprzed 999 dni" = 83 px konczy sie
  // jeszcze dalej w prawo). ZOSTAJE 15 px odstepu w najciasniejszym ukladzie.
  // Pionowo: 101..118, a separator modulu lezy na y = 92 — 9 px nad pigulka.
  if (cost != nullptr && cost->tariff >= 0 && freshMs(cost->atMs, cfg::COST_STALE_MS)) {
    const bool dear = cost->tariff == 1;
    const char* tl = dear ? "DROGI" : "TANI";
    const int tw = plex::width(plex::f11(), tl);
    const int bx = lx + hdrW + 6;
    s.fillRoundRect(bx, top - 11, tw + 14, 17, 5, dear ? col::WARN : col::OK);
    plex::str(s, plex::f11(), tl, bx + 7, top, col::BG);
  }

  if (pv.asleep) {
    wx::glyph(s, 0, true, lx + 12, top + 30, 12, true);   // ksiezyc
    plex::str(s, plex::f20(), "śpi - noc", lx + 32, top + 34, col::SECOND);
    char sub[40];
    if (w.sunrise[0]) snprintf(sub, sizeof(sub), "wróci po wschodzie (%s)", w.sunrise);
    else snprintf(sub, sizeof(sub), "falownik milczy po zachodzie");
    plex::str(s, plex::f13(), sub, lx, top + 58, col::MUTE);
    return;
  }

  // Falownik ONLINE. Zaciskamy prod/load do >=0: falownik potrafi na chwile podac
  // NIESPOJNE dane (np. eksport > produkcja => "zuzycie domu" wychodzi UJEMNE). Wtedy
  // szerokosc segmentu paska (selfUse/span) stawala sie ujemna i fillRect przelewal pasek
  // na cala szerokosc — wjazd w lewa, ciemna kolumne (zgloszone przez wlasciciela). Ujemne
  // przeplywy nie maja sensu fizycznego, wiec tniemy je do 0 u zrodla.
  const int prod = pv.data.powerAcW < 0 ? 0 : pv.data.powerAcW;
  const int load = pv.data.houseLoadW < 0 ? 0 : pv.data.houseLoadW;
  const int gridW = pv.data.gridPowerW;   // >0 oddajemy, <0 pobor
  // (v167) KONTRAKT TEGO MODULU: WIELKA LICZBA JEST SUMA PASKA. Pasek nizej dzieli
  // max(prod, load), czyli RAZ produkcje (na [z PV][→sieć]), a RAZ pobor domu
  // (na [z PV][z sieci]) — i tak MUSI byc, bo tylko wieksza z dwoch mocy zawiera
  // w sobie obie czesci. Ale zamiana podstawy musi byc widoczna, inaczej niebieski
  // segment jest raz procentem PRODUKCJI, a raz procentem POBORU i obie liczby
  // wygladaja identycznie (25% w poludnie i 13% rano znacza wtedy co innego).
  // Mowi to slowo przy wielkiej liczbie ("kW prod." / "kW pobór") — pod warunkiem,
  // ze wielka liczba naprawde jest ta podstawa.
  // BLAD DO v166: warunek brzmial `prod >= load && prod > 120`, wiec gdy obie moce
  // byly ponizej 120 W, a produkcja nie mniejsza od poboru (swit, zmierzch, zima —
  // np. PV 100 W, dom 50 W), wielka liczba pokazywala POBOR ("0,1 kW pobór"),
  // a pasek dzielil PRODUKCJE 100 W na [50 W z PV][50 W do sieci]. Podpis opisywal
  // wtedy inna wielkosc niz pasek pod nim, i nic tego nie zdradzalo.
  // `prod > load` daje rownowaznosc scisla: prod > load => max = prod (podpis
  // "prod."), prod <= load => max = load (podpis "pobór"). Prog 120 W jest zbedny,
  // bo przy prod = load = 0 warunek i tak jest falszywy — zostaje uczciwe
  // "0,0 kW pobór" nad pustym torem paska (span = 0, patrz nizej).
  const bool producing = prod > load;

  // (v161) Wyciszenie i wiek — wzorzec z v158 (ekran glowny, wiersz "odczuwalna"
  // ustepujacy miejsca wiekowi). ZERO NOWYCH WIERSZY: prawy gorny wiersz modulu
  // istnial i tak, tylko zamiast "dziś 12,3 kWh" niesie teraz "sprzed 14 min"
  // w col::WARN, czyli w kolorze, ktory paleta V3 opisuje wprost jako "nieswieze".
  // "dziś X kWh" celowo ustepuje: to licznik NARASTAJACY (przy starym odczycie
  // zanizony, ale nigdy zawyzony), a wiek jest w tej chwili wazniejsza informacja.
  // Najszerszy wariant wieku "sprzed 89 min" ma 80 px w f13, a "dziś 12,3 kWh" 88 px
  // — napis wyrownany do prawej (DATA_R = 313), wiec krotszy tekst nie moze nikomu
  // wjechac w lewo.
  const uint16_t cMain = pvOld ? col::MUTE : col::PANEL;
  const uint16_t cSec = pvOld ? col::MUTE : col::SECOND;

  char todayL[24];
  if (pvOld) {
    agoWords(todayL, sizeof(todayL), pvAge);
    plex::strRight(s, plex::f13(), todayL, grid::DATA_R, top, col::WARN);
  } else {
    char today[16];
    fmt1(today, sizeof(today), pv.data.energyTodayKwh);
    snprintf(todayL, sizeof(todayL), "dziś %s kWh", today);
    plex::strRight(s, plex::f13(), todayL, grid::DATA_R, top, col::MUTE);
  }

  // Wielka liczba = przeplyw DOMINUJACY (produkcja gdy produkujemy, inaczej pobor domu).
  char big[16];
  fmt1(big, sizeof(big), (producing ? prod : load) / 1000.f);
  const int bwv = plex::str(s, plex::f20(), big, lx, top + 34, cMain);
  // Podpis skrocony (" kW prod." / " kW pobór", bez "uksji"/"domu"): przy dwucyfrowej
  // mocy (dom > 10 kW — indukcja+piekarnik) pelny "12,4 kW pobór domu" (edge x=266)
  // nachodzil na prawa metryke "PV 0,0 kW" (left x=256). Teraz najszerszy lewy podpis
  // konczy sie na x=230, a najwezsza prawa metryka ("dom 19,9 kW") zaczyna x=239 -> 9 px
  // luzu. Sens obu metryk zachowany (druga metryka nizej pokazuje ten drugi przeplyw).
  plex::str(s, plex::f13(), producing ? " kW prod." : " kW pobór",
            lx + bwv, top + 34, cSec);
  // DRUGA metryka (prawy gorny wiersz): ten drugi przeplyw, zeby ZAWSZE bylo widac i
  // produkcje, i biezace ZUZYCIE domu (wlasciciel: brakowalo "ile zuzywamy"). Gdy
  // produkujemy -> "dom {pobor}"; gdy pobieramy -> "PV {produkcja}".
  {
    char sv[10], sl[18];
    fmt1(sv, sizeof(sv), (producing ? load : prod) / 1000.f);
    snprintf(sl, sizeof(sl), producing ? "dom %s kW" : "PV %s kW", sv);
    plex::strRight(s, plex::f13(), sl, grid::DATA_R, top + 34, col::MUTE);
  }

  // --- PASEK PRZEPLYWU ENERGII (dynamiczny, wszystkie przypadki) ----------------
  // Rozklad mocy na trzy skladniki:
  //   selfUse = min(prod, load)  — autokonsumpcja        (NIEBIESKI col::SELF)
  //   export  = max(0, prod-load) — nadwyzka PV do sieci  (ZIELONY  col::OK)
  //   import  = max(0, load-prod) — dom dobiera z sieci   (CZERWONY col::GRID)
  // (v167) TE TRZY KOLORY SA WZORCEM DLA CALEGO PROJEKTU — ten pasek jest jedynym
  // miejscem, gdzie wszystkie trzy skladniki bilansu stoja obok siebie, wiec to on
  // definiuje jezyk kolorow energii (pelny kontrakt: ThemeV3.h przy col::SELF).
  // W v167 dostosowal sie do niego ekran PRAD — oba paski dnia i wykres doby.
  // Tutaj NIC sie nie zmienilo: ten modul mial racje od poczatku.
  // Szerokosc paska = max(prod, load), wiec oba scenariusze sie mieszcza:
  //   prod > load -> [niebieski selfUse][zielony export]  (oddajemy nadprodukcje)
  //   prod < load -> [niebieski selfUse][czerwony import] (kupujemy z sieci)
  //   prod ~ 0    -> caly czerwony import                 (noc / brak slonca)
  // (void)gridW/producing niepotrzebne — liczymy przeplyw wprost z prod/load; gridW
  // sluzy tylko do etykiety kierunku, ktora i tak wynika z export/import.
  const int selfUse = prod < load ? prod : load;
  const int expW = prod > load ? prod - load : 0;
  const int impW = load > prod ? load - prod : 0;
  const int span = prod > load ? prod : load;
  const int barX = lx, barY = top + 44, barW = grid::DATA_R - grid::DATA_L, barH = 8;
  s.fillRect(barX, barY, barW, barH, col::LINE);   // tor (gdy prod=load=0)
  if (span > 0) {
    int sw = static_cast<int>(barW * (selfUse / static_cast<float>(span)) + 0.5f);
    if (sw < 0) sw = 0; else if (sw > barW) sw = barW;   // twardy clamp — pasek nigdy poza tor
    // (v161) Przy starym odczycie WSZYSTKIE segmenty schodza na col::MUTE. Kolor
    // jest tu calym komunikatem (niebieski = z PV do domu, zielony = oddajemy,
    // czerwony = dobieramy z sieci), wiec przy danych po progu bylby stwierdzeniem
    // o kierunku przeplywu, ktorego akurat NIE mierzymy. Proporcje zostaja — one
    // opisuja ostatni znany stan i sa uczciwe pod podpisem "sprzed X min".
    // col::MUTE, nie col::LINE: tor paska JEST col::LINE, wiec pasek by zniknal.
    if (sw > 0) s.fillRect(barX, barY, sw, barH, pvOld ? col::MUTE : col::SELF);
    if (expW > 0) {
      s.fillRect(barX + sw, barY, barW - sw, barH, pvOld ? col::MUTE : col::OK);
    } else if (impW > 0) {
      s.fillRect(barX + sw, barY, barW - sw, barH, pvOld ? col::MUTE : col::GRID);
    }
  }

  // --- TRZY WARTOSCI POD PASKIEM (kolory = segmenty; jednostka kW z wielkiej liczby
  // wyzej, wiec wartosci bez jednostki). produkcja (zielony) · z PV do domu (niebieski)
  // · sieC: pobor (czerwony) albo oddawanie (zielony). Font f11 (waski) — trzy zmieszcza
  // sie w kolumnie danych z zapasem.
  // Wartosci WYROWNANE DO SEGMENTOW paska (wlasciciel: dawniej trzy liczby upchane od
  // lewej, kolory NIE w kolejnosci segmentow — "zle poukladane"). Teraz: "z PV" (niebieski)
  // pod LEWYM segmentem od barX; "→sieć"/"z sieci" (zielony/czerwony) do PRAWEJ krawedzi
  // paska. Produkcji calkowitej tu nie dublujemy — jest wielka liczba wyzej ("kW produkcji").
  // Gdy dom pobiera (prod<load): lewy = "z PV" (ile daje PV), prawy = "z sieci" (dobor).
  const bool exporting = expW > 0;
  char vb[10], vc[10], sb[16], sc[18];
  fmtKw(vb, sizeof(vb), selfUse / 1000.f);
  fmtKw(vc, sizeof(vc), (exporting ? expW : impW) / 1000.f);
  snprintf(sb, sizeof(sb), "z PV %s", vb);
  snprintf(sc, sizeof(sc), "%s %s", exporting ? "→sieć" : "z sieci", vc);
  plex::str(s, plex::f11(), sb, barX, top + 66, pvOld ? col::MUTE : col::SELF);
  plex::strRight(s, plex::f11(), sc, grid::DATA_R, top + 66,
                 pvOld ? col::MUTE : (exporting ? col::OK : col::GRID));

  // --- (v180) DZISIEJSZY KOSZT ZAKUPU Z SIECI ----------------------------------
  // Druga linia POD podpisem "z sieci"/"→sieć", baseline top+82 (y=194), prawa
  // krawedz DATA_R — czyli w tej samej kolumnie, co liczba, ktora opisuje.
  // Zmierzone w tablicy Plex11 (nie przymierzone): najszerszy wariant swiezy
  // "zakup dziś 999,99 zł" ma 118 px, czyli zaczyna sie na x=195. Wariant nieswiezy
  // to dwa przebiegi: "sprzed 365 dni" (84 px) + 4 px przerwy + "zakup 999,99 zł"
  // (92 px) = 180 px, czyli x=133. Kolumna danych zaczyna sie na 127 (grid::DATA_L),
  // a ciemna kolumna kontekstu konczy na 119 — zostaje 6 px do marginesu i 14 px do
  // ciemnego tla, wiec nawet wariant skrajny nie dotyka ani jednego, ani drugiego
  // (a realna kwota dobowa to jedna-dwie cyfry, nie trzy). W pionie:
  // linia wyzej siega y=179, ta zajmuje 185..195, moduleSep stoi na y=203.
  // Segment "z PV" po lewej jest LINIE WYZEJ (top+66), wiec na tej wysokosci nie ma
  // sasiada — kolizji poziomej nie ma z czym miec.
  //
  // KOLOR: col::MUTE, i to jest decyzja, a nie brak pomyslu. Linia WYZEJ niesie juz
  // kolor z kontraktu bilansu (czerwony col::GRID = dobieramy z sieci, zielony
  // col::OK = oddajemy) i to ONA jest komunikatem chwili. Dzienna suma zlotowek jest
  // KONTEKSTEM do tamtej liczby, a nie drugim alarmem: gdyby dostala ten sam czerwony,
  // blok czytalby sie jak dwa ostrzezenia zamiast jednego, a oko nie mialoby po czym
  // poznac, ktore z nich jest o TERAZ. Zadnego nowego koloru do palety nie dokladamy.
  //
  // TRZY STANY, wszystkie jawnie (regula v158, jak wszedzie indziej):
  //   (a) atMs == 0  -> NIE RYSUJEMY NIC. Pusty wiersz jest uczciwszy niz
  //                     "zakup dziś 0,00 zł", ktore twierdziloby, ze dzis nie
  //                     kupilismy ani kilowatogodziny — a to jest zdanie o domu,
  //                     nie o naszym braku danych.
  //   (b) swieze     -> "zakup dziś 4,80 zł".
  //   (c) stare      -> kwota ZOSTAJE (to wciaz jedyne, co wiemy), ale slowo "dziś"
  //                     ustepuje miejsca WIEKOWI. Ten sam zabieg, co w naglowku tego
  //                     modulu kilkadziesiat linii wyzej (v161: "dziś X kWh" ustepuje
  //                     "sprzed 14 min") i na OGRZEWANIU — wiek wchodzi w miejsce
  //                     mniej pilnej tresci, zaden nowy element sie nie pojawia.
  //                     I nie jest to kosmetyka: po polnocy bez lacznosci "dziś"
  //                     bylo by wprost falszywe, bo Home Assistant wlasnie wyzerowal
  //                     licznik, a my mamy kwote z wczoraj. Sam wiek dostaje col::WARN
  //                     — ten sam kolor "nieswieze", co przy PV i piecu.
  if (cost != nullptr && cost->atMs != 0) {
    char zl[12];
    fmt2(zl, sizeof(zl), cost->zl);
    char line[32];
    if (freshMs(cost->atMs, cfg::COST_STALE_MS)) {
      snprintf(line, sizeof(line), "zakup dziś %s zł", zl);
      plex::strRight(s, plex::f11(), line, grid::DATA_R, top + 82, col::MUTE);
    } else {
      char ago[24];
      agoWords(ago, sizeof(ago), okAgeS(cost->atMs));
      const int aw = plex::strRight(s, plex::f11(), ago, grid::DATA_R, top + 82, col::WARN);
      snprintf(line, sizeof(line), "zakup %s zł", zl);
      plex::strRight(s, plex::f11(), line, grid::DATA_R - aw - 4, top + 82, col::MUTE);
    }
  }

  (void)producing;
  (void)gridW;
}

// (v180) `cost` przechodzi TYLKO przez tu do mainPvModule — v3Main sam go nie czyta.
// Wskaznik, a nie referencja, bo warstwa danych ma prawo go jeszcze nie podpiac
// (nullptr) — dokladnie jak `air`/`auto` na pozostalych ekranach.
void v3Main(TFT_eSPI& s, const WeatherModel& w, const PvModel& pv, const CostModel* cost) {
  sceneBg(s);

  // --- kolumna kontekstu (ciemna) ---
  drawClock(s, grid::MARGIN_CTX + 2, 34, col::ONDARK);
  {
    const time_t now = time(nullptr);
    if (now >= 1700000000) {
      struct tm tmv{};
      localtime_r(&now, &tmv);
      char d[24];
      // Skrocony miesiac (kMonthShort) + baseline 48 (bylo 52): pelna data "pon 30
      // października" (112 px) wystawala poza kolumne (x=10..122) I kolidowala z gornym
      // promieniem slonca (CLEAR: r=19, srodek 62,86 -> promien siega y=51). Teraz data
      // dolem konczy sie na y=48, a promien zaczyna y=51 -> 3 px odstepu.
      snprintf(d, sizeof(d), "%s %d %s", kDowLo[tmv.tm_wday % 7], tmv.tm_mday, kMonthShort[tmv.tm_mon % 12]);
      plex::str(s, plex::f13(), d, grid::MARGIN_CTX + 2, 48, col::ONDARK_DIM);
    }
  }

  // (v158) TRZY STANY, NIE DWA. Do v157 haveWx wymagalo TAKZE swiezosci, wiec
  // prognoza starsza niz prog znikala z ekranu razem z glifem i temperatura, a na jej
  // miejsce wchodzil placeholder "prognoza / nieaktualna". To bylo uczciwe (nic nie
  // klamalo), ale WYRZUCALO informacje, ktora nadal cos znaczy: 20 minut po progu
  // temperatura sprzed godziny to wciaz lepsza odpowiedz na pytanie "ile jest na
  // dworze" niz pusty prostokat. Teraz stara wartosc ZOSTAJE, ale jest wyraznie
  // oznaczona: caly blok schodzi na ONDARK_DIM (zamiast pelnego ONDARK), a linia
  // "odczuwalna X°" ustepuje miejsca wiekowi ("sprzed 52 min") w col::WARN — kolorze,
  // ktory paleta V3 opisuje wprost jako "nieswieze / uwaga". Zero nowych wierszy:
  // wiek zajmuje wiersz, ktory i tak tam byl.
  // Placeholder (makieta 21) zostaje dla stanu, w ktorym danych NIE MA WCALE.
  const bool haveWx = w.ready && w.current.valid;
  const bool wxOld = haveWx && !wxFresh();
  const uint16_t wxMain = wxOld ? col::ONDARK_DIM : col::ONDARK;
  if (haveWx) {
    const auto& c = w.current;

    // KOLEJNOSC RYSOWANIA: najpierw LICZBA, potem GLIF (na wierzchu). Wlasciciel chcial,
    // by krople/gwiazdki z ikony DELIKATNIE ZACHODZILY na temperature (byly na wierzchu,
    // przezroczyscie — glif maluje tylko swoje piksele, tla nie zamalowuje), a NIE liczba
    // na krople. Liczbe i tak latwiej odczytac, wiec jest "pod spodem", a cienkie kreski
    // widoczne na niej. Temperatura odrobine nizej (156 zamiast 150), zeby chmura miescila
    // sie nad nia, a na sama gore liczby spadaly tylko krople.
    char big[12];
    snprintf(big, sizeof(big), "%.0f°", c.tempC);
    plex::str(s, plex::f52(), big, grid::MARGIN_CTX, 156, wxMain);

    // Ten sam wiersz (baseline 178) niesie DWIE rozne prawdy zaleznie od stanu:
    // swieza prognoza -> odczuwalna; przeterminowana -> jej wiek. Szerokosci w f13
    // policzone: "odczuwalna -12°" = 91 px, najdluzszy wariant wieku "sprzed 89 min"
    // = 80 px; kolumna kontekstu daje 104 px (od MARGIN_CTX=8 do CTX_W-8=112).
    char feels[24];
    if (wxOld) {
      agoWords(feels, sizeof(feels), wxAgeS());
      plex::str(s, plex::f13(), feels, grid::MARGIN_CTX, 174, col::WARN);
    } else {
      snprintf(feels, sizeof(feels), "odczuwalna %.0f°", c.feelsC);
      plex::str(s, plex::f13(), feels, grid::MARGIN_CTX, 174, col::ONDARK_DIM);
    }

    // Opis pogody — zawijany do dwoch linii, gdy nie miesci sie w kolumnie.
    //
    // (v183) BASELINE DRUGIEJ LINII BYL POZA OBSZAREM RYSOWANIA. Stalo tu "druga linia
    // na y=208 (<240)" i to bylo prawda, DOPOKI ekran rysowalo sie na pelnej wysokosci
    // 240 px. Po podziale na sprite (y=0..205, WeatherUi::VIEW_H) i dolny pas rysowany
    // osobno, y=208 wypada JUZ POZA sprite'em — drugie slowo bylo scinane w polowie
    // wysokosci. Zglosil wlasciciel: "slowo zachmurzenie jest uciete".
    // Caly blok jedzie wiec w gore o tyle, zeby ostatnia linia siedziala na baseline
    // 200 — tej samej, ktorej uzywaja podpisy na ekranach PV i AUTO, czyli sprawdzonej
    // (6 px zapasu na ogonki pod linia pisma). Odstep miedzy liniami zostaje 13 px,
    // rowny wysokosci fontu f13.
    const char* desc = wxico::labelForCode(c.weatherCode, !c.isDay);
    const int maxw = grid::CTX_W - 2 * grid::MARGIN_CTX;
    if (plex::width(plex::f13(), desc) <= maxw) {
      plex::str(s, plex::f13(), desc, grid::MARGIN_CTX, 190, wxMain);
    } else {
      char l1[32] = {}, l2[32] = {};
      const char* sp = strrchr(desc, ' ');
      if (sp) {
        const size_t k = static_cast<size_t>(sp - desc);
        strncpy(l1, desc, k < sizeof(l1) ? k : sizeof(l1) - 1);
        snprintf(l2, sizeof(l2), "%s", sp + 1);
      } else {
        snprintf(l1, sizeof(l1), "%s", desc);
      }
      plex::str(s, plex::f13(), l1, grid::MARGIN_CTX, 187, wxMain);
      if (l2[0]) plex::str(s, plex::f13(), l2, grid::MARGIN_CTX, 200, wxMain);
    }

    // GLIF NA WIERZCHU (po liczbie): chmura ~y60..108 (nad temperatura, ktorej gora
    // ~y107), a krople/gwiazdki spadaja z niej na sama gore liczby — zachodza delikatnie
    // i przezroczyscie, jak chcial wlasciciel.
    wx::glyph(s, c.weatherCode, !c.isDay, 62, 86, 24, false);   // cy 84->86: gorny promien slonca schodzi pod date (patrz K3)
  } else {
    // Mockup 21: pogoda nie pobrana - placeholder zamiast glifu i temperatury.
    s.drawRect(grid::MARGIN_CTX, 66, 60, 46, col::ONDARK_DIM);
    s.drawFastHLine(grid::MARGIN_CTX + 8, 130, 40, col::ONDARK_DIM);
    s.fillCircle(grid::MARGIN_CTX + 48, 130, 4, col::ONDARK_DIM);
    // (v158) Ta galaz obsluguje juz TYLKO brak danych — przeterminowana prognoza idzie
    // wyzej, ze swoja ostatnia wartoscia i wiekiem. Zostaly dwa przypadki i oba maja
    // WLASNY napis, bo to dwie rozne usterki: odpowiedz przyszla, ale bez biezacego
    // pomiaru (w.ready, current.valid == false — Open-Meteo potrafi oddac same
    // prognozy godzinowe) kontra nie doszlo nic (swiezy start, brak sieci).
    // "brak internetu" bylby klamstwem, gdy siec dziala, a milczy tylko API.
    if (w.ready) {
      plex::str(s, plex::f13(), "prognoza", grid::MARGIN_CTX, 176, col::ONDARK_DIM);
      plex::str(s, plex::f13(), "bez pomiaru", grid::MARGIN_CTX, 194, col::MUTE);
    } else {
      // "czekam na prognozę" (f13 = 118 px) wychodzilo poza ciemna kolumne (x=8..119,
      // budzet 111 px) — jasnoszary ogon na jasnym tle. Krotsze "czekam na dane" (93 px)
      // miesci sie w jednej linii, a "brak internetu" (81 px) zostaje w drugiej.
      plex::str(s, plex::f13(), "czekam na dane", grid::MARGIN_CTX, 176, col::ONDARK_DIM);
      plex::str(s, plex::f13(), "brak internetu", grid::MARGIN_CTX, 194, col::MUTE);
    }
  }

  // --- kolumna danych (jasna): OPAD 12 H ---
  // Etykieta "OPAD" bez "12 H": trzymanie w tytule "12 H" obok dopisku "za 10 h"
  // mylilo (wlasciciel: "za 10 czy za 12 h?"). 12 h to zakres WYKRESU (widac na osi
  // godzin ponizej), a dopisek mowi, KIEDY spodziewany jest szczyt opadu — podany
  // teraz jako GODZINA ZEGAROWA (np. "~22:00 · 85%"), a nie "za N h", bo godzina jest
  // jednoznaczna i nie kloci sie z "12 h" z tytulu.
  plex::str(s, plex::f11(), "OPAD", grid::DATA_L, 26, col::SECOND);
  {
    int bestProb = 0, bestHour = -1;
    for (int i = 0; i < WX_HOURS; ++i)
      if (w.hours[i].valid && w.hours[i].data.precipProb > bestProb) {
        bestProb = w.hours[i].data.precipProb;
        bestHour = w.hours[i].hourOfDay;
      }
    char hint[24];
    if (!w.ready) snprintf(hint, sizeof(hint), "jeszcze nie pobrany");
    // (v158) Zamiast slowa "nieaktualna" — WIEK. Slowo mowilo tylko tyle, ze cos jest
    // nie tak; liczba mowi, czy chodzi o kwadrans (siec sie zacina) czy o pol dnia
    // (API padlo albo router stoi). Kropka obok i tak jest juz bursztynowa.
    else if (!wxFresh()) agoWords(hint, sizeof(hint), wxAgeS());
    else if (bestProb >= 20 && bestHour >= 0)
      snprintf(hint, sizeof(hint), "~%d:00 · %d%%", bestHour, bestProb);
    else snprintf(hint, sizeof(hint), "sucho");
    int rx = grid::DATA_R;
    freshDot(s, rx - 3, 22, wxFreshState(w));
    rx -= 12;
    plex::strRight(s, plex::f13(), hint, rx, 26, col::MUTE);
  }
  precipChart(s, w, grid::DATA_L, 40, grid::DATA_W - 2 * grid::MARGIN, 34, !wxFresh());

  moduleSep(s, 92);

  // --- kolumna danych: PRAD (albo JUTRO) ---
  mainPvModule(s, w, pv, cost, 112);

  moduleSep(s, 203);
}

void v3MainBottom(TFT_eSPI& tft, const AirModel* air) {
  sceneBottom(tft);
  // Trzeci modul: POWIETRZE (mockup 01/09/17/21).
  plex::str(tft, plex::f11(), "POWIETRZE", grid::DATA_L, 226, col::SECOND);
  if (air && air->ready) {
    // (v158) Stan (c) dla powietrza: probka starsza niz cfg::AIR_SAMPLE_STALE_S (3 h)
    // dostaje plakietke w kolorze linii zamiast w kolorze klasy. Klasa ("BARDZO DOBRE")
    // zostaje — nadal jest to ostatnia znana prawda — ale traci kolor, ktory na tym
    // ekranie jest calym komunikatem (zielony = mozna otworzyc okno). Zaden nowy
    // element sie nie pojawia: ta sama pigulka, inne wypelnienie.
    const bool airOld = !airSampleFresh(*air);
    const uint16_t bc = airOld ? col::LINE : airCol(air->index);
    // Sama KLASA jakosci, bez "· liczba". Wlasciciel pytal, skad ta kropka i po co —
    // to byl separator przed wartoscia PM, ktora i tak jest na ekranie POWIETRZE.
    // Bez niej plakietka jest czysta i nie ma sierocego "·".
    const char* nm = airIndexName(air->index);
    const int tw = plex::width(plex::f11(), nm);
    const int bx = grid::DATA_R - tw - 14;
    // Plakietka wyrownana do linii "POWIETRZE": tekst na baseline 226 (jak etykieta),
    // pigulka wysrodkowana wzgledem niego — wczesniej tekst byl 2 px nizej.
    tft.fillRoundRect(bx, 215, tw + 14, 17, 5, bc);
    // Na wyszarzonej pigulce jasny napis (col::BG) znika — LINE to #BCBFB6, czyli
    // niemal ta sama jasnosc. Stary napis dostaje wiec ciemny tekst panelu.
    plex::str(tft, plex::f11(), nm, bx + 7, 226, airOld ? col::PANEL : col::BG);
  } else {
    freshDot(tft, grid::DATA_R - 3, 222, Fresh::UNKNOWN);
    plex::strRight(tft, plex::f13(), "brak danych", grid::DATA_R - 12, 226, col::MUTE);
  }
}

// --- WARIANT NOCNY (makieta 02) ------------------------------------------------
// Ciemno w pokoju + pora nocna => ekran glowny zwija sie do minimum: wielki zegar na
// czerni, jedna linia z temperatura na zewnatrz, a gdy spodziewany opad — druga linia
// "deszcz od ~HH:00" w akcencie. Dolny pas (drawV3Bottom) tez czerni sie w nocy.
void v3MainNight(TFT_eSPI& s, const WeatherModel& w) {
  s.fillRect(0, 0, grid::W, 206, 0x0000);   // pelna czern (nie col::BG)

  // Wielki zegar wysrodkowany. Bez NTP: "--:--" (font f52 nie ma myslnika — kreski recznie).
  const time_t now = time(nullptr);
  if (now >= 1700000000) {
    struct tm tmv{};
    localtime_r(&now, &tmv);
    char clk[8];
    snprintf(clk, sizeof(clk), "%d:%02d", tmv.tm_hour, tmv.tm_min);
    plex::strCenter(s, plex::f52(), clk, grid::W / 2, 116, col::BG);
  } else {
    const pltxt::FontSet f = plex::f52();
    const int dw = plex::width(f, "0");
    const int total = dw * 5;
    int cx = grid::W / 2 - total / 2;
    for (int i = 0; i < 5; ++i) {
      if (i == 2) cx += plex::str(s, f, ":", cx, 116, col::BG);
      else { s.fillRect(cx + 2, 116 - 14, dw - 4, 5, col::BG); cx += dw; }
    }
  }

  // Temperatura na zewnatrz (makieta: "12° na zewnatrz"). wxFresh: nieaktualnej pogody
  // NIE pokazujemy nawet nocnym minimalizmem — lepiej sam zegar niz stara liczba.
  if (w.ready && w.current.valid && wxFresh()) {
    char tline[24];
    snprintf(tline, sizeof(tline), "%.0f° na zewnątrz", w.current.tempC);
    plex::strCenter(s, plex::f20(), tline, grid::W / 2, 152, col::BG);
  }

  // Spodziewany opad: najblizsza godzina z realnym prawdopodobienstwem/iloscia opadu.
  if (w.ready && wxFresh()) {
    int rainHour = -1;
    for (int i = 0; i < WX_HOURS; ++i) {
      const HourSlot& h = w.hours[i];
      if (h.valid && (h.data.precipProb >= 40 || h.data.precipMm >= 0.2f)) {
        rainHour = h.hourOfDay;
        break;
      }
    }
    if (rainHour >= 0) {
      char rl[24];
      snprintf(rl, sizeof(rl), "deszcz od ~%d:00", rainHour);
      const int tw = plex::width(plex::f13(), rl);
      const int total = tw + 16;
      const int x0 = grid::W / 2 - total / 2;
      // Parasolka (prymityw): kopulka (gorna polowa kola) + trzonek z haczykiem.
      const int ux = x0 + 6, uy = 190;
      s.fillCircle(ux, uy, 6, col::ACCENT);
      s.fillRect(ux - 7, uy + 1, 15, 8, 0x0000);   // sciecie dolnej polowy -> kopulka
      s.fillRect(ux - 1, uy, 2, 8, col::ACCENT);    // trzonek
      s.drawFastHLine(ux - 3, uy + 7, 3, col::ACCENT);   // haczyk
      plex::str(s, plex::f13(), rl, x0 + 16, uy + 5, col::ACCENT);
    }
  }
}

// --- PASEK STARTOWY (makieta 07) -----------------------------------------------
// Podczas rozruchu (pogoda jeszcze niegotowa, wczesny uptime) dolny pas pokazuje pasek
// techniczny sieci zamiast POWIETRZA. Znika sam, gdy pogoda wejdzie albo minie okno
// startu — patrz drawV3Bottom. Adresy/SSID sa RUNTIME (jak na ekranie diag), nie w kodzie.
void v3StartBottom(TFT_eSPI& tft) {
  tft.fillRect(0, 206, grid::W, grid::H - 206, col::PANEL);   // ciemny, techniczny
  char b[96];
  if (WiFi.status() == WL_CONNECTED) {
    // Pasek LEWOSTRONNY (x=7) musi zmiescic sie do x=313. SSID + IP + dBm potrafi wyjsc
    // poza szerokosc, wiec przycinamy SSID (fitSsid) do budzetu = 306 px - "sieć: " -
    // niezmienna czesc " · IP · dBm". Realne SSID/IP tylko z runtime — nic w kodzie.
    char tail[40];
    snprintf(tail, sizeof(tail), " · %s · %d dBm", WiFi.localIP().toString().c_str(),
             static_cast<int>(WiFi.RSSI()));
    const int avail = (grid::W - grid::MARGIN) - grid::MARGIN - plex::width(plex::f13(), "sieć: ");
    char ssid[40];
    fitSsid(ssid, sizeof(ssid), WiFi.SSID().c_str(), avail - plex::width(plex::f13(), tail));
    snprintf(b, sizeof(b), "sieć: %s%s", ssid, tail);
  } else {
    snprintf(b, sizeof(b), "sieć: łączę z Wi-Fi...");
  }
  plex::str(tft, plex::f13(), b, grid::MARGIN, 220, col::ONDARK_DIM);
  plex::str(tft, plex::f13(), "zniknie, gdy wszystko wstanie", grid::MARGIN, 235, col::MUTE);
}

// ============================================================ RADAR ============
// Makieta 03 (wektorowa) i 20 (bez rastra).

// Skalowanie ukladu zrodlowego 320x172 na ekran 320x206 (x 1:1, y rozciagniete).
inline int mapY(int srcY) { return (srcY * 206) / coast::SRC_H; }

void v3Radar(TFT_eSPI& s, const WeatherModel& w, const RadarViewModel* rmp) {
  static const RadarViewModel kEmpty{};
  const RadarViewModel& rm = rmp ? *rmp : kEmpty;

  // --- wariant bez rastra (mockup 20): tylko pomiar punktowy nad domem ---
  if (rm.raster == nullptr || rm.frames == 0) {
    s.fillRect(0, 0, grid::W, 206, col::SEA);
    plex::str(s, plex::f11(), "RADAR", grid::MARGIN, 20, col::ONDARK);
    {
      char r[28];
      if (w.radarValid) {
        agoWords(r, sizeof(r), w.radarAgeSec);
        char full[40];
        snprintf(full, sizeof(full), "pomiar punktowy %s", r);
        int rx = grid::W - grid::MARGIN;
        // (v158) Bylo: 1200 (20 min) wpisane liczba. Teraz cfg::RADAR_STALE_MS (15 min
        // = 2,5 x kadencja 5 min). radarAgeSec to wiek POMIARU RainViewera, wiec
        // porownujemy sekundy z sekundami.
        freshDot(s, rx - 3, 16,
                 w.radarAgeSec < cfg::RADAR_STALE_MS / 1000 ? Fresh::OK : Fresh::STALE);
        rx -= 12;
        plex::strRight(s, plex::f13(), full, rx, 20, col::ONDARK_DIM);
      }
    }
    const bool rain = w.radarValid && w.radarLevel > 0;
    plex::strCenter(s, plex::f20(), rain ? "Nad domem pada" : "Nad domem nie pada",
                    grid::W / 2, 92, col::ONDARK);
    plex::strCenter(s, plex::f13(), rain ? "opad w promieniu domu" : "i w promieniu 3,5 km też nie",
                    grid::W / 2, 116, col::ONDARK_DIM);
    s.drawFastHLine(40, 138, grid::W - 80, col::BORDER);
    plex::strCenter(s, plex::f13(), "mapa opadów niedostępna w tej sesji", grid::W / 2, 160, col::MUTE);
    plex::strCenter(s, plex::f13(), "(brak PSRAM przy starcie - pomoże restart)", grid::W / 2, 178, col::MUTE);
    plex::strCenter(s, plex::f13(), "pomiar co 5 min · RainViewer", grid::W / 2, 200, col::BORDER);
    return;
  }

  // --- mapa wektorowa ---
  s.fillRect(0, 0, grid::W, 206, col::SEA);

  const int sx = rm.shiftX, sy = rm.shiftY;
  static const uint16_t kRain[6] = {0, col::RAIN4, col::RAIN3, col::RAIN2, col::RAIN, col::RAIN};

  // Warstwa opadu: iterujemy WIERSZAMI zrodla (172) i rysujemy ciagami jednakowego
  // poziomu, przeskalowane w pionie. To ~55 tys. odczytow inline (jak V1) - miesci
  // sie w budzecie klatki. Przesuniecie (sx,sy) to probkowanie zrodla (interpolacja
  // ruchu chmur miedzy klatkami); poza rastrem levelAt() zwraca 0.
  for (int row = 0; row < coast::SRC_H; ++row) {
    const int y0 = mapY(row);
    const int rh = mapY(row + 1) - y0;
    const int hh = rh > 0 ? rh : 1;
    int x = 0;
    while (x < coast::SRC_W) {
      const uint8_t lv = rm.levelAt(x - sx, row - sy);
      if (lv == 0) { ++x; continue; }
      int x2 = x + 1;
      while (x2 < coast::SRC_W && rm.levelAt(x2 - sx, row - sy) == lv) ++x2;
      s.fillRect(x, y0, x2 - x, hh, kRain[lv > 5 ? 5 : lv]);
      x = x2;
    }
  }

  // --- linia brzegowa (polilinia z CoastMap.h, na wierzchu opadu) ---
  // Segment dluzszy niz ~24 px zrodla to zamkniecie ramki wypelnienia (kadr), nie
  // wybrzeze - pomijamy je, zeby nie ciac mapy krechami wzdluz krawedzi.
  for (int sp = 0; sp < coast::SUBPATHS; ++sp) {
    const int start = pgm_read_word(&coast::START[sp]);
    const int len = pgm_read_word(&coast::LEN[sp]);
    int px = static_cast<int16_t>(pgm_read_word(&coast::XY[2 * start]));
    int py = static_cast<int16_t>(pgm_read_word(&coast::XY[2 * start + 1]));
    for (int k = 1; k < len; ++k) {
      const int idx = start + k;
      const int cx = static_cast<int16_t>(pgm_read_word(&coast::XY[2 * idx]));
      const int cy = static_cast<int16_t>(pgm_read_word(&coast::XY[2 * idx + 1]));
      const int dx = cx - px, dy = cy - py;
      if (dx * dx + dy * dy <= 576)
        s.drawLine(px, mapY(py), cx, mapY(cy), col::COAST);
      px = cx;
      py = cy;
    }
  }

  // --- Gdynia: celownik ---
  const float lat = settings().lat, lon = settings().lon;
  const int gsx = static_cast<int>((lon - gmapr::LON_MIN) / (gmapr::LON_MAX - gmapr::LON_MIN) * coast::SRC_W);
  const int gsy = static_cast<int>((gmapr::LAT_MAX - lat) / (gmapr::LAT_MAX - gmapr::LAT_MIN) * coast::SRC_H);
  const int gx = gsx, gy = mapY(gsy);
  s.drawCircle(gx, gy, 6, col::BG);
  s.drawCircle(gx, gy, 5, col::ACCENT);
  s.drawFastHLine(gx - 9, gy, 6, col::ACCENT);
  s.drawFastHLine(gx + 4, gy, 6, col::ACCENT);
  s.drawFastVLine(gx, gy - 9, 6, col::ACCENT);
  s.drawFastVLine(gx, gy + 4, 6, col::ACCENT);
  s.fillCircle(gx, gy, 1, col::BG);

  // --- naglowek na mapie ---
  const int back = rm.frames > 0 ? (rm.frames - 1) * 10 : 120;
  char rest[28];
  snprintf(rest, sizeof(rest), "· -%d min → teraz", back);
  const int rw = plex::str(s, plex::f11(), "RADAR", grid::MARGIN, 20, col::ONDARK);
  plex::str(s, plex::f13(), rest, grid::MARGIN + rw + 6, 20, col::ONDARK_DIM);

  {
    // Wiek najnowszej klatki (frameEpoch) - "pomiar sprzed X min".
    // (v158) BLAD DO NAPRAWY, nie kosmetyka: kropka stanu pokazywala `rm.hasRain`,
    // czyli "czy na mapie jest opad", a nie "czy mapa jest swieza". W praktyce
    // znaczyla wiec: sucho -> bursztynowa (alarm o niczym), ulewa sprzed dwoch
    // godzin -> zielona (klamstwo). Zielona kropka ma znaczyc JEDNO: dane sa
    // biezace. Teraz liczy sie od frameEpoch, tak samo jak napis obok, i przez ten
    // sam prog cfg::RADAR_MAP_STALE_S (30 min = 2,5 x klatka 10-minutowa).
    // frameEpoch == 0 albo brak NTP = nie wiemy, ile ma lat -> UNKNOWN (brak kropki),
    // a nie zgadywanie w ktoras strone.
    char age[28] = "pomiar";
    Fresh mapFresh = Fresh::UNKNOWN;
    if (rm.frameEpoch > 0) {
      const time_t now = time(nullptr);
      if (now > 1700000000 && now >= static_cast<time_t>(rm.frameEpoch)) {
        const uint32_t ageS = static_cast<uint32_t>(now - static_cast<time_t>(rm.frameEpoch));
        char a[24];
        agoWords(a, sizeof(a), ageS);
        snprintf(age, sizeof(age), "pomiar %s", a);
        mapFresh = ageS < cfg::RADAR_MAP_STALE_S ? Fresh::OK : Fresh::STALE;
      }
    }
    int rx = grid::W - grid::MARGIN;
    freshDot(s, rx - 3, 16, mapFresh);
    rx -= 12;
    plex::strRight(s, plex::f13(), age, rx, 20,
                   mapFresh == Fresh::STALE ? col::WARN : col::ONDARK_DIM);
  }
  plex::str(s, plex::f13(), "brzeg · polilinia · Gdynia ~300 km", grid::MARGIN, 36, col::BORDER);

  // Plakietka opadu (gdy pada) - prawy dolny rog mapy.
  if (rm.hasRain) {
    const char* lbl = "opad na mapie";
    const int tw = plex::width(plex::f13(), lbl);
    const int bx = grid::W - grid::MARGIN - tw - 16;
    s.fillRoundRect(bx, 182, tw + 16, 18, 4, col::PANEL);
    plex::str(s, plex::f13(), lbl, bx + 8, 195, col::ONDARK);
  }
}

void v3RadarBottom(TFT_eSPI& tft, const RadarViewModel* rmp) {
  static const RadarViewModel kEmpty{};
  const RadarViewModel& rm = rmp ? *rmp : kEmpty;
  tft.fillRect(0, 206, grid::W, 34, col::PANEL);   // caly pas ciemny (spojnie z mapa)

  plex::str(tft, plex::f10(), "−120 min", grid::MARGIN, 226, col::ONDARK_DIM);
  {
    const char* nowL = "teraz";
    const int tw = plex::width(plex::f10(), nowL);
    plex::str(tft, plex::f10(), nowL, grid::W - grid::MARGIN - tw - 10, 226, col::ONDARK_DIM);
    tft.fillRect(grid::W - grid::MARGIN - 8, 219, 8, 8, col::RAIN2);
  }

  // Os czasu: kropki klatek, biezaca wyrozniona.
  const int n = rm.frames > 0 ? rm.frames : radarmap::FRAMES;
  const int ax0 = grid::MARGIN + 56, ax1 = grid::W - grid::MARGIN - 44;
  tft.drawFastHLine(ax0, 223, ax1 - ax0, col::BORDER);
  for (int i = 0; i < n; ++i) {
    const int x = ax0 + (n > 1 ? (i * (ax1 - ax0)) / (n - 1) : 0);
    const bool cur = (i == rm.frameIdx);
    if (cur) tft.fillCircle(x, 223, 3, col::ACCENT);
    else tft.fillCircle(x, 223, 1, col::ONDARK_DIM);
  }
}

// ============================================================ 5 DNI ============
// Makieta 04. Pelnojasne tlo, 5 wierszy + wspolna skala tygodnia.

void v3Days(TFT_eSPI& s, const WeatherModel& w) {
  s.fillRect(0, 0, grid::W, 206, col::BG);
  // Prognoza NIEAKTUALNA (>30 min bez udanego pobrania) liczy sie jak brak: wiersze
  // pokazuja nazwy dni, ale wartosci -> "-" (font nie ma dlugiego myslnika U+2014, wiec
  // uzywamy tego samego znaku, co placeholder braku opadu nizej), a kropka naglowka STALE.
  // Nie dotyczy to przypadku "nigdy nie pobrano" (n==0 nizej -> "Pobieram prognozę...").
  const bool fresh = w.ready && wxFresh();
  char hr[24] = "";
  // UWAGA: w.updatedAt to SAMO "HH:MM" (5 znakow), a NIE znacznik ISO — wypelnia je
  // hhmmFromEpoch() w WeatherClient.cpp (snprintf "%02d:%02d"). Bylo tu "%.5s" z
  // w.updatedAt + 11, czyli offset godziny w ISO "2026-07-27T07:15" — model tego formatu
  // nigdy nie przechowywal, wiec +11 celowalo za terminator, w wyzerowany ogon bufora
  // char[20], i naglowek pokazywal samo "prognoza z". Bierzemy caly string, tak jak V1
  // (WeatherUi.cpp). NIE "poprawiac" z powrotem na offset.
  if (w.updatedAt[0]) snprintf(hr, sizeof(hr), "prognoza z %s", w.updatedAt);
  lightHeader(s, "5 DNI", fresh ? hr : nullptr, wxFreshState(w));

  int n = 0;
  float wkMin = 1e9f, wkMax = -1e9f;
  for (int i = 0; i < WX_DAYS; ++i)
    if (w.days[i].valid) {
      ++n;
      if (w.days[i].tempMin < wkMin) wkMin = w.days[i].tempMin;
      if (w.days[i].tempMax > wkMax) wkMax = w.days[i].tempMax;
    }
  if (n == 0) {
    plex::strCenter(s, plex::f20(), "Pobieram prognozę...", grid::W / 2, 110, col::MUTE);
    return;
  }
  if (wkMax - wkMin < 2.f) { wkMin -= 1.f; wkMax += 1.f; }

  // Nad kolumna opadu zostaje sam "opad". Opisu min/max tu juz nie ma, bo po przebudowie
  // wiersza znaczenie niesie POZYCJA liczby: stoi przy tym koncu paska, ktory oznacza
  // (lewo = wkMin = chlodniej, prawo = wkMax = cieplej — ta sama skala co w stopce).
  // Naglowka opisowego juz raz probowano i nie zadzialal: f10 w MUTE, dwa wiersze wyzej,
  // nie laczy sie w oku z liczbami (wlasciciel pytal o nie drugi raz JUZ po jego dodaniu).
  // Zostawienie "max min" nad kolumna, w ktorej tych liczb juz nie ma, byloby gorsze
  // niz brak opisu — myliloby zamiast tlumaczyc.
  plex::strRight(s, plex::f10(), "opad", grid::DATA_R, 46, col::MUTE);

  // UKLAD WIERSZA: liczba stoi przy tym koncu paska, ktory oznacza — min po LEWEJ,
  // max po PRAWEJ. Pasek biegnie po wspolnej skali tygodnia (lewa krawedz = wkMin,
  // prawa = wkMax; stopka to powtarza), wiec "lewo = zimniej, prawo = cieplej" czyta
  // sie bez podpisu. Ten sam zabieg, co przy pasku energii na ekranie glownym w v143:
  // wartosc wyrownana do tego, co opisuje, zamiast upchana w jednej kolumnie obok.
  //   [DZIEN f20] [glif] [min f13] [====pasek====] [MAX f20]        [opad f13]|DATA_R
  // Granice liczymy RAZ przed petla, na zywo z plex::width i dla NAJSZERSZYCH
  // przypadkow — pasek musi miec identyczna geometrie w kazdym wierszu, bo to wspolna
  // skala. Najszersze: skrot dnia z kDowHi ("CZW"), zimowe "-19°" (snprintf "%.0f°"
  // przy -18,6) i dwucyfrowy opad "88 mm".
  const int precipL = grid::DATA_R - plex::width(plex::f13(), "88 mm");
  const int minW = plex::width(plex::f13(), "-19°");
  const int maxW = plex::width(plex::f20(), "-19°");
  int dowW = 0;
  for (const char* nm : kDowHi) {
    const int nw = plex::width(plex::f20(), nm);
    if (nw > dowW) dowW = nw;
  }
  // Glif NIE jest kwadratem o boku r: przy r=9 tarcza slonca ma promien r*80/100 = 7,
  // a promienie siegaja jeszcze 7*70/100 + 3 dalej — lacznie +-14 px od srodka
  // (ThemeV3.cpp, sunOrMoon). Srodek wyliczamy, zamiast trzymac dawne 78, bo glif musi
  // sie cofnac w lewo, zeby zrobic miejsce na kolumne min przed paskiem.
  const int glyphR = 9, glyphHalf = 14;
  const int glyphCx = grid::MARGIN + dowW + 7 + glyphHalf;   // 7 px odstepu od nazwy dnia
  const int barX = glyphCx + glyphHalf + 8 + minW + 6;       // 8 px za glifem, 6 px min->pasek
  const int barW = precipL - 12 - maxW - 8 - barX;           // 8 px pasek->max, 12 px max->opad

  const time_t now = time(nullptr);
  const int rowY0 = 56, pitch = 30;
  int r = 0;
  for (int i = 0; i < WX_DAYS; ++i) {
    if (!w.days[i].valid) continue;
    const auto& d = w.days[i];
    const int y = rowY0 + r * pitch;

    // Dzien tygodnia z daty (gwarantuje polskie znaki), zapas: d.name.
    char dow[6];
    if (now > 1700000000) {
      const time_t dd = now + static_cast<time_t>(d.dayOffset) * 86400;
      struct tm tmv{};
      localtime_r(&dd, &tmv);
      snprintf(dow, sizeof(dow), "%s", kDowHi[tmv.tm_wday % 7]);
    } else {
      snprintf(dow, sizeof(dow), "%s", d.name[0] ? d.name : "-");
    }
    plex::str(s, plex::f20(), dow, grid::MARGIN, y + 8, col::PANEL);

    // Glif i KOLOROWY pasek tylko dla swiezej prognozy — nieaktualna zostawia sam
    // wyszarzony tor (bez sugerowania konkretnej, starej temperatury).
    if (fresh) wx::glyph(s, d.weatherCode, false, glyphCx, y + 4, glyphR, true);

    // Pasek temperatury na wspolnej skali (plaski kolor wg tempMax). barX/barW policzone
    // raz przed petla, wiec kazdy wiersz ma dokladnie te sama geometrie skali.
    const int xa = barX + static_cast<int>((d.tempMin - wkMin) / (wkMax - wkMin) * barW);
    const int xb = barX + static_cast<int>((d.tempMax - wkMin) / (wkMax - wkMin) * barW);
    s.fillRect(barX, y + 1, barW, 6, col::LINE);
    if (fresh) s.fillRoundRect(xa, y, (xb - xa > 4 ? xb - xa : 4), 8, 3, tempCol(d.tempMax));

    // KOLUMNA OPADU zostaje STALA przy prawej krawedzi — wiedza kupiona bledem:
    // dwucyfrowy opad ("11 mm") wjezdzal kiedys na temperature, wiec rezerwujemy
    // szerokosc "88 mm" i lewa granica precipL nie zalezy od tego, czy opad ma 1 czy
    // 2 cyfry ani czy w ogole jest. Rezerwacja obowiazuje dalej: barW policzono wyzej
    // tak, zeby najszerszy max konczyl sie PRZED precipL (12 px zapasu).
    char pm[10];
    const bool hasP = fresh && d.precipMm >= 0.1f;
    if (!fresh)    snprintf(pm, sizeof(pm), "-");   // prognoza nieaktualna -> myslnik
    else if (hasP) snprintf(pm, sizeof(pm), "%.0f mm", d.precipMm);
    else           snprintf(pm, sizeof(pm), "-");
    plex::strRight(s, plex::f13(), pm, grid::DATA_R, y + 8, hasP ? col::RAIN : col::MUTE);

    char hi[8], lo[8];
    if (fresh) {
      snprintf(hi, sizeof(hi), "%.0f°", d.tempMax);
      snprintf(lo, sizeof(lo), "%.0f°", d.tempMin);
    } else {
      snprintf(hi, sizeof(hi), "-");
      snprintf(lo, sizeof(lo), "-");
    }
    // STALE KOLUMNY — wzorzec: widzet pogody Apple. Liczby NIE wedruja za kolorowym
    // segmentem (probowano tego w v148 i wyszedl balagan: przy kazdym wierszu liczby
    // staly gdzie indziej, a wycierane przerwy siekaly tor na kawalki). Wiersz ma cztery
    // twarde krawedzie, te same we wszystkich wierszach: min|120, tor 126..211, max|261,
    // opad|313. Segment plywa w srodku toru i tylko on niesie informacje o polozeniu.
    // Min i max wyrownane DO PRAWEJ — dzieki temu "9°", "29°" i "-19°" koncza sie w tym
    // samym miejscu i kolumna jest kolumna, a nie postrzepiona krawedzia.
    // Baseline y + 8 bez zmian (wspolny z nazwa dnia).
    plex::strRight(s, plex::f13(), lo, barX - 6, y + 8, col::MUTE);
    plex::strRight(s, plex::f20(), hi, precipL - 12, y + 8, fresh ? col::PANEL : col::MUTE);
    ++r;
  }
}

void v3DaysBottom(TFT_eSPI& tft, const WeatherModel& w) {
  tft.fillRect(0, 206, grid::W, 34, col::BG);
  float wkMin = 1e9f, wkMax = -1e9f;
  bool any = false;
  for (int i = 0; i < WX_DAYS; ++i)
    if (w.days[i].valid) {
      any = true;
      if (w.days[i].tempMin < wkMin) wkMin = w.days[i].tempMin;
      if (w.days[i].tempMax > wkMax) wkMax = w.days[i].tempMax;
    }
  tft.drawFastHLine(grid::MARGIN, 210, grid::W - 2 * grid::MARGIN, col::LINE);
  // Nieaktualna prognoza -> pusta stopka (bez starej skali min/max), spojnie z "-" w wierszach.
  if (!any || !(w.ready && wxFresh())) return;
  char lo[8], hi[8];
  snprintf(lo, sizeof(lo), "%.0f°", wkMin);
  snprintf(hi, sizeof(hi), "%.0f°", wkMax);
  plex::str(tft, plex::f13(), lo, grid::MARGIN, 228, col::SECOND);
  plex::strRight(tft, plex::f13(), hi, grid::W - grid::MARGIN, 228, col::SECOND);
  plex::strCenter(tft, plex::f11(), "SKALA WSPÓLNA DLA TYGODNIA", grid::W / 2, 227, col::MUTE);
}

// ============================================================ PRAD =============
// (v164) Uklad "bilans + wykres" na wzor panelu FusionSolar — wariant B z makiety,
// zaakceptowany przez wlasciciela. Ewolucja makiety 05: naglowek i stany swiezosci
// z v158/v161 bez zmian, gorny blok scisniety, w srodku DWA PASKI BILANSU DNIA
// (PV: zuzyte/oddane, DOM: z PV/z sieci), nizej ISTNIEJACY wykres doby, tylko nizszy.

// --- BILANS DNIA [kWh] scalkowany z profilu doby --------------------------------
// Falownik daje przez Modbus tylko produkcje dzienna (rej. 32114) i moce CHWILOWE
// — zadnego licznika energii ODDANEJ/POBRANEJ dzis. Calkujemy wiec PvHistory:
// 144 sloty po 10 min, w slocie OSTATNI udany odczyt mocy (kadencja dzienna 30 s),
// kWh = W * (1/6) h / 1000 = W / 6000. Metoda prostokatow na probce chwilowej —
// przy 10-minutowym slocie blad pojedynczego slotu bywa spory (chmura na 9 minut),
// ale w sumie dnia sie usrednia; obie serie (watts/load) sa probkowane W TEJ SAMEJ
// chwili i zerowane RAZEM o polnocy (PvHistory::push -> reset), wiec min/roznica
// na parze probek jest spojna. Petla 144 iteracji na klatke — tania, bez cache.
// NOC: Huawei wylacza Modbus TCP po zachodzie, wiec nocne sloty sa PUSTE i nocny
// pobor domu (~0,1-0,3 kW) NIE wchodzi do "z sieci" — pasek DOM liczy tylko czesc
// doby, w ktorej falownik odpowiadal. Uczciwiej tego z dostepnych rejestrow nie
// policzymy (licznik energii pobranej wymagalby nowego rejestru — nie dokladamy).
struct PvDayKwh {
  float selfPv;    // energia PV zuzyta na miejscu: sum(min(prod, pobor))
  float fromGrid;  // energia dobrana z sieci:      sum(max(pobor - prod, 0))
};
PvDayKwh pvDayEnergy(const PvHistory& h) {
  int32_t selfSum = 0, impSum = 0;   // [W * slot]; max 144 * 65535 < 2^24, bez przepelnienia
  for (int i = 0; i < PvHistory::SLOTS; ++i) {
    if (!h.filled[i]) continue;
    const int32_t p = h.watts[i], l = h.load[i];
    selfSum += p < l ? p : l;
    if (l > p) impSum += l - p;
  }
  return {selfSum / 6000.f, impSum / 6000.f};
}

// --- (v165) BILANS DNIA GOTOWY DO POKAZANIA -------------------------------------
// JEDNO miejsce, w ktorym rozstrzyga sie "miernik czy calka", bo te liczby czytaja
// DWIE funkcje rysujace: paski w v3Pv() i procent autokonsumpcji w v3PvBottom().
// Gdyby kazda wybierala zrodlo sama, wystarczylaby jedna rozjechana galaz, zeby
// pasek pokazywal 70%, a podpis pod wykresem 55% — na jednym ekranie, w tej samej
// klatce. Sygnatura bierze SNAPSHOT, nie caly PvModel, bo o zrodle decyduja
// wylacznie dane, a nie stan swiezosci.
//
// CO JEST MIERZONE, A CO WYLICZANE (przy `fromMeter`):
//   mierzone:  produkcja dzis (rej. 32114), ODDANE dzis (licznik 37119 minus baza
//              z polnocy), POBRANE dzis (licznik 37121 minus baza);
//   wyliczane: zuzycie wlasne = produkcja − oddane,
//              zuzycie domu   = zuzycie wlasne + pobrane.
// Dokladnie taki uklad ma panel FusionSolar wlasciciela i dlatego jego liczby sie
// domykaja: 27,43 = 20,85 + 6,58 oraz 34,38 = 20,85 + 13,53.
//
// DLACZEGO ZUZYCIE WLASNE JEST WYLICZANE, A NIE MIERZONE: nie ma go czym zmierzyc.
// Miernik stoi na ZLACZU Z SIECIA i widzi wylacznie dwa kierunki przeplywu przez to
// zlacze; energia z paneli, ktora do zlacza nigdy nie dotarla, jest dla niego
// niewidzialna. Za to wychodzi z odejmowania DWOCH liczb calkowanych sprzetowo,
// wiec nie dziedziczy bledu naszego probkowania co 10 minut — inaczej niz calka
// z v164, ktora byla dotad jedynym zrodlem tej pozycji.
//
// SPOJNOSC SUM WYMUSZONA KONSTRUKCYJNIE, nie sprawdzana po fakcie: `selfPv`
// powstaje jako `pvToday − toGrid`, wiec pasek "PV DZIŚ" sumuje sie do swojego
// tytulu z definicji; `home` powstaje jako `selfPv + fromGrid`, wiec pasek "DOM
// DZIŚ" tak samo. Przy okazji obie pozycje "z PV" to teraz TA SAMA liczba —
// w v164 nie byly (patrz galaz fallbacku), bo kazdy pasek liczyl ja inna droga.
struct PvBalance {
  float pvToday;   // produkcja dzis [kWh], rej. 32114
  float selfPv;    // zuzyte na miejscu
  float toGrid;    // oddane do sieci
  float fromGrid;  // pobrane z sieci
  float home;      // zuzycie domu = selfPv + fromGrid
  bool fromMeter;  // true = liczniki miernika, false = calka profilu doby (v164)
};

PvBalance pvBalance(const PvSnapshot& d, const PvHistory& h) {
  PvBalance b{};
  b.pvToday = d.energyTodayKwh > 0.f ? d.energyTodayKwh : 0.f;

  // FALLBACK JEST OBOWIAZKOWY: `meterTodayOk` jest false, gdy miernika nie ma,
  // baza z polnocy jest niepelna (pierwszy dzien po aktualizacji, brak NTP, start
  // w srodku dnia) albo licznik sie cofnal. Ekran liczy wtedy DOKLADNIE jak v164.
  // Milczacy miernik nie moze wygasic ekranu.
  if (d.meterTodayOk) {
    // Klamra `oddane <= produkcja`: normalnie licznik oddania nie przekroczy
    // produkcji dnia, ale obie liczby maja WLASNE zera doby — 32114 zeruje falownik
    // o swojej polnocy, nasza baza o polnocy lokalnego zegara. Przy rozjezdzie tych
    // chwil (albo gdyby do zlacza dolozylo sie kiedys inne zrodlo) reszta wyszlaby
    // ujemna. Klamrujemy ODDANE, bo to ono jest wtedy wieksze, niz powinno,
    // i dzieki temu `selfPv >= 0` bez osobnego sprawdzania.
    b.toGrid = d.meterTodayExportKwh < b.pvToday ? d.meterTodayExportKwh : b.pvToday;
    b.selfPv = b.pvToday - b.toGrid;
    b.fromGrid = d.meterTodayImportKwh;
    b.fromMeter = true;
  } else {
    // Sciezka v164. UWAGA: tu zuzycie wlasne jest MIERZONE (calka), a oddane
    // wychodzi z odejmowania — czyli odwrotnie niz wyzej. Klamra tez jest po
    // drugiej stronie: przycinamy zuzyte do rejestru 32114, bo licznik falownika
    // jest wiarygodniejszy niz nasza calka.
    const PvDayKwh de = pvDayEnergy(h);
    b.selfPv = de.selfPv < b.pvToday ? de.selfPv : b.pvToday;
    b.toGrid = b.pvToday - b.selfPv;
    b.fromGrid = de.fromGrid;
    // (v164) Tytul paska "DOM DZIŚ" bral SUROWA calke `de.selfPv`, a pasek "PV
    // DZIŚ" te sama liczbe PRZYCIETA — wiec oba paski potrafily podac inne "z PV".
    // Kazdy sumowal sie do wlasnego tytulu i to bylo wtedy wazniejsze. Teraz
    // uzywamy przycietej w obu miejscach: suma nadal sie zgadza (bo `home` liczymy
    // z tej samej liczby), a ekran przestaje podawac dwie wartosci jednej rzeczy.
    b.fromMeter = false;
  }
  b.home = b.selfPv + b.fromGrid;
  return b;
}

// Pasek bilansu dnia: dwa segmenty na pelnej szerokosci kolumny (306 px, h=11),
// procent w segmencie tylko gdy sie miesci (szerokosc tekstu + 8 px luzu).
// `hasData` false (produkcja/zuzycie ~0 rano albo zima) -> sam tor col::LINE,
// zadnych segmentow ani procentow — pusty pasek zamiast dzielenia przez zero.
// `muted` (dane stare, wzorzec v158): segment A col::MUTE, segment B col::LINE —
// podzial wciaz widac, ale kolor-komunikat ("zielone=dobre", "czerwone=platne")
// jest zdjety; procenty tez znikaja, bo sa czescia tego samego komunikatu.
// txtA/txtB: kolor procentu dobrany do jasnosci segmentu — jeden kontrast, zero
// zgadywania w miejscu wywolania. (v167) Po zmianie palety pasków przeliczone
// wprost ze wzoru WCAG na tle #F4F4F0 (BG) i #1A1C1E (PANEL):
//   SELF #3D78C4 (Y=0,182): BG 4,09 : 1  vs PANEL 3,80 : 1  -> BG
//   OK   #4D9A4D (Y=0,253): BG 3,13 : 1  vs PANEL 4,96 : 1  -> PANEL
//   GRID #C04A3A (Y=0,164): BG 4,44 : 1  vs PANEL 3,50 : 1  -> BG
// Przy okazji kontrast W PASKU "PV DZIŚ" WZROSL: do v166 lewy segment byl zielony
// z jasnym procentem (3,13), a teraz jest niebieski z jasnym (4,09); prawy byl
// jasnozielony OK2 z ciemnym, a jest zielony z ciemnym (4,96).
void dayBar(TFT_eSPI& s, int y, float frac, bool hasData, bool muted,
            uint16_t colA, uint16_t colB, uint16_t txtA, uint16_t txtB) {
  const int bx = grid::MARGIN, bw = grid::W - 2 * grid::MARGIN, bh = 11;
  s.fillRect(bx, y, bw, bh, col::LINE);   // tor
  if (!hasData) return;
  const float f = clampf(frac, 0.f, 1.f);
  int sw = static_cast<int>(bw * f + 0.5f);
  if (sw < 0) sw = 0; else if (sw > bw) sw = bw;   // twardy clamp jak w mainPvModule
  if (sw > 0) s.fillRect(bx, y, sw, bh, muted ? col::MUTE : colA);
  if (sw < bw) s.fillRect(bx + sw, y, bw - sw, bh, muted ? col::LINE : colB);
  if (muted) return;
  const int pA = static_cast<int>(f * 100.f + 0.5f);
  char pb[16];
  snprintf(pb, sizeof(pb), "%d%%", pA);
  // Baseline y+9: cyfry f10 (7 px) siedza w pasku y..y+10 z ~2 px swiatla u gory.
  if (plex::width(plex::f10(), pb) + 8 <= sw)
    plex::strCenter(s, plex::f10(), pb, bx + sw / 2, y + 9, txtA);
  snprintf(pb, sizeof(pb), "%d%%", 100 - pA);
  if (plex::width(plex::f10(), pb) + 8 <= bw - sw)
    plex::strCenter(s, plex::f10(), pb, bx + sw + (bw - sw) / 2, y + 9, txtB);
}

void v3Pv(TFT_eSPI& s, const PvModel& pv, const PvHistory& hist) {
  s.fillRect(0, 0, grid::W, 206, col::BG);

  // (v161) TRZY STANY, DOKLADNIE TE Z v158 — teraz mozliwe takze tutaj.
  // Do v160 netTask przy bledzie NADPISYWAL caly gPv pustym modelem (`gPv = tmp`
  // bez warunku), wiec "mamy stare dane" po prostu nie istnialo: ostatnia znana moc
  // znikala razem z pierwsza nieudana proba. Od v161 blok PV w pogoda-gdynia.ino
  // zostawia `gPv.data` nietkniete i zmienia tylko online/asleep/errorMsg, wiec:
  //   (a) !pv.data.valid            -> NIGDY nie bylo udanego odczytu ("nie odpowiada"
  //                                    jest tu PRAWDA i zostaje);
  //   (b) online && wiek < prog     -> dane biezace, pelne kolory;
  //   (c) mamy dane, ale stare      -> te same liczby, ale WYCISZONE i podpisane
  //                                    wiekiem — wzorzec ekranu glownego z v158.
  // Prog z Config.h: cfg::PV_STALE_MS (90 s) w dzien, cfg::PV_STALE_NIGHT_MS (15 min)
  // gdy falownik spi. Wiek liczony od diag().pvOkAt, ktore rusza sie WYLACZNIE po
  // udanym odczycie.
  const uint32_t pvAge = okAgeS(diag().pvOkAt);
  const bool pvEver = pv.data.valid;
  const bool pvFresh = pv.online && freshMs(diag().pvOkAt, pv.asleep ? cfg::PV_STALE_NIGHT_MS
                                                                    : cfg::PV_STALE_MS);
  // `!pv.asleep` w definicji "stare": sen falownika to stan NEUTRALNY, nie awaria
  // i nie przeterminowanie. Ma wlasny, prawdziwy ekran ("śpi") kilka linii nizej,
  // a nocna produkcja NAPRAWDE wynosi zero — pokazanie tam wyciszonej mocy sprzed
  // zachodu byloby dokladnie tym klamstwem, ktore to wydanie usuwa.
  const bool pvOld = pvEver && !pvFresh && !pv.asleep;

  // Dopisek w naglowku. Przy starych danych — WIEK (jak "sprzed 52 min" na ekranie
  // glownym), przy swiezych — godzina odczytu.
  // (v161) "odczyt HH:MM" bralo dotad time(nullptr) W CHWILI RYSOWANIA, czyli
  // pokazywalo BIEZACY zegar podpisany slowem "odczyt" — przy zawieszonym netTasku
  // ten napis szedl do przodu co minute, choc nowych danych nie bylo od godziny.
  // Teraz odejmujemy wiek: to jest naprawde godzina ostatniego UDANEGO odczytu.
  char hr[24] = "";
  if (pvOld) {
    agoWords(hr, sizeof(hr), pvAge);
  } else if (pvEver) {
    const time_t now = time(nullptr);
    if (now > 1700000000) {
      const time_t readAt = now - static_cast<time_t>(pvAge);
      struct tm tmv{};
      localtime_r(&readAt, &tmv);
      snprintf(hr, sizeof(hr), "odczyt %02d:%02d", tmv.tm_hour, tmv.tm_min);
    }
  }
  // Kolejnosc wazna i CELOWO taka: sen > wiek > godzina odczytu > "offline".
  // Puste `hr` (mamy dane, ale NTP jeszcze nie dal czasu, wiec nie umiemy podac
  // godziny) NIE MOZE zjechac do "offline" — falownik wtedy odpowiada, milczy zegar.
  const char* right = nullptr;
  if (pv.asleep) right = "śpi - noc";
  else if (hr[0]) right = hr;
  else if (!pvEver) right = "offline";
  lightHeader(s, "PRĄD", right,
              pvFresh ? Fresh::OK : (pv.asleep || !pvEver ? Fresh::UNKNOWN : Fresh::STALE));

  // Stan (a): nigdy nie bylo odczytu. Falownik nieskonfigurowany, zly adres, pierwsze
  // sekundy po starcie — tu naprawde nie ma czego pokazac i napis jest prawda.
  if (!pvEver && !pv.asleep) {
    plex::strCenter(s, plex::f20(), "Falownik nie odpowiada", grid::W / 2, 96, col::MUTE);
    plex::strCenter(s, plex::f13(), diag().pvErr[0] ? diag().pvErr : "sprawdź połączenie",
                    grid::W / 2, 120, col::MUTE);
    return;
  }
  if (pv.asleep) {
    wx::glyph(s, 0, true, 60, 96, 18, true);
    plex::str(s, plex::f52(), "śpi", 96, 118, col::MUTE);
    plex::str(s, plex::f13(), "falownik milczy po zachodzie (noc)", grid::MARGIN, 150, col::MUTE);
    return;
  }

  // (v161) Wyciszenie stanu (c). Moc, pobor domu i bilans sieci to wielkosci
  // CHWILOWE — dlatego ich prog swiezosci to 90 s, a nie kwadrans. Gdy sa stare,
  // pokazujemy je nadal (to wciaz jedyne, co o instalacji wiemy), ale ZDEJMUJEMY
  // im kolor: kolor niesie tu znaczenie ("oddajemy do sieci" na zielono, "dobieramy"
  // na czerwono), a przy danych sprzed pol godziny bylby obietnica bez pokrycia.
  // Zostaje ksztalt i liczba, znika sugestia, ze to dzieje sie TERAZ. Dokladnie ta
  // sama decyzja, co przy slupkach opadu w v158 (precipChart, parametr `muted`).
  const uint16_t cMain = pvOld ? col::MUTE : col::PANEL;
  const uint16_t cSec = pvOld ? col::MUTE : col::SECOND;

  // --- GORNY BLOK (y=34..66, scisniety pod paski bilansu) -----------------------
  // Wielka moc AC (f52, baseline 66 — cyfry 28..66, tuz pod linia naglowka y=30).
  // Przy prod >= 9,95 kW bez dziesiatych: "12,3" ma w f52 117 px i razem z "kW"
  // (f20, 32 px) konczyloby sie na x=161, wjezdzajac w kolumne metryk (x=141).
  // "12" ma 66 px -> "kW" konczy sie na x=110, zostaje >=31 px luzu; strata 0,1 kW
  // przy >=10 kW to <1% i ponizej dokladnosci chwilowego odczytu.
  const int prod = pv.data.powerAcW;
  const int load = pv.data.houseLoadW;
  const int gridW = pv.data.gridPowerW;
  char big[16];
  if (prod >= 9950) snprintf(big, sizeof(big), "%.0f", prod / 1000.f);
  else fmt1(big, sizeof(big), prod / 1000.f);
  const int bw = plex::str(s, plex::f52(), big, grid::MARGIN, 66, cMain);
  plex::str(s, plex::f20(), "kW", grid::MARGIN + bw + 5, 66, cSec);

  // Prawa krawedz (wiersze 46/64): energia dzienna (rej. 32114) i licznik zycia
  // (rej. 32106). "dziś 99,9 kWh" (77 px) zaczyna sie na x=236. Rysowane PRZED
  // metrykami sieci, bo szerokosc "łącznie" wyznacza budzet metryki w tym samym
  // wierszu (patrz kaskada nizej).
  char ev[12];
  fmt1(ev, sizeof(ev), pv.data.energyTodayKwh);
  char e2[28];
  snprintf(e2, sizeof(e2), "dziś %s kWh", ev);
  plex::strRight(s, plex::f13(), e2, grid::DATA_R, 46, col::MUTE);
  char tot[28];
  snprintf(tot, sizeof(tot), "łącznie %.0f", pv.data.energyTotalKwh);
  const int totW = plex::strRight(s, plex::f13(), tot, grid::DATA_R, 64, col::MUTE);

  // Metryki (x=141, wiersze 46/64): pobor domu + kierunek sieci. Semantyka kolorow
  // jak dotad: "do sieci +" zielony (oddajemy), "z sieci" czerwony (kupujemy) —
  // znak w napisie tylko przy oddawaniu (mockup B), kierunek poboru niesie slowo
  // i kolor. Wiersz 46: "dom 19,9 kW" (74 px) konczy sie na x=215, "dziś 99,9 kWh"
  // zaczyna na x=236 -> 21 px luzu.
  char lv[12];
  fmt1(lv, sizeof(lv), load / 1000.f);
  char l2[28];
  snprintf(l2, sizeof(l2), "dom %s kW", lv);
  plex::str(s, plex::f13(), l2, 141, 46, cMain);
  // Wiersz 64 dzieli 172 px miedzy metryke sieci a "łącznie", wiec szerokosc
  // metryki MIERZYMY i w razie ciasnoty degradujemy dwustopniowo (zawsze do
  // prawdziwej, tylko krotszej formy):
  //   pelna:  "do sieci +15,5 kW" (100 px) / "z sieci 15,5 kW" (82 px)
  //   krok 1: bez dziesiatych      "do sieci +16 kW" (89 px)  — przy >=10 kW
  //           strata 0,5 kW to <5% i mniej niz drganie odczytu chwilowego;
  //   krok 2: bez znaku "+"        "do sieci 16 kW"  (79 px)  — kierunek wciaz
  //           niesie slowo i zielony kolor, znak byl tylko ozdobnikiem mockupu.
  // Budzet = 313 − szer("łącznie …") − 3 px przerwy − 141. Najczestszy uklad
  // (5-cyfrowe "łącznie 99999", 76 px): budzet 93 px — "do sieci +9,9 kW" (93 px)
  // wchodzi co do piksela; 6-cyfrowy licznik (83 px, >100 MWh w ~15. roku pracy)
  // zbija budzet do 86 px i wtedy dziala kaskada. Import ("z sieci …", max 82 px)
  // nie degraduje sie nigdy.
  const int agw = gridW < 0 ? -gridW : gridW;
  const int gAvail = grid::DATA_R - totW - 3 - 141;
  char gv[12];
  char g2[28];
  fmt1(gv, sizeof(gv), agw / 1000.f);
  snprintf(g2, sizeof(g2), gridW >= 0 ? "do sieci +%s kW" : "z sieci %s kW", gv);
  if (plex::width(plex::f13(), g2) > gAvail) {
    snprintf(gv, sizeof(gv), "%.0f", agw / 1000.f);
    snprintf(g2, sizeof(g2), gridW >= 0 ? "do sieci +%s kW" : "z sieci %s kW", gv);
  }
  if (plex::width(plex::f13(), g2) > gAvail && gridW >= 0) {
    snprintf(g2, sizeof(g2), "do sieci %s kW", gv);
  }
  plex::str(s, plex::f13(), g2, 141, 64, pvOld ? col::MUTE : (gridW >= 0 ? col::OK : col::GRID));

  // --- DWA PASKI BILANSU DNIA (serce ukladu FusionSolar) ------------------------
  // PASEK 1 "PV DZIŚ": ile z dzisiejszej produkcji zostalo W DOMU, a ile poszlo
  // do sieci. Suma paska = produkcja dzienna Z REJESTRU 32114 (ta sama liczba, co
  // "dziś" wyzej — jedna prawda na ekranie). Procenty liczone z kWh, nie z mocy
  // chwilowych.
  //
  // (v165) ROLE SIE ODWROCILY. Do v164 mierzone bylo "zuzyte" (calka z profilu
  // doby), a "oddane" wychodzilo z odejmowania. Teraz — gdy baza licznikow jest
  // pelna — mierzone jest ODDANE (licznik 37119 minus stan z polnocy), a zuzycie
  // wlasne jest reszta. Zamiana jest celowa: licznik calkuje sprzetowo i bez
  // przerw, nasza calka probkuje co 10 minut i milczy, kiedy milczy falownik.
  // Klamra zostaje, tylko po drugiej stronie — szczegoly w bloku wyboru zrodla.
  // Zrodlo bilansu (miernik albo calka) rozstrzyga pvBalance() — jedno miejsce
  // dla obu paskow i dla procentu autokonsumpcji w v3PvBottom.
  const PvBalance bal = pvBalance(pv.data, hist);
  const float pvToday = bal.pvToday;
  const float selfPv = bal.selfPv;
  const float expKwh = bal.toGrid;
  char t1v[12], la[12], lb[12], line1[48];
  fmt1(t1v, sizeof(t1v), pvToday);
  snprintf(line1, sizeof(line1), "PV DZIŚ %s kWh", t1v);
  plex::str(s, plex::f11(), line1, grid::MARGIN, 88, cSec);
  fmt1(la, sizeof(la), selfPv);
  fmt1(lb, sizeof(lb), expKwh);
  // Legenda po prawej w tym samym wierszu. Kolejnosc slow = kolejnosc segmentow
  // paska (lewy->prawy).
  // (v167) "zużyte" USTAPILO slowu "z PV" — jedyna zmiana napisu na tym ekranie.
  // Powod: to jest DOKLADNIE ta sama liczba (`bal.selfPv`), co "z PV" w legendzie
  // paska "DOM DZIŚ" 28 px nizej i co "z PV" na ekranie glownym. Od v167 nosi tez
  // ten sam kolor, a dwa rozne slowa pod jednym kolorem podwazalyby caly komunikat:
  // czytelnik uznalby, ze to jednak dwie rozne wielkosci, ktore przypadkiem trafily
  // w ten sam odcien. Jedna wielkosc = jeden kolor I jedno slowo.
  // Szerokosci: najszersze "z PV 99,9 · oddane 99,9" ma w f11 134 px (bylo
  // "zużyte …" = 150 px), wiec start x=313−134=179, a tytul "PV DZIŚ 99,9 kWh"
  // (100 px od x=7) konczy sie na x=107 -> 72 px luzu (bylo 56).
  snprintf(line1, sizeof(line1), "z PV %s · oddane %s", la, lb);
  plex::strRight(s, plex::f11(), line1, grid::DATA_R, 88, cSec);
  // Prog 0,05 kWh (nie ==0): rano/zima licznik dzienny potrafi dlugo stac na
  // pojedynczych watogodzinach — pasek z procentami przy tak malej podstawie bylby
  // szumem. Ponizej progu: pusty tor, bez segmentow i procentow (patrz dayBar).
  // (v167) [SELF autokonsumpcja][OK oddane] — dokladnie lewa i prawa czesc paska
  // ekranu glownego w stanie eksportu, tylko w kWh za dobe zamiast w kW chwilowych.
  // Bylo [OK zuzyte][OK2 oddane], czyli zielen dzielona na dwa odcienie.
  dayBar(s, 92, pvToday > 0.05f ? selfPv / pvToday : 0.f, pvToday > 0.05f, pvOld,
         col::SELF, col::OK, col::BG, col::PANEL);

  // PASEK 2 "DOM DZIŚ": czym dom byl dzis zasilany. Tytul = `bal.home`, ktore
  // pvBalance() liczy jako `selfPv + fromGrid`, czyli jako sume TYCH segmentow —
  // pasek sumuje sie do tytulu z definicji, niezaleznie od wybranego zrodla.
  //
  // (v165) PRZY MIERNIKU "z sieci" JEST POMIAREM: to licznik 37121 minus jego stan
  // z polnocy. To wlasnie ta pozycja byla dotad zanizona — calka z profilu doby
  // pomijala godziny, w ktorych falownik nie odpowiadal (Huawei potrafi wylaczyc
  // Modbus TCP po zachodzie), czyli okolo 1-2 kWh nocnego poboru dziennie. Licznik
  // stoi na zlaczu z siecia i liczy takze wtedy, gdy nikt go nie pyta.
  //
  // BEZ MIERNIKA (fallback) obie czesci ida z TEJ SAMEJ calki profilu doby
  // (z PV = min(prod, pobor), z sieci = max(pobor − prod, 0)) — zachowanie v164
  // razem z jego znana wada opisana wyzej.
  const float homeKwh = bal.home;
  char t2v[12];
  fmt1(t2v, sizeof(t2v), homeKwh);
  snprintf(line1, sizeof(line1), "DOM DZIŚ %s kWh", t2v);
  plex::str(s, plex::f11(), line1, grid::MARGIN, 116, cSec);
  fmt1(la, sizeof(la), bal.selfPv);
  fmt1(lb, sizeof(lb), bal.fromGrid);
  // Najszersze "z PV 99,9 · z sieci 99,9" ma 126 px (start x=187), tytul "DOM DZIŚ
  // 99,9 kWh" konczy sie na x=120 -> 67 px luzu.
  snprintf(line1, sizeof(line1), "z PV %s · z sieci %s", la, lb);
  plex::strRight(s, plex::f11(), line1, grid::DATA_R, 116, cSec);
  // (v167) [SELF autokonsumpcja][GRID z sieci] — dokladnie ten sam pasek co na
  // ekranie glownym w stanie importu. LEWY SEGMENT JEST TERAZ TYM SAMYM KOLOREM,
  // CO LEWY SEGMENT PASKA WYZEJ, bo to ta sama liczba (`bal.selfPv`); do v166 byl
  // pomaranczowy (col::PV) obok zielonego, choc oba pokazywaly np. 20,9 kWh.
  // Prawy segment bez zmian — czerwien "z sieci" byla zgodna z kontraktem od v164.
  // Kolor procentu w lewym segmencie zmieniony z PANEL na BG, bo SELF jest ciemny
  // (4,09 : 1 zamiast 3,80 : 1 — rachunek przy dayBar).
  dayBar(s, 120, homeKwh > 0.05f ? bal.selfPv / homeKwh : 0.f, homeKwh > 0.05f, pvOld,
         col::SELF, col::GRID, col::BG, col::BG);

  // Wykres doby: WYPELNIONY (nie linia — na zywo linia byla nieczytelna), slupki
  // pelnej szerokosci slotu, bez przerw.
  //
  // (v167) TRZY SERIE ZAMIAST DWOCH — dolozona AUTOKONSUMPCJA. Kolejnosc rysowania
  // (od tylu): 1. pobor domu, 2. produkcja, 3. autokonsumpcja = min(produkcja,
  // pobor). Kazda seria idzie OD PODSTAWY w gore, wiec pozniejsza zakrywa dolna
  // czesc wczesniejszej.
  //
  // CO Z TEJ KOLEJNOSCI WYNIKA WIZUALNIE: widac DOKLADNIE TRZY ROZLACZNE PASMA,
  // ktore sa trzema skladnikami bilansu z kontraktu (ThemeV3.h) — dolem zawsze
  // autokonsumpcja, a nad nia albo nadwyzka oddana, albo energia dobrana z sieci,
  // nigdy oba naraz. To ten sam podzial, co pasek na ekranie glownym, tylko
  // rozciagniety w czas. Dlatego SERIE nie dostaja "wlasnych" kolorow: seria poboru
  // jest rysowana czerwienia GRID, bo widac z niej wylacznie czesc PONAD
  // autokonsumpcja, a to jest wlasnie import; seria produkcji zielenia OK, bo widac
  // z niej wylacznie nadwyzke, czyli eksport. Bursztyn col::PV znika z tego wykresu:
  // "produkcja ogolem" nie jest skladnikiem, tylko suma dwoch pasm.
  //
  // RACHUNEK (peak = 4000 W, ch−2 = 62 px, wysokosc = int(62 * moc / 4000)):
  //  A) PRODUKCJA > POBOR (poludnie, prod 3000, pobor 1000):
  //     lh = int(62*0,25) = 15, ph = int(62*0,75) = 46, sh = int(62*0,25) = 15.
  //     Rysujemy GRID 0..15 -> OK 0..46 (zakrywa czerwony w calosci) -> SELF 0..15.
  //     WIDAC: 0..15 niebieski = autokonsumpcja 1000 W, 15..46 zielony = 2000 W
  //     oddane. Czerwonego nie widac ani piksela — i slusznie, import = 0.
  //  B) POBOR > PRODUKCJA (wieczor, prod 1000, pobor 3000):
  //     lh = 46, ph = 15, sh = 15. GRID 0..46 -> OK 0..15 -> SELF 0..15.
  //     WIDAC: 0..15 niebieski = 1000 W z PV, 15..46 czerwony = 2000 W z sieci.
  //     Zielonego nie widac — eksport = 0.
  //  C) prod = 0 (noc): ph = sh = 0, caly slupek czerwony = pobor z sieci.
  //  D) pobor = 0: lh = sh = 0, caly slupek zielony = wszystko poszlo do sieci.
  //  E) prod = pobor: lh = ph = sh, caly slupek niebieski = nic nie plynie przez
  //     zlacze z siecia.
  // Trzeci fillRect NIE jest redundantny: bez niego dolne pasmo mialoby kolor serii
  // rysowanej jako druga (zielony), czyli twierdziloby, ze energia zjedzona przez
  // dom zostala oddana do sieci.
  //
  // BEZ NOWEJ TABLICY W RAM: min() liczone w locie z dwoch pol, ktore i tak juz
  // czytamy w tej samej iteracji. Wysokosc pasma autokonsumpcji jest liczona TYM
  // SAMYM wyrazeniem, co lh i ph, tylko z mniejsza z dwoch mocy — a poniewaz to
  // doslownie jedno z tamtych dwoch wyrazen, wychodzi z niego dokladnie
  // min(lh, ph) co do piksela, bez ryzyka, ze niebieskie pasmo wystaje ponad
  // slupek, ktory ma przykrywac (zaokraglenia nie moga sie rozjechac).
  // (v164) Nizszy niz do v163 (bylo cy=90, ch=84): gorna polowa ekranu oddana
  // paskom bilansu, wykres schodzi na y=140..204 — os godzin w dolnym pasie
  // (v3PvBottom, y=216) zostaje na miejscu i teraz siedzi tuz pod podstawa.
  const int cx = grid::MARGIN, cy = 140, cw = grid::W - 2 * grid::MARGIN, ch = 64;
  const int base = cy + ch;
  s.drawFastHLine(cx, base, cw, col::LINE);
  const uint16_t peak = hist.peak();

  // Aktualny slot (do zaznaczenia "reszta doby przed nami").
  int curSlot = -1;
  {
    const time_t now = time(nullptr);
    if (now > 1700000000) {
      struct tm tmv{};
      localtime_r(&now, &tmv);
      curSlot = (tmv.tm_hour * 60 + tmv.tm_min) / 10;
    }
  }
  if (peak > 0) {
    for (int i = 0; i < PvHistory::SLOTS; ++i) {
      const int x0 = cx + (i * cw) / PvHistory::SLOTS;
      const int x1 = cx + ((i + 1) * cw) / PvHistory::SLOTS;
      const int bw = (x1 - x0) > 1 ? (x1 - x0) : 1;   // pelna szerokosc slotu, bez luk
      if (hist.filled[i]) {
        const uint16_t wS = hist.watts[i], lS = hist.load[i];
        const uint16_t sS = wS < lS ? wS : lS;   // autokonsumpcja w locie, bez tablicy
        // 1. POBOR DOMU z tylu — widoczna zostanie tylko czesc ponad autokonsumpcja,
        //    czyli energia dobrana z sieci, i stad czerwien.
        const int lh = static_cast<int>((ch - 2) * (lS / static_cast<float>(peak)));
        if (lh > 0) s.fillRect(x0, base - lh, bw, lh, col::GRID);
        // 2. PRODUKCJA — widoczna zostanie tylko nadwyzka ponad autokonsumpcja,
        //    czyli energia oddana do sieci, i stad zielen.
        const int ph = static_cast<int>((ch - 2) * (wS / static_cast<float>(peak)));
        if (ph > 0) s.fillRect(x0, base - ph, bw, ph, col::OK);
        // 3. AUTOKONSUMPCJA na wierzchu — dolne pasmo kazdego slupka.
        const int sh = static_cast<int>((ch - 2) * (sS / static_cast<float>(peak)));
        if (sh > 0) s.fillRect(x0, base - sh, bw, sh, col::SELF);
      } else if (i > curSlot && curSlot >= 0) {
        s.fillRect(x0, base - 2, bw, 2, col::LINE);   // reszta doby - jeszcze przed nami
      }
    }
  } else {
    plex::strCenter(s, plex::f13(), "brak profilu doby", grid::W / 2, base - 30, col::MUTE);
  }
}

void v3PvBottom(TFT_eSPI& tft, const PvModel& pv, const PvHistory& hist) {
  tft.fillRect(0, 206, grid::W, 34, col::BG);
  // Osie godzin wykresu (wyrownane z v3Pv).
  const int cx = grid::MARGIN, cw = grid::W - 2 * grid::MARGIN;
  for (int hh = 0; hh <= 24; hh += 6) {
    char hb[4];
    snprintf(hb, sizeof(hb), "%d", hh);
    const int x = cx + (hh * cw) / 24;
    plex::strCenter(tft, plex::f10(), hb, x, 216, col::MUTE);
  }
  // (v167) LEGENDA NAZYWA WIDOCZNE PASMA, NIE SERIE — i to jest tu cala decyzja.
  // Wykres rysuje TRZY SERIE (pobor domu, produkcja, autokonsumpcja), ale czytelnik
  // nie widzi ANI JEDNEJ z nich w calosci: dwie pierwsze sa przykryte i wystaja
  // z nich wylacznie roznice. Podpis "pobór domu" przy czerwonej probce wskazywalby
  // wiec pole, ktorego na wykresie NIE MA — czerwone pasmo nigdy nie jest calym
  // poborem domu, tylko jego czescia dobrana z sieci; tak samo zielone nie jest
  // produkcja, tylko jej nadwyzka. Nazywamy dokladnie to, co widac, i DOKLADNIE
  // TYMI SAMYMI slowami, co legendy obu paskow wyzej: "z PV", "oddane", "z sieci".
  // (Do v166 legenda nazywala serie i wtedy to bylo poprawne, bo serii byly dwie
  // i kazda miala wlasne, w pelni widoczne pole.)
  //
  // SZEROKOSCI (f13, probka 10 px + 4 px odstepu do tekstu, pas x=7..313 = 306 px):
  //   probka + "z PV"    (25) = 39 px -> x  7..46
  //   probka + "oddane"  (42) = 56 px -> x 58..114
  //   probka + "z sieci" (33) = 47 px -> x 126..173
  //   "autokonsumpcja 100%" = 128 px, do prawej krawedzi -> start x=185
  // Odstepy miedzy pozycjami: 12, 12 i 12 px, przy 4 px miedzy probka a jej wlasnym
  // podpisem — trzykrotna roznica wystarcza, zeby sasiednie pozycje sie nie zlewaly.
  // Procent liczony w wariancie NAJSZERSZYM (100%); typowe "62%" ma 121 px, czyli
  // 7 px luzu wiecej. Suma pozycji 39+56+47+128 = 270 px, odstepy 36 px = 306 px.
  tft.fillRect(grid::MARGIN, 226, 10, 8, col::SELF);
  plex::str(tft, plex::f13(), "z PV", grid::MARGIN + 14, 233, col::SECOND);
  tft.fillRect(58, 226, 10, 8, col::OK);   // próbka wypełniona, jak pasma wykresu
  plex::str(tft, plex::f13(), "oddane", 72, 233, col::SECOND);
  tft.fillRect(126, 226, 10, 8, col::GRID);
  plex::str(tft, plex::f13(), "z sieci", 140, 233, col::SECOND);
  // Autokonsumpcja = procent DZISIEJSZEJ produkcji zuzyty na miejscu — dokladnie
  // udzial lewego segmentu paska "PV DZIŚ". (v165) Te same liczby bierzemy z tego
  // samego pvBalance(), co paski, wiec podpis nie moze sie z nimi rozjechac przy
  // przelaczeniu zrodla miernik/calka. Najszersze "autokonsumpcja 100%" ma
  // 128 px (start x=185), a "z sieci" konczy sie na x=173 -> 12 px luzu.
  // Trzy stany jak w v3Pv: bez odczytu / sen / produkcja ~0 -> bez napisu (procent
  // z pustej podstawy to szum); dane stare -> liczba zostaje, kolor na MUTE.
  // (v167) TEN PROCENT ZOSTAJE, choc trzecia probka legendy zabrala mu 17 px luzu.
  // Kusilo, zeby go skasowac jako duplikat — ta sama liczba stoi w lewym segmencie
  // paska "PV DZIŚ" cztery wiersze wyzej. Ale tamten procent jest rysowany TYLKO
  // wtedy, gdy segment jest dosc szeroki (szerokosc napisu + 8 px, patrz dayBar)
  // i znika CALKOWICIE przy starych danych (`muted` konczy dayBar przed procentami).
  // Dokladnie w tych dwoch przypadkach — mala autokonsumpcja i dane po progu
  // swiezosci — ten napis jest jej JEDYNYM miejscem na ekranie.
  const bool pvFresh = pv.online && freshMs(diag().pvOkAt, pv.asleep ? cfg::PV_STALE_NIGHT_MS
                                                                     : cfg::PV_STALE_MS);
  const bool pvOld = pv.data.valid && !pvFresh && !pv.asleep;
  const PvBalance bal = pvBalance(pv.data, hist);
  const float pvToday = bal.pvToday;
  if (pv.data.valid && !pv.asleep && pvToday > 0.05f) {
    char ak[32];
    snprintf(ak, sizeof(ak), "autokonsumpcja %d%%",
             static_cast<int>(bal.selfPv / pvToday * 100.f + 0.5f));
    plex::strRight(tft, plex::f13(), ak, grid::W - grid::MARGIN, 233,
                   pvOld ? col::MUTE : col::SECOND);
  }
}

// ============================================================ AUTO =============
// (v174) Stan Tesli, wariant "pasmowy" przyjety przez wlasciciela. Uklad od gory:
// naglowek -> wielkie naladowanie + zasieg -> pasek baterii -> cztery kolumny
// (moc/prad/dodane/stan) -> pasmo "skad prad dzisiaj" -> [dolny pas: kafelek trybu].
//
// EKRAN STOI ZARAZ ZA PRADEM w petli rotacji (kV3Loop w WeatherUi.cpp) i CELOWO
// powtarza jego jezyk: to samo dwukolorowe pasmo, ten sam podzial "z PV / z sieci".
//
// DANE PRZYCHODZA, A NIE SA POBIERANE. Wszystko, co tu widac, przyszlo po MQTT
// z Home Assistanta (MqttClient.cpp, temat <prefix>/auto/stan). Ta funkcja NIE ma
// prawa niczego dopytywac ani liczyc wieku z innego zrodla niz a.atMs.

// Kolor kafelka trybu. TO JEST KONTRAKT Z PIERSCIENIEM WLED W GARAZU, a nie dobor
// estetyczny: ten sam tryb ma tam swiecic tym samym kolorem, wiec zmiana tutaj bez
// zmiany tam sprawia, ze ekran i lampa mowia co innego o tym samym stanie.
//   PV     zielony  — ladujemy wylacznie nadwyzka, czyli darmowa energia,
//   PV+MIN niebieski — nadwyzka plus minimalny dobor z sieci (kolor "opadu", jedyny
//                      niebieski w palecie poza col::SELF, ktory jest zarezerwowany
//                      dla energii z PV zuzytej na miejscu — patrz kontrakt v167),
//   MAX    czerwony  — pelna moc z sieci, czyli placimy za wszystko: col::GRID, ten
//                      sam czerwony, ktorym na PRADZIE malowana jest energia dobrana
//                      z sieci. To NIE jest przypadek — obie rzeczy znacza to samo,
//   OFF    bursztyn  — ladowanie wylaczone. col::WARN, a NIE col::PV/col::SUN: na
//                      kafelku stoi BIALY tekst, a bialy na #E0A92E ma kontrast 2,1,
//                      czyli z dwoch metrow jest nieczytelny; na ciemniejszym
//                      #B8901F wychodzi 3,0. Sens sie zgadza takze semantycznie —
//                      col::WARN jest w tej palecie kolorem "uwaga / nieaktywne".
// NOWYCH KOLOROW NIE DOKLADAMY DO PALETY: wszystkie cztery juz w niej stoja.
uint16_t autoModeCol(const AutoModel& a) {
  if (a.modeIs("PV")) return col::OK;
  if (a.modeIs("PV+MIN")) return col::RAIN;
  if (a.modeIs("MAX")) return col::GRID;
  if (a.modeIs("OFF")) return col::WARN;
  return col::MUTE;   // tryb spoza kontraktu — szary, zeby nie udawac znaczenia
}

// Kolor WARTOSCI w kolumnie STAN. Zielony tylko wtedy, gdy energia naprawde plynie;
// "czeka"/"postoj" to stany czynne, ale bezczynne energetycznie (SECOND), a "spi"
// i brak kabla to stany, w ktorych auto nic od nas nie chce (MUTE).
uint16_t autoStateCol(const AutoModel& a) {
  if (a.stateIs("laduje")) return col::OK;
  if (a.stateIs("czeka") || a.stateIs("stoi")) return col::SECOND;
  return col::MUTE;
}

// Ile procent dzisiejszego ladowania przyszlo ze slonca. Zwraca -1, gdy dzis nie
// bylo czego dzielic (obie liczby ~zero) — procent z pustej podstawy to szum, ta
// sama zasada, co przy autokonsumpcji w v3PvBottom.
int autoSunPct(const AutoModel& a) {
  const float tot = a.sunKwh + a.gridKwh;
  if (tot < 0.05f) return -1;
  return static_cast<int>(a.sunKwh / tot * 100.f + 0.5f);
}

void v3Auto(TFT_eSPI& s, const AutoModel* ap) {
  s.fillRect(0, 0, grid::W, 206, col::BG);
  static const AutoModel kEmpty{};
  const AutoModel& a = ap ? *ap : kEmpty;

  // Rotacja tego ekranu tu NIE DOJEDZIE bez danych (viewSkipped), ale przypiac go
  // przez POST /api/view?i=12 mozna ZAWSZE — i wtedy musi byc co pokazac.
  if (a.atMs == 0) {
    lightHeader(s, "AUTO", nullptr, Fresh::UNKNOWN);
    plex::strCenter(s, plex::f20(), "Brak danych o aucie", grid::W / 2, 110, col::MUTE);
    plex::strCenter(s, plex::f13(), "nie przyszła żadna wiadomość MQTT",
                    grid::W / 2, 134, col::MUTE);
    return;
  }

  // Wiek odczytu w naglowku — ta sama para (agoWords + freshDot), co na PRADZIE
  // i OGRZEWANIU. Prog cfg::AUTO_STALE_MS jest TYM SAMYM progiem, ktory wyrzuca ten
  // ekran z rotacji, wiec kropka STALE moze sie tu pojawic tylko na ekranie
  // PRZYPIETYM z panelu — i wtedy jest jedyna informacja, ze dane stanely.
  const bool fresh = freshMs(a.atMs, cfg::AUTO_STALE_MS);
  char ago[24];
  agoWords(ago, sizeof(ago), okAgeS(a.atMs));
  lightHeader(s, "AUTO", ago, fresh ? Fresh::OK : Fresh::STALE);

  // Stare dane wyciszamy tak samo jak moc chwilowa na PRADZIE (v161): liczby zostaja
  // (to wciaz jedyne, co o aucie wiemy), znika kolor, czyli sugestia "dzieje sie teraz".
  const uint16_t cMain = fresh ? col::PANEL : col::MUTE;
  const uint16_t cSec = fresh ? col::SECOND : col::MUTE;

  // --- WIELKIE NALADOWANIE + ZASIEG (y=33..72) ---------------------------------
  // Szerokosci policzone, nie przymierzone: "100" w f52 ma 99 px (x=7..106) i razem
  // ze znakiem "%" (f20, 18 px) konczy sie na x=129. Po prawej zasieg czterocyfrowy
  // "9999 km" (f20, 89 px) zaczyna sie na x=224 — zostaje 95 px luzu, wiec kolizji
  // nie ma nawet w wariancie skrajnym.
  char big[8];
  snprintf(big, sizeof(big), "%u", static_cast<unsigned>(a.soc));
  const int bw = plex::str(s, plex::f52(), big, grid::MARGIN, 72, cMain);
  plex::str(s, plex::f20(), "%", grid::MARGIN + bw + 5, 72, cSec);

  char km[16];
  snprintf(km, sizeof(km), "%d km", static_cast<int>(a.rangeKm));
  plex::strRight(s, plex::f20(), km, grid::DATA_R, 52, cMain);
  char lim[16];
  snprintf(lim, sizeof(lim), "limit %u%%", static_cast<unsigned>(a.limitPct));
  plex::strRight(s, plex::f13(), lim, grid::DATA_R, 71, col::MUTE);

  // --- PASEK BATERII (x=7..312, y=82..90) --------------------------------------
  // NIE uzywamy tv3::bar(): tamten helper rysuje prostokaty o ostrych rogach (paski
  // bilansu na PRADZIE), a bateria ma byc zaokraglona — to jedyny element tego
  // ekranu, ktory przedstawia FIZYCZNY przedmiot, i zaokraglenie odroznia go od
  // pasm energii nizej. Promien 4 przy wysokosci 9 daje pelne polkola na koncach.
  constexpr int kBarX = grid::MARGIN, kBarY = 82, kBarH = 9;
  constexpr int kBarW = grid::DATA_R - grid::MARGIN;   // 306
  s.fillRoundRect(kBarX, kBarY, kBarW, kBarH, 4, col::LINE);
  const int fillW = (kBarW * a.soc) / 100;
  // Ponizej ~9 px zaokraglony prostokat degeneruje sie do kropki, wiec bardzo niskie
  // naladowanie rysujemy prostokatem — inaczej "3%" wygladaloby jak zero.
  if (fillW >= kBarH) s.fillRoundRect(kBarX, kBarY, fillW, kBarH, 4, fresh ? col::OK : col::MUTE);
  else if (fillW > 0) s.fillRect(kBarX, kBarY, fillW, kBarH, fresh ? col::OK : col::MUTE);

  // Kreska limitu: gdzie ladowanie ma sie zatrzymac. Wystaje 2 px nad i pod pasek,
  // zeby byla widoczna takze wtedy, gdy stoi na wypelnieniu (kolor PANEL na zielonym).
  // Przyciecie do prawej krawedzi paska: przy limicie 100% wyliczony x wypadlby
  // DOKLADNIE na 313, czyli o piksel za paskiem, i kreska wisialaby w powietrzu.
  int limX = kBarX + (kBarW * a.limitPct) / 100;
  if (limX > kBarX + kBarW - 2) limX = kBarX + kBarW - 2;
  if (limX < kBarX) limX = kBarX;
  s.fillRect(limX, kBarY - 2, 2, kBarH + 4, col::PANEL);

  s.drawFastHLine(grid::MARGIN, 100, grid::W - 2 * grid::MARGIN, col::LINE);

  // --- CZTERY KOLUMNY (etykieta y=118, wartosc y=140) --------------------------
  // Podzial 306 px na cztery rowne slupki po ~76 px: x = 7 / 83 / 159 / 235.
  // NAJCIASNIEJSZE MIEJSCE CALEGO EKRANU, policzone dla wartosci skrajnych:
  //   MOC    "11,0 kW"    45 px -> 7..52     (kolumna do 83  => 31 px luzu)
  //   PRĄD   "255 A"      33 px -> 83..116   (kolumna do 159 => 43 px luzu)
  //   DODANE "99,9 kWh"   52 px -> 159..211  (kolumna do 235 => 24 px luzu)  <- MIN
  //   STAN   "brak kabla" 58 px -> 235..293  (krawedz 313    => 20 px luzu)
  // Etykiety sa wezsze od wartosci w kazdej kolumnie, wiec nie one wyznaczaja limit.
  auto colCell = [&](int x, const char* label, const char* value, uint16_t vc) {
    // Etykieta ZAWSZE col::MUTE — takze przy starych danych. Wyciszanie dotyczy
    // WARTOSCI (one klamia, gdy sa stare), a nie nazwy wielkosci: "MOC" znaczy
    // "moc" niezaleznie od tego, kiedy przyszla ostatnia wiadomosc.
    plex::str(s, plex::f11(), label, x, 118, col::MUTE);
    plex::str(s, plex::f13(), value, x, 140, vc);
  };
  char mv[16];
  fmt1(mv, sizeof(mv), a.kw);
  char m2[20];
  snprintf(m2, sizeof(m2), "%s kW", mv);
  colCell(7, "MOC", m2, cMain);

  char av[12];
  snprintf(av, sizeof(av), "%u A", static_cast<unsigned>(a.amps));
  colCell(83, "PRĄD", av, cMain);

  char dv[16];
  fmt1(dv, sizeof(dv), a.addedKwh);
  char d2[20];
  snprintf(d2, sizeof(d2), "%s kWh", dv);
  colCell(159, "DODANE", d2, cMain);

  colCell(235, "STAN", autoStateLabel(a), fresh ? autoStateCol(a) : col::MUTE);

  s.drawFastHLine(grid::MARGIN, 152, grid::W - 2 * grid::MARGIN, col::LINE);

  // --- SKAD PRAD DZISIAJ (pasmo dwuczesciowe, y=174..185) ----------------------
  // KOLORY SA KONTRAKTEM Z v167, NIE WYBOREM TEGO EKRANU (patrz komentarz przy
  // tv3::col::OK). Energia oddana do auta ZE SLONCA to energia z PV ZUZYTA NA
  // MIEJSCU — czyli dokladnie ta sama wielkosc, ktora na ekranie GLOWNYM i na
  // PRADZIE nazywa sie "z PV" i ma kolor col::SELF (niebieski). Zielony col::OK
  // znaczy w tym projekcie "ODDANE DO SIECI", a energia, ktora poszla do samochodu,
  // do sieci wlasnie NIE poszla — pomalowanie jej na zielono twierdziloby, ze ta
  // sama kilowatogodzina jednoczesnie wyjechala z domu i w nim zostala.
  // Druga czesc to col::GRID, ten sam czerwony, co "z sieci" na PRADZIE.
  // Zadnego nowego zielonego (ani zadnego innego koloru) tu nie wprowadzamy.
  plex::str(s, plex::f11(), "SKĄD PRĄD DZISIAJ", grid::MARGIN, 168, cSec);
  constexpr int kBandX = grid::MARGIN, kBandY = 174, kBandH = 12;
  constexpr int kBandW = grid::DATA_R - grid::MARGIN;   // 306
  const float sun = a.sunKwh > 0.f ? a.sunKwh : 0.f;
  const float net = a.gridKwh > 0.f ? a.gridKwh : 0.f;
  const float tot = sun + net;
  if (tot < 0.05f) {
    // Dzis jeszcze nic nie wjechalo w auto. Pusta sciezka zamiast dzielenia przez
    // zero — i zaden podpis z liczbami, bo obie wynosza zero.
    s.fillRect(kBandX, kBandY, kBandW, kBandH, col::LINE);
    plex::str(s, plex::f13(), "dziś nic nie ładowano", grid::MARGIN, 200, col::MUTE);
  } else {
    int sunW = static_cast<int>(kBandW * (sun / tot) + 0.5f);
    if (sunW < 0) sunW = 0;
    if (sunW > kBandW) sunW = kBandW;
    s.fillRect(kBandX, kBandY, kBandW, kBandH, col::GRID);
    if (sunW > 0) s.fillRect(kBandX, kBandY, sunW, kBandH, col::SELF);
    // Podpisy: "słońce 99,9 kWh" (91 px, x=7..98) i "sieć 99,9 kWh" (77 px, do
    // prawej krawedzi, x=236..313) — 138 px luzu miedzy nimi w wariancie skrajnym.
    char sv[12], nv[12], line[32];
    fmt1(sv, sizeof(sv), sun);
    fmt1(nv, sizeof(nv), net);
    snprintf(line, sizeof(line), "słońce %s kWh", sv);
    plex::str(s, plex::f13(), line, grid::MARGIN, 200, cSec);
    snprintf(line, sizeof(line), "sieć %s kWh", nv);
    plex::strRight(s, plex::f13(), line, grid::DATA_R, 200, cSec);
  }
}

// Dolny pas AUTO: kafelek trybu po lewej + dwie linijki pomocnicze po prawej.
// Konwencja jak w v3PvBottom/v3HomeBottom — wlasne tlo, cienka linia u gory pasa
// (to jest ta sama linia, ktora na innych ekranach oddziela tresc od stopki),
// rysowane WPROST na TFT, bo pas 206..239 lezy poza sprite'em.
void v3AutoBottom(TFT_eSPI& tft, const AutoModel* ap) {
  tft.fillRect(0, 206, grid::W, 34, col::BG);
  tft.drawFastHLine(grid::MARGIN, 210, grid::W - 2 * grid::MARGIN, col::LINE);
  static const AutoModel kEmpty{};
  const AutoModel& a = ap ? *ap : kEmpty;
  if (a.atMs == 0) {
    plex::str(tft, plex::f13(), "czekam na dane z auta", grid::MARGIN, 228, col::MUTE);
    return;
  }
  const bool fresh = freshMs(a.atMs, cfg::AUTO_STALE_MS);

  // KAFELEK TRYBU (92x26, x=7..98, y=212..237). Najszerszy napis "PV+MIN" ma w f13
  // 46 px i wysrodkowany na x=53 zajmuje 30..76, czyli po 11 px marginesu w kafelku.
  // Bialy tekst na wypelnieniu: to jedyny element ekranu z odwroconym kontrastem
  // i tak ma byc — tryb ma byc widoczny z drugiego konca pokoju bez czytania.
  // Przy STARYCH danych kafelek schodzi na col::MUTE: kolor trybu jest obietnica
  // "tak wlasnie teraz laduje", a po 45 s ciszy nie mamy jej czym pokryc.
  //
  // NAPIS TRYBU PRZYCHODZI Z SIECI, wiec nie mamy nad nim wladzy — a tu jest jedyne
  // miejsce na ekranie, w ktorym cudzy tekst ma zostac WEWNATRZ figury. Straznikiem
  // jest rozmiar pola: AutoModel::mode ma 8 B, czyli najwyzej 7 znakow, a siedem
  // NAJSZERSZYCH glifow f13 (~12 px) to 84 px — wysrodkowane na x=53 zajmuja 11..95
  // i nadal miesza sie w kafelku 7..99. Powiekszenie tamtego pola bez powiekszenia
  // kafelka wypuscilo by napis poza zaokraglona krawedz.
  constexpr int kTileX = grid::MARGIN, kTileY = 212, kTileW = 92, kTileH = 26;
  tft.fillRoundRect(kTileX, kTileY, kTileW, kTileH, 6,
                    fresh ? autoModeCol(a) : col::MUTE);
  const int tcx = kTileX + kTileW / 2;
  plex::strCenter(tft, plex::f10(), "TRYB", tcx, 223, col::BG);
  plex::strCenter(tft, plex::f13(), a.mode[0] ? a.mode : "?", tcx, 236, col::BG);

  // DWIE LINIJKI POMOCNICZE (do prawej krawedzi). Zadna z nich NIE POWTARZA liczby
  // z gory ekranu — powtorzenie kosztowaloby miejsce i nie dodawaloby nic:
  //   1) stan kabla: jedyne pole z wiadomosci, ktorego nigdzie indziej nie widac
  //      (kolumna STAN mowi, co auto ROBI, nie czy jest w ogole podpiete),
  //   2) udzial slonca w dzisiejszym ladowaniu: pasmo wyzej pokazuje to ksztaltem,
  //      ale z dwoch metrow nie da sie odczytac proporcji 60/40 od 70/30. Ta sama
  //      decyzja i to samo uzasadnienie, co przy "autokonsumpcja %" w v3PvBottom.
  // Szerokosci: "kabel odłączony" 88 px (x=225..313), "100% dzisiaj ze słońca"
  // 124 px (x=189..313) — kafelek konczy sie na x=98, wiec 91 px luzu.
  plex::strRight(tft, plex::f13(), a.cable ? "kabel podpięty" : "kabel odłączony",
                 grid::DATA_R, 223, a.cable ? col::PANEL : col::MUTE);
  const int pct = autoSunPct(a);
  if (pct >= 0) {
    char l2[32];
    snprintf(l2, sizeof(l2), "%d%% dzisiaj ze słońca", pct);
    plex::strRight(tft, plex::f13(), l2, grid::DATA_R, 236, col::MUTE);
  }
}

// ============================================================ ZWROT Z PV =======
// (v181) Jedyny ekran w tym projekcie, ktory patrzy WSTECZ O LATA, a nie na ostatnie
// minuty: ile z kosztu instalacji fotowoltaicznej (cfg::PV_KOSZT_PLN) juz wrocilo
// i kiedy wroci reszta. Zrodla sa DWA i celowo rozne co do natury:
//   * kPaybackHist (PaybackHist.h) — 37 punktow miesiecznych we FLASHU, prawda
//     historyczna, ktora nie zalezy od tego, czy MQTT dzis dziala,
//   * CostModel::pvPln — biezaca skumulowana korzysc z <prefix>/dom/stan.
// Dlatego ten ekran, w odroznieniu od AUTO, MA CO POKAZAC ZAWSZE: cisza brokera
// zabiera mu wylacznie biezaca kwote, nie caly wykres (patrz viewSkipped).

// Indeks punktu historii -> rok i miesiac (1..12). Punkt 0 to kPaybackHistMonth0
// roku kPaybackHistYear0; indeksy WIEKSZE niz kPaybackHistN-1 sa legalne i opisuja
// miesiace PROGNOZY — te sama arytmetyka liczy date pelnego zwrotu i podpisy osi X.
void paybackYm(int idx, int& year, int& month) {
  const int m0 = kPaybackHistMonth0 - 1 + idx;   // miesiace liczone od stycznia year0
  year = kPaybackHistYear0 + m0 / 12;
  month = m0 % 12 + 1;
}

// Tempo przyrostu korzysci [zl/mies.] z OKNA 12 OSTATNICH PUNKTOW historii.
// Dlaczego okno, a nie srednia z calosci: pierwszy rok instalacji jest nietypowy
// (rozruch w sierpniu, czyli pol sezonu), a ceny energii przez te trzy lata zmienily
// sie wielokrotnie — srednia z 2023 r. prognozowalaby dzisiejszym tempem sprzed
// dwoch taryf. Rowne 12 miesiecy to takze JEDYNE okno, ktore zawiera pelny cykl
// roczny: krotsze braloby albo same zimy (tempo bliskie zeru), albo same lata.
// Historia krotsza niz 13 punktow (dzis nierealna, ale straznik w PaybackHist.h
// dopuszcza 2) siega po najstarszy punkt, jaki jest.
int paybackRate() {
  const int back = kPaybackHistN > 12 ? 12 : kPaybackHistN - 1;
  const int32_t d = static_cast<int32_t>(kPaybackHist[kPaybackHistN - 1]) -
                    static_cast<int32_t>(kPaybackHist[kPaybackHistN - 1 - back]);
  return static_cast<int>(d / back);
}

// Sufit prognozy: 40 lat. Nie jest ozdoba — przy tempie 1 zl/mies. (mozliwym po
// serii zimowych miesiecy w krotkiej historii) brakujace 20 tys. zl daloby 20 tysiecy
// punktow na osi X szerokiej na 306 px, czyli wykres bez zadnej tresci i podpisy lat
// zlepione w szara kreske. Prognoza dluzsza niz zycie paneli nie jest prognoza.
constexpr int kPaybackMaxFcMonths = 480;

// Ile miesiecy do osiagniecia cfg::PV_KOSZT_PLN przy tym tempie.
// Zwraca 0 = juz splacone, -1 = prognozy NIE MA (tempo <= 0 albo poza sufitem).
int paybackMonthsLeft(int32_t from, int rate) {
  if (rate <= 0) return -1;
  const int32_t miss = cfg::PV_KOSZT_PLN - from;
  if (miss <= 0) return 0;
  const int32_t n = (miss + rate - 1) / rate;   // w GORE: miesiac niepelny tez trwa
  return n > kPaybackMaxFcMonths ? -1 : static_cast<int>(n);
}

// Polska odmiana "rok/lata/lat" wg liczby — ta sama funkcja co czujnikNoun nizej,
// dla innego rzeczownika. Literaly z flasha, bez bufora w RAM.
const char* rokNoun(int n) {
  const int t = n % 100, u = n % 10;
  if (n == 1) return "rok";
  if (u >= 2 && u <= 4 && (t < 12 || t > 14)) return "lata";
  return "lat";
}

void v3Payback(TFT_eSPI& s, const CostModel* cp) {
  s.fillRect(0, 0, grid::W, 206, col::BG);

  // --- SKAD BIERZE SIE GLOWNA LICZBA -------------------------------------------
  // (b)/(c) MQTT cokolwiek przyslal -> liczy sie pvPln, bo jest nowsza niz ostatni
  //         punkt historii (ten konczy sie na ostatnim ZAMKNIETYM miesiacu);
  // (a)     nie przyszlo nic       -> ostatni punkt historii. Ekran zostaje pelny,
  //         tylko przestaje twierdzic, ze pokazuje stan na TERAZ (patrz nizej).
  const bool haveMqtt = (cp != nullptr && cp->atMs != 0);
  const bool fresh = haveMqtt && freshMs(cp->atMs, cfg::PAYBACK_STALE_MS);
  // STAN (c) z v158: dane sa, ale przeterminowane -> liczby zostaja, znika kolor.
  // Brak MQTT (a) NIE jest przeterminowaniem: historia we flashu nie ma wieku, wiec
  // wyszarzenie jej byloby klamstwem w druga strone.
  const bool dim = haveMqtt && !fresh;
  const int32_t histLast = static_cast<int32_t>(kPaybackHist[kPaybackHistN - 1]);
  int32_t gain = haveMqtt ? cp->pvPln : histLast;
  if (gain < 0) gain = 0;

  if (haveMqtt) {
    char ago[24];
    agoWords(ago, sizeof(ago), okAgeS(cp->atMs));
    lightHeader(s, "FOTOWOLTAIKA", ago, fresh ? Fresh::OK : Fresh::STALE);
  } else {
    lightHeader(s, "FOTOWOLTAIKA", "z historii", Fresh::UNKNOWN);
  }

  const uint16_t cMain = dim ? col::MUTE : col::PANEL;
  const uint16_t cSec = dim ? col::MUTE : col::SECOND;

  // --- WIELKI PROCENT ZWROTU (y=33..72) ----------------------------------------
  // Geometria przepisana z ekranu AUTO, bo jest ta sama: "100" w f52 ma 99 px
  // (x=7..106), znak "%" (f20, 18 px) konczy sie na x=129. Po prawej stronie
  // najszerszy napis tego pasa to "brak danych do prognozy" (f13, 132 px,
  // x=181..313) — 52 px luzu w wariancie skrajnym.
  int pct = static_cast<int>((gain * 100) / cfg::PV_KOSZT_PLN);
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;   // po splacie z nadwyzka pasek stoi na 100%, nie na 137%
  char big[8];
  snprintf(big, sizeof(big), "%d", pct);
  const int bw = plex::str(s, plex::f52(), big, grid::MARGIN, 72, cMain);
  plex::str(s, plex::f20(), "%", grid::MARGIN + bw + 5, 72, cSec);

  // --- DATA PELNEGO ZWROTU (prawy gorny rog) -----------------------------------
  // PROGNOZE LICZYMY OD OSTATNIEGO PUNKTU HISTORII, nie od biezacego pvPln — i to
  // jest decyzja, nie przeoczenie. Tempo pochodzi z okna miesiecy ZAMKNIETYCH, wiec
  // doklejenie do niego kwoty z polowy biezacego miesiaca mieszaloby dwie rozne
  // jednostki czasu i przesuwaloby date o kilka tygodni w tam i z powrotem w ciagu
  // kazdego miesiaca. Krzywa prognozy nizej wychodzi z TEGO SAMEGO punktu.
  const int rate = paybackRate();
  const int left = paybackMonthsLeft(histLast, rate);
  plex::strRight(s, plex::f11(), "PEŁNY ZWROT", grid::DATA_R, 52, col::MUTE);
  if (left > 0) {
    int fy = 0, fm = 0;
    paybackYm(kPaybackHistN - 1 + left, fy, fm);
    char dbuf[16];
    snprintf(dbuf, sizeof(dbuf), "%d.%d", fm, fy);
    plex::strRight(s, plex::f20(), dbuf, grid::DATA_R, 72, cMain);
  } else if (left == 0) {
    plex::strRight(s, plex::f20(), "osiągnięty", grid::DATA_R, 72, cMain);
  } else {
    // Tempo <= 0 (historia stoi w miejscu) albo prognoza dluzsza niz sufit. Zadnej
    // daty NIE ZMYSLAMY i zadnej linii przerywanej nizej nie rysujemy.
    plex::strRight(s, plex::f13(), "brak danych do prognozy", grid::DATA_R, 72, col::MUTE);
  }

  // --- PASEK POSTEPU (x=7..312, y=82..90) --------------------------------------
  // tv3::bar, czyli prostokat o OSTRYCH rogach — konwencja paskow proporcji tego
  // motywu (bilans na PRADZIE). Zaokraglony pasek AUTO jest wyjatkiem uzasadnionym
  // tym, ze przedstawia fizyczna bateria; tutaj nie ma zadnego przedmiotu.
  bar(s, grid::MARGIN, 82, grid::DATA_R - grid::MARGIN, 9,
      static_cast<float>(gain) / static_cast<float>(cfg::PV_KOSZT_PLN),
      dim ? col::MUTE : col::OK, col::LINE);

  // --- DWIE MALE LICZBY (y=104) ------------------------------------------------
  if (haveMqtt) {
    char gb[16], l[40];
    groupNum(gb, sizeof(gb), static_cast<uint32_t>(gain));
    snprintf(l, sizeof(l), "wróciło %s zł", gb);
    plex::str(s, plex::f13(), l, grid::MARGIN, 104, cSec);
    const int32_t rest = cfg::PV_KOSZT_PLN - gain;
    if (rest > 0) {
      char rb[16];
      groupNum(rb, sizeof(rb), static_cast<uint32_t>(rest));
      snprintf(l, sizeof(l), "zostało %s zł", rb);
    } else {
      snprintf(l, sizeof(l), "instalacja spłacona");
    }
    plex::strRight(s, plex::f13(), l, grid::DATA_R, 104, cSec);
  } else {
    // STAN (a): procent i wykres stoja, ale BIEZACEJ KWOTY NIE PODAJEMY. Ostatni
    // punkt historii jest z konca poprzedniego miesiaca — napisanie go tutaj jako
    // "wróciło X zl" twierdziloby, ze to stan na dzis, a to jest dokladnie to jedno
    // zdanie, ktorego bez MQTT wypowiedziec nie mozemy. Zamiast liczby: skad procent.
    int hy = 0, hm = 0;
    paybackYm(kPaybackHistN - 1, hy, hm);
    char l[48];
    snprintf(l, sizeof(l), "bez danych bieżących · historia do %d.%d", hm, hy);
    plex::str(s, plex::f13(), l, grid::MARGIN, 104, col::MUTE);
  }

  s.drawFastHLine(grid::MARGIN, 112, grid::W - 2 * grid::MARGIN, col::LINE);

  // --- WYKRES (y=124..188, opisy osi X na y=199) -------------------------------
  // OS Y JEST STALA I ZACZYNA SIE OD ZERA: 0 zl u dolu, cfg::PV_KOSZT_PLN u gory.
  // Nie skalujemy jej do danych, jak robi to wykres pokoi — tam pytanie brzmi "jak
  // bardzo sie roznia", tutaj "jak daleko do celu", a przy osi dopasowanej do maksimum
  // krzywa zawsze dobijalaby do gornej krawedzi i ekran twierdzilby, ze jestesmy
  // u konca. Gorna krawedz JEST celem — dlatego linia odniesienia lezy dokladnie na
  // niej i jest podpisana kwota.
  constexpr int pX0 = grid::MARGIN;    // 7
  constexpr int pX1 = grid::DATA_R;    // 313
  constexpr int pY0 = 124;             // poziom cfg::PV_KOSZT_PLN
  constexpr int pY1 = 188;             // poziom 0 zl (etykiety lat na 199, wciaz < 206)
  const int fcN = (left > 0) ? left : 0;
  const int total = kPaybackHistN + fcN;   // punktow na osi X (>= 2, patrz PaybackHist.h)
  auto xOf = [&](int i) {
    return pX0 + (i * (pX1 - pX0)) / (total - 1);
  };
  auto yOf = [&](int32_t v) {
    if (v <= 0) return pY1;
    if (v >= cfg::PV_KOSZT_PLN) return pY0;
    return pY1 - static_cast<int>((v * (pY1 - pY0)) / cfg::PV_KOSZT_PLN);
  };

  // Linia kosztu (cel) + jej kwota, oraz linia zera. Obie col::LINE — to CHROME,
  // nie dane; kolorem mowi wylacznie krzywa.
  s.drawFastHLine(pX0, pY0, pX1 - pX0, col::LINE);
  s.drawFastHLine(pX0, pY1, pX1 - pX0, col::LINE);
  {
    char cb[16], cl[24];
    groupNum(cb, sizeof(cb), static_cast<uint32_t>(cfg::PV_KOSZT_PLN));
    snprintf(cl, sizeof(cl), "%s zł", cb);
    plex::strRight(s, plex::f10(), cl, pX1, pY0 - 3, col::MUTE);
  }

  // OS X: pionowa kreska na kazdym STYCZNIU + podpis roku. Podpis wypada, gdy nie ma
  // dla niego miejsca obok poprzedniego — przy prognozie na 8 lat wychodzi 8 stycznii
  // na 306 px (38 px na rok, "2031" w f10 ma 22 px), czyli mieszcza sie wszystkie,
  // ale przy dluzszej prognozie krata zostaje, a zlepiajace sie liczby znikaja.
  {
    int lastRight = -100;
    for (int i = 0; i < total; ++i) {
      int yy = 0, mm = 0;
      paybackYm(i, yy, mm);
      if (mm != 1) continue;
      const int x = xOf(i);
      s.drawFastVLine(x, pY0, pY1 - pY0, col::LINE);
      char yb[8];
      snprintf(yb, sizeof(yb), "%d", yy);
      const int w = plex::width(plex::f10(), yb);
      if (x - w / 2 >= lastRight + 6 && x + w / 2 <= pX1) {
        plex::strCenter(s, plex::f10(), yb, x, pY1 + 11, col::MUTE);
        lastRight = x + w / 2;
      }
    }
  }

  // KRZYWA HISTORII: linia ciagla, col::OK, grubosc 2 px (drugi przebieg o +1 w y) —
  // ta sama decyzja, co przy liniach pokoi: 1 px na zaparowanym ST7789 z 2 m ginie.
  // Zielen jest tu tym samym zielonym, co "oddane do sieci" na PRADZIE, i to jest
  // zgodne z kontraktem kolorow z v167: te pieniadze POCHODZA z produkcji PV.
  const uint16_t cCurve = dim ? col::MUTE : col::OK;
  int px = -1, py = -1;
  for (int i = 0; i < kPaybackHistN; ++i) {
    const int x = xOf(i), y = yOf(static_cast<int32_t>(kPaybackHist[i]));
    if (px >= 0) {
      s.drawLine(px, py, x, y, cCurve);
      s.drawLine(px, py + 1, x, y + 1, cCurve);
    }
    px = x;
    py = y;
  }

  // PROGNOZA: linia PRZERYWANA, col::MUTE i grubosc 1 px — dwie rozne roznice wobec
  // krzywej historii, bo jedna by nie wystarczyla. Kreskowanie mowi "to nie zdarzylo
  // sie jeszcze", a odebranie zieleni i grubosci mowi to samo drugi raz, dla kogos,
  // kto patrzy z drugiego konca pokoju i przerw nie rozroznia. Zielona linia na tym
  // ekranie ma znaczyc PIENIADZE, KTORE JUZ WROCILY — ani zlotowki wiecej.
  // Wzor kreski: 2 miesiace rysowane, 2 pominiete (~6 px / 6 px przy 100 punktach).
  if (fcN > 0) {
    int32_t v = histLast;
    int qx = px, qy = py;
    for (int k = 1; k <= fcN; ++k) {
      v += rate;
      if (v > cfg::PV_KOSZT_PLN) v = cfg::PV_KOSZT_PLN;
      const int x = xOf(kPaybackHistN - 1 + k), y = yOf(v);
      if (((k - 1) / 2) % 2 == 0) s.drawLine(qx, qy, x, y, col::MUTE);
      qx = x;
      qy = y;
    }
  }
}

// Dolny pas ZWROT: tempo po lewej + dwie linijki kontekstu po prawej. Konwencja jak
// w v3AutoBottom/v3HomeBottom — wlasne tlo, cienka linia u gory pasa, rysowane WPROST
// na TFT (pas 206..239 lezy poza sprite'em).
// ZADNA z tych linii NIE POWTARZA liczby z gory ekranu: tempo jest jedyna wielkoscia,
// z ktorej cala prognoza sie bierze, a ktorej nigdzie wyzej nie widac, a czas "jeszcze
// 5 lat 3 mies." to ta sama informacja co data, ale wyrazona jako DLUGOSC — z dwoch
// metrow "11.2031" nie mowi nic o tym, czy to blisko.
void v3PaybackBottom(TFT_eSPI& tft, const CostModel* cp) {
  (void)cp;   // pas dolny zyje wylacznie z historii — patrz komentarz nizej
  tft.fillRect(0, 206, grid::W, 34, col::BG);
  tft.drawFastHLine(grid::MARGIN, 210, grid::W - 2 * grid::MARGIN, col::LINE);

  // CELOWO BEZ cp: tempo i czas do zwrotu licza sie WYLACZNIE z kPaybackHist, wiec ten
  // pas wyglada tak samo przy zywym MQTT i przy jego ciszy. Gdyby czytal pvPln,
  // musialby tez umiec stan (a) i (c) — a nie mialby czym: historia nie ma wieku.
  const int rate = paybackRate();
  const int32_t histLast = static_cast<int32_t>(kPaybackHist[kPaybackHistN - 1]);
  const int left = paybackMonthsLeft(histLast, rate);

  plex::str(tft, plex::f11(), "TEMPO", grid::MARGIN, 222, col::MUTE);
  char t[24];
  if (rate > 0) snprintf(t, sizeof(t), "+%d zł/mies.", rate);
  else snprintf(t, sizeof(t), "bez przyrostu");
  plex::str(tft, plex::f13(), t, grid::MARGIN, 236, rate > 0 ? col::OK : col::MUTE);

  // Szerokosci: "ze średniej 12 miesięcy" 137 px (x=176..313), "jeszcze 5 lat 3 mies."
  // 129 px (x=184..313). Kolumna tempa konczy sie na ~x=95, wiec ponad 80 px luzu.
  plex::strRight(tft, plex::f13(), "ze średniej 12 miesięcy", grid::DATA_R, 222, col::MUTE);
  char l2[40];
  if (left < 0) {
    snprintf(l2, sizeof(l2), "prognoza niedostępna");
  } else if (left == 0) {
    snprintf(l2, sizeof(l2), "instalacja spłacona");
  } else {
    const int ly = left / 12, lm = left % 12;
    if (ly > 0 && lm > 0) snprintf(l2, sizeof(l2), "jeszcze %d %s %d mies.", ly, rokNoun(ly), lm);
    else if (ly > 0) snprintf(l2, sizeof(l2), "jeszcze %d %s", ly, rokNoun(ly));
    else snprintf(l2, sizeof(l2), "jeszcze %d mies.", lm);
  }
  plex::strRight(tft, plex::f13(), l2, grid::DATA_R, 236, col::MUTE);
}

// ============================================================ POKOJE ===========
// Wykres temperatur wielu pokoi na WSPOLNEJ, OPISANEJ osi Y (°C) i osi X (ruchome
// okno 24 h). Zastapil dawna liste wierszy ze sparkline w tle: wlasciciel odrzucil
// tamten uklad wprost ("wykresy bez osi X i Y opisanych"). Kluczowa roznica: skala
// pionowa jest TERAZ WSPOLNA dla wszystkich pokoi (nie per-wiersz), wiec 1° roznicy
// miedzy pokojami widac od razu i mozna je porownac. Biezace wartosci + legenda
// kolorow siedza w dolnym pasie (v3HomeBottom), zeby na samym wykresie zostaly tylko
// linie i osie.

// Szesc rozroznialnych na jasnym tle kolorow linii, indeksowanych slotem pokoju
// (RoomHistory / Settings::ble[]). const -> .rodata we flashu: zero statycznego RAM-u.
// Pierwsze cztery to kolory danych z palety motywu; piaty i szosty to JEDYNE surowe
// hexy w tym ekranie — paleta tv3 nie ma piatego ani szostego koloru serii.
static const uint16_t kRoomColors[6] = {
  col::RAIN,   // 0 niebieski
  col::GRID,   // 1 czerwony
  col::OK,     // 2 zielony
  col::WARN,   // 3 bursztyn
  0x8A35,      // 4 fiolet  #8E44AD
  0x0C6F,      // 5 morski  #0E8C7A
};

// Polska odmiana rzeczownika "czujnik" wg liczby (naglowek: "1 czujnik", "4 czujniki",
// "5 czujnikow"). Zwraca literal z flasha — bez bufora w RAM.
const char* czujnikNoun(int n) {
  const int t = n % 100, u = n % 10;
  if (n == 1) return "czujnik";
  if (u >= 2 && u <= 4 && (t < 12 || t > 14)) return "czujniki";
  return "czujników";
}

void v3Home(TFT_eSPI& s, const RoomModel* rmp, const RoomHistory* rhp, uint32_t nowMs) {
  (void)nowMs;   // etykiety godzin bierzemy z time() (czas lokalny), nowMs tu zbedny
  s.fillRect(0, 0, grid::W, 206, col::BG);
  static const RoomModel kEmpty{};
  const RoomModel& rm = rmp ? *rmp : kEmpty;

  // Naglowek: ile czujnikow widzi bramka + okno wykresu. Pusty stan (0 czujnikow) jak dotad.
  char hr[24];
  snprintf(hr, sizeof(hr), "%d %s · 24 h", rm.sensorCount, czujnikNoun(rm.sensorCount));
  lightHeader(s, "POKOJE", rm.sensorCount ? hr : nullptr);

  if (rm.count == 0) {
    plex::strCenter(s, plex::f20(), "Brak czujników", grid::W / 2, 110, col::MUTE);
    plex::strCenter(s, plex::f13(), "dodaj czujnik BLE w panelu", grid::W / 2, 134, col::MUTE);
    return;
  }

  // ---- GORA: biezace temperatury WIELKA czcionka, 2 kolumny x 2 wiersze ----
  // Wlasciciel: dolna mini-legenda byla nieczytelna. Wartosci ida na gore duzym fontem,
  // a wykres scisniety i zepchniety w dol. Kolejnosc cel = kolejnosc czujnikow (slot):
  // lewa kolumna pierwsze dwa, prawa nastepne dwa. Kolorowa kropka przy nazwie wiaze
  // wartosc z linia na wykresie (legenda wtopiona w naglowki — osobnej juz nie ma).
  // FONT: temperatura na f20, NIE f24 — f24 to okrojony font ZEGARA (spacja . 0-9 : °,
  // 14 glifow, BEZ przecinka), wiec "21,2" wyszloby "212". f20 ma pelne 120 glifow.
  // Po prawej od temperatury: wilgotnosc (gora) + zrodlo sygnalu (dol): przez bramke
  // Shelly (r.viaGw=true) albo wprost do ESP (false) — wlasciciel chce to rozroznic.
  {
    const int colX[2]     = {grid::MARGIN, grid::W / 2 + 4};   // 7, 164
    const int nameBase[2] = {48, 90};                          // linie bazowe nazw (f13)
    const int tempBase[2] = {72, 114};                         // linie bazowe temperatur (f20)
    const int metaDX      = 70;                                // przesuniecie w prawo: wilgotnosc + zrodlo
    for (int i = 0; i < rm.count && i < 4; ++i) {
      const RoomRow& r = rm.rows[i];
      const int cx = colX[i / 2], row = i % 2;
      // (v158) Bylo 900 wpisane liczba; ta sama wartosc stoi teraz w Config.h jako
      // cfg::BLE_STALE_MS razem z pomiarem, z ktorego wynika (wieki 22-78 s na zywo).
      // r.ageS jest w sekundach, prog w ms — stad dzielenie.
      const bool stale = r.ageS >= cfg::BLE_STALE_MS / 1000;
      const uint16_t dotC = (r.slot >= 0 && r.slot < RoomHistory::ROOMS) ? kRoomColors[r.slot % 6] : col::MUTE;
      s.fillCircle(cx + 3, nameBase[row] - 4, 3, dotC);
      plex::str(s, plex::f13(), r.name ? r.name : "-", cx + 11, nameBase[row],
                stale ? col::MUTE : col::SECOND);
      char tvb[12];
      if (r.hasTemp) { char t[10]; fmt1(t, sizeof(t), r.tempC); snprintf(tvb, sizeof(tvb), "%s°", t); }
      else snprintf(tvb, sizeof(tvb), "-");
      plex::str(s, plex::f20(), tvb, cx + 11, tempBase[row], (stale || !r.hasTemp) ? col::MUTE : col::PANEL);
      // Prawa strona komorki: wilgotnosc (gora) + zrodlo (dol). Jest na to miejsce obok f20.
      if (r.hasHum) {
        char hm[8];
        snprintf(hm, sizeof(hm), "%.0f%%", r.humidity);
        plex::str(s, plex::f13(), hm, cx + metaDX, tempBase[row] - 9, stale ? col::MUTE : col::SECOND);
      }
      if (r.hasTemp)
        plex::str(s, plex::f11(), r.viaGw ? "Shelly" : "ESP", cx + metaDX, tempBase[row] + 2, col::MUTE);
    }
  }

  // ---- DOL: wykres SCISNIETY (~50% wysokosci) i zepchniety w dol pod wartosci ----
  // plotX0 zostawia z lewej ~26 px na etykiety osi Y (°C). plotY1=188 trzyma etykiety
  // godzin (y=199) jeszcze w sprite (<206). plotY0=126: obszar zaczyna sie pod blokiem
  // temperatur (dolny wiersz f20 + zrodlo konczy sie ~119).
  const int plotX0 = grid::MARGIN + 26;   // 33
  const int plotX1 = grid::DATA_R;         // 313
  const int plotY0 = 126;                  // gora obszaru (pod blokiem wartosci)
  const int plotY1 = 188;                  // baza wykresu

  // WSPOLNE min/max po WSZYSTKICH pokojach: i po historii (t10 != NO_T), i po biezacych
  // temperaturach (pokoj bez historii tez ma miescic sie w osi). Przy okazji liczymy, ile
  // pokoi ma dosc probek na linie (>=2) — zero => stan "zbieram dane".
  float tmin = 1e9f, tmax = -1e9f;
  int roomsPlotted = 0;
  for (int i = 0; i < rm.count; ++i) {
    const RoomRow& r = rm.rows[i];
    if (r.hasTemp) {
      if (r.tempC < tmin) tmin = r.tempC;
      if (r.tempC > tmax) tmax = r.tempC;
    }
    if (!rhp || r.slot < 0 || r.slot >= RoomHistory::ROOMS) continue;
    int valid = 0;
    for (int k = 0; k < RoomHistory::SLOTS; ++k) {
      const int16_t v = rhp->t10[r.slot][k];
      if (v == RoomHistory::NO_T) continue;
      const float t = v / 10.f;
      if (t < tmin) tmin = t;
      if (t > tmax) tmax = t;
      ++valid;
    }
    if (valid >= 2) ++roomsPlotted;
  }
  if (tmin > tmax) { tmin = 20.f; tmax = 22.f; }   // brak jakichkolwiek liczb — zakres zapasowy

  // OS Y: padding po 0,5° i zaokraglenie do pelnych stopni; wymuszony MINIMALNY rozstaw 3°
  // (przy realnym rozrzucie ~1° bliskie linie inaczej by sie zlepily) rozciagniety
  // symetrycznie. Krok 1° do rozpietosci 5°, dalej 2°. Gorny prog wyrownany do
  // wielokrotnosci kroku, zeby ostatnia podzialka trafiala dokladnie w krawedz wykresu.
  int yLo = static_cast<int>(floorf(tmin - 0.5f));
  int yHi = static_cast<int>(ceilf(tmax + 0.5f));
  if (yHi - yLo < 3) { const int need = 3 - (yHi - yLo); yLo -= need / 2; yHi += need - need / 2; }
  const int step = (yHi - yLo <= 5) ? 1 : 2;
  if ((yHi - yLo) % step) yHi += step - ((yHi - yLo) % step);
  // Mapowanie temperatury na y (baza=yLo u dolu, yHi u gory). Uzywane przez podzialke i linie.
  auto yOf = [&](float t) {
    return plotY1 - static_cast<int>((t - yLo) / static_cast<float>(yHi - yLo) * (plotY1 - plotY0) + 0.5f);
  };

  // Poziome linie podzialki na cala szerokosc + etykieta "%d°" do prawej, tuz przed osia.
  for (int tv = yLo; tv <= yHi; tv += step) {
    const int y = yOf(static_cast<float>(tv));
    s.drawFastHLine(plotX0, y, plotX1 - plotX0, col::LINE);
    char lbl[8];
    snprintf(lbl, sizeof(lbl), "%d°", tv);
    plex::strRight(s, plex::f10(), lbl, plotX0 - 3, y + 3, col::MUTE);
  }

  // OS X: ruchome 24 h. Pionowe linie siatki + etykiety godzin co 6 h. Pozycje liczymy z
  // CZASU (t godzin wstecz od teraz), nie z numeru slotu — dzieki temu podpis "12" stoi
  // tam, gdzie realnie byla godzina 12, niezaleznie od fazy okna. Zegar nieustawiony
  // (przed NTP) -> zapasowo pokazujemy offset "−Nh" zamiast zmyslonej godziny.
  const time_t now = time(nullptr);
  const bool clockOk = now > 1700000000;
  for (int th = 24; th >= 0; th -= 6) {
    const int x = plotX1 - static_cast<int>((th / 24.f) * (plotX1 - plotX0) + 0.5f);
    if (th != 0 && th != 24) s.drawFastVLine(x, plotY0, plotY1 - plotY0, col::LINE);
    char hb[8];
    if (th == 0) snprintf(hb, sizeof(hb), "teraz");
    else if (clockOk) {
      const time_t moment = now - static_cast<time_t>(th) * 3600;
      struct tm tmv{};
      localtime_r(&moment, &tmv);
      snprintf(hb, sizeof(hb), "%d", tmv.tm_hour);
    } else {
      snprintf(hb, sizeof(hb), "−%dh", th);
    }
    // Skrajne podpisy wyrownane do wnetrza (teraz do prawej, −24h do lewej), zeby "teraz"
    // nie wyszlo poza ekran, a lewy podpis nie wlazl w kolumne etykiet °C; srodkowe centrowane.
    if (th == 0) plex::strRight(s, plex::f10(), hb, plotX1, plotY1 + 11, col::MUTE);
    else if (th == 24) plex::str(s, plex::f10(), hb, plotX0, plotY1 + 11, col::MUTE);
    else plex::strCenter(s, plex::f10(), hb, x, plotY1 + 11, col::MUTE);
  }

  // LINIE POKOI na wierzchu siatki. Kolor = slot pokoju. Dziura (NO_T, np. przerwa w
  // zasilaniu) PRZERYWA linie — nie laczymy w poprzek luki. Grubosc 2 px (drugi segment
  // przesuniety o +1 w y), bo cienka linia na tanim, zaparowanym ST7789 z 2 m ginela.
  if (rhp) {
    for (int i = 0; i < rm.count; ++i) {
      const RoomRow& r = rm.rows[i];
      if (r.slot < 0 || r.slot >= RoomHistory::ROOMS) continue;
      int valid = 0;
      for (int k = 0; k < RoomHistory::SLOTS; ++k)
        if (rhp->t10[r.slot][k] != RoomHistory::NO_T) ++valid;
      if (valid < 2) continue;   // <2 probek: pokoj tylko w legendzie (wartosc biezaca)
      const uint16_t c = kRoomColors[r.slot % 6];
      int px = -1, py = -1;
      for (int k = 0; k < RoomHistory::SLOTS; ++k) {   // k=0 najstarsza -> lewa krawedz
        const int16_t v = rhp->t10[r.slot][rhp->idx(k)];
        if (v == RoomHistory::NO_T) { px = -1; continue; }
        const int x = plotX0 + (k * (plotX1 - plotX0)) / (RoomHistory::SLOTS - 1);
        const int y = yOf(v / 10.f);
        if (px >= 0) { s.drawLine(px, py, x, y, c); s.drawLine(px, py + 1, x, y + 1, c); }
        px = x; py = y;
      }
    }
  }

  // Sa czujniki, ale zaden nie ma jeszcze 2 probek: osie juz stoja (z biezacych temperatur),
  // dorysuj delikatny komunikat na srodku obszaru — ekran nigdy nie jest pusty.
  if (roomsPlotted == 0)
    plex::strCenter(s, plex::f13(), "wykres pojawi się po zebraniu danych",
                    (plotX0 + plotX1) / 2, (plotY0 + plotY1) / 2, col::MUTE);
}

// Dolny pas POKOJE. Wartosci biezace przeniesione na GORE ekranu (duzy font, 2 kolumny),
// wiec dawna mini-legenda kolorow jest juz niepotrzebna — kropki stoja przy nazwach na
// gorze. Tu zostaje tylko cichy podpis, ze pionowa skala wykresu jest WSPOLNA dla wszystkich
// pokoi (bez tego ktos moglby czytac kazda linie we wlasnej skali). Rysowany WPROST na TFT.
void v3HomeBottom(TFT_eSPI& tft, const RoomModel* rmp) {
  tft.fillRect(0, 206, grid::W, 34, col::BG);
  tft.drawFastHLine(grid::MARGIN, 210, grid::W - 2 * grid::MARGIN, col::LINE);
  if (!rmp || rmp->count == 0) return;   // brak czujnikow: pusty pas (obszar wykresu ma komunikat)
  plex::strCenter(tft, plex::f11(), "WSPÓLNA SKALA · RUCHOME 24 H", grid::W / 2, 227, col::MUTE);
}

// ============================================================ OGRZEWANIE =======
// Makieta 15. Pelnojasne tlo, CWU + wykres palnika.

void v3Boiler(TFT_eSPI& s, const vi::Model* bp, const BurnerHistory* bhp) {
  s.fillRect(0, 0, grid::W, 206, col::BG);

  // (v161) STAN (a): NIGDY nie bylo udanego odczytu. Do v160 netTask przy KAZDYM
  // bledzie robil `gVi.valid = false`, wiec ten warunek lapal takze "mielismy dane
  // trzy minuty temu" — i caly ekran gasl po jednej nieudanej probie. Teraz netTask
  // gVi przy bledzie NIE DOTYKA, wiec `valid == false` znaczy dokladnie to, co pisze:
  // od wlaczenia urzadzenia piec nie odpowiedzial ANI RAZU (brak autoryzacji, zly
  // token, martwa chmura). Wtedy "Piec nie odpowiada" jest PRAWDA i zostaje.
  if (!bp || !bp->valid) {
    lightHeader(s, "OGRZEWANIE", nullptr, Fresh::UNKNOWN);
    plex::strCenter(s, plex::f20(), "Piec nie odpowiada", grid::W / 2, 110, col::MUTE);
    plex::strCenter(s, plex::f13(), diag().viErr[0] ? diag().viErr : "skonfiguruj w panelu",
                    grid::W / 2, 134, col::MUTE);
    return;
  }
  const vi::Model& b = *bp;

  // STAN (b) kontra (c): swieze kontra stare. Prog cfg::VI_STALE_MS = 8 min, czyli
  // 2,5 kadencji odpytu (180 s) — jedno nieudane pobranie NIE robi z danych starych,
  // trzecie z rzedu juz tak (ponowienie po bledzie leci co 120 s). Stempel `b.okAt`
  // jest WLASNOSCIA modelu i ustawia go Viessmann.cpp wylacznie na sciezce sukcesu,
  // tuz przed `out = m` — nie ma jak drgnac po bledzie.
  const uint32_t viAge = okAgeS(b.okAt);
  const bool viOld = !freshMs(b.okAt, cfg::VI_STALE_MS);

  // Naglowek + plakietka wygasania autoryzacji.
  plex::str(s, plex::f11(), "OGRZEWANIE", grid::MARGIN, 22, col::SECOND);
  {
    const int dl = vi::daysLeft();
    char badge[24];
    if (dl >= 0 && dl <= 30) {
      snprintf(badge, sizeof(badge), "odnów dostęp - panel");
      const int tw = plex::width(plex::f13(), badge);
      const int bx = grid::W - grid::MARGIN - tw - 14;
      s.fillRoundRect(bx, 8, tw + 14, 18, 4, col::WARNBG);
      plex::str(s, plex::f13(), badge, bx + 7, 21, col::WARN);
    } else if (viOld) {
      // (v161) WIEK zamiast "dostęp N dni" — ten sam wiersz, ktory i tak tam byl
      // (wzorzec v158 z ekranu glownego: wiek wchodzi w miejsce mniej pilnej tresci,
      // zadnego nowego elementu). Kolejnosc jest swiadoma: plakietka "odnów dostęp"
      // WYGRYWA z wiekiem, bo wygasajaca autoryzacja jest zwykle PRZYCZYNA tego, ze
      // dane sa stare, i jako jedyna mowi wlascicielowi, co ma zrobic.
      // Szerokosc: najdluzszy wariant "sprzed 89 min" = 80 px w f13, wyrownany do
      // prawej na x=313, czyli zaczyna sie na x=233; etykieta "OGRZEWANIE" (f11)
      // konczy sie ponizej x=80. Zero kolizji.
      char ago[24];
      agoWords(ago, sizeof(ago), viAge);
      plex::strRight(s, plex::f13(), ago, grid::W - grid::MARGIN, 22, col::WARN);
    } else if (dl >= 0) {
      snprintf(badge, sizeof(badge), "dostęp %d dni", dl);
      plex::strRight(s, plex::f13(), badge, grid::W - grid::MARGIN, 22, col::MUTE);
    }
  }
  s.drawFastHLine(grid::MARGIN, 30, grid::W - 2 * grid::MARGIN, col::LINE);

  // (v161) DECYZJA PER POLE — najwazniejsza czesc tej zmiany, wiec stoi wprost tu.
  // Nie wszystko, co przyszlo z pieca, wolno pokazac jako "stare, ale nasze":
  //
  //   POKAZUJEMY (wyciszone + wiek w naglowku):
  //     * temperatura CWU — zbiornik z woda ma ogromna bezwladnosc cieplna; 8 minut
  //       to dla niego pojedyncze stopnie, wiec ostatnia znana wartosc nadal odpowiada
  //       na pytanie "czy jest ciepla woda";
  //     * nastawa CWU i tryb (komfort/eko/wyłączona) — to KONFIGURACJA, nie pomiar.
  //       Zmienia sie recznie, raz na tygodnie. Stara wartosc jest tu praktycznie
  //       zawsze aktualna;
  //     * temperatura zasilania — pomiar chwilowy, ale wielkosc CIAGLA i zmieniajaca
  //       sie w skali minut przy 100-litrowej instalacji; wyciszona i podpisana
  //       wiekiem niesie wiecej niz pusty ekran.
  //
  //   NIE POKAZUJEMY:
  //     * "teraz: włączony / teraz: wyłączony" (b.burnerActive). To stan DWUSTANOWY
  //       o zyciu krotszym niz nasza wlasna kadencja — Viessmann.h opisuje to wprost:
  //       cykl grzania CWU potrafi zaczac sie i skonczyc MIEDZY dwoma odpytami co
  //       3 minuty. Slowo "teraz" przy wartosci sprzed kwadransa jest po prostu
  //       nieprawda, a wyciszenie jej nie naprawia: czytelnik przeczyta "palnik
  //       pracuje", nie "palnik pracowal kiedys". Brak odczytu jest tu UCZCIWSZY niz
  //       stara wartosc — i mamy juz na to gotowy, prawdziwy napis ("brak odczytu"),
  //       uzywany dotad przy braku cechy hasBurnerState.
  //     * modulacja palnika — z tego samego powodu; na ekranie i tak nie ma jej jako
  //       liczby, wchodzi wylacznie do WYKRESU doby, a ten jest odporny z definicji
  //       (kazdy slupek to wlasny slot czasu, brak nowych probek = brak nowych slupkow,
  //       a nie stary slupek udajacy biezacy).
  //
  //   LICZNIKI NARASTAJACE (godziny palnika, liczba startow, gaz dzisiaj) sa osobnym
  //   przypadkiem i traktujemy je osobno: stara wartosc licznika jest ZANIZONA, ale
  //   nigdy zawyzona — nie klamie o kierunku. Gaz z dolnego pasa ma wlasna obsluge
  //   w v3BoilerBottom (razem z pulapka polnocy). Godzin i startow ten ekran nie
  //   pokazuje wcale — sa tylko w /api/diag.
  const uint16_t cMain = viOld ? col::MUTE : col::PANEL;
  const uint16_t cSec = viOld ? col::MUTE : col::SECOND;

  // Wielka CWU.
  char big[12];
  snprintf(big, sizeof(big), b.hasDhwTemp ? "%.0f°" : "-", b.dhwTempC);
  const int bw = plex::str(s, plex::f52(), big, grid::MARGIN, 74, cMain);
  char sub[32];
  if (b.hasDhwTarget) snprintf(sub, sizeof(sub), "ciepła woda · zadane %.0f°", b.dhwTargetC);
  else snprintf(sub, sizeof(sub), "ciepła woda");
  plex::str(s, plex::f13(), sub, grid::MARGIN, 92, cSec);
  (void)bw;

  // Prawa: zasilanie + tryb.
  if (b.hasSupplyTemp) {
    char sup[20];
    snprintf(sup, sizeof(sup), "zasilanie %.0f°", b.supplyTempC);
    plex::strRight(s, plex::f13(), sup, grid::DATA_R, 50, cMain);
  }
  {
    const char* mode = strcmp(b.dhwMode, "comfort") == 0   ? "komfort"
                       : strcmp(b.dhwMode, "eco") == 0     ? "eko"
                       : strcmp(b.dhwMode, "off") == 0     ? "wyłączona"
                       : b.dhwMode[0]                      ? b.dhwMode
                                                           : "-";
    char tr[24];
    snprintf(tr, sizeof(tr), "tryb: %s", mode);
    plex::strRight(s, plex::f13(), tr, grid::DATA_R, 68, cSec);
  }

  // Wykres palnika (modulacja doby).
  plex::str(s, plex::f11(), "PALNIK DZIŚ", grid::MARGIN, 120, col::SECOND);
  {
    // "teraz: ..." WYLACZNIE przy swiezym odczycie — patrz decyzja per pole wyzej.
    const char* st = (viOld || !b.hasBurnerState)
                         ? "brak odczytu"
                         : (b.burnerActive ? "teraz: włączony" : "teraz: wyłączony");
    plex::strRight(s, plex::f13(), st, grid::DATA_R, 120,
                   (!viOld && b.burnerActive) ? col::PV : col::MUTE);
  }
  const int cx = grid::MARGIN, cy = 130, cw = grid::W - 2 * grid::MARGIN, ch = 44;
  const int base = cy + ch;
  s.drawFastHLine(cx, base, cw, col::LINE);
  static const BurnerHistory kEmptyBh{};
  const BurnerHistory& bh = bhp ? *bhp : kEmptyBh;
  const int pk = bh.peak();
  for (int i = 0; i < BurnerHistory::SLOTS; ++i) {
    if (!bh.filled[i] || bh.mod[i] == 0) continue;
    const int x = cx + (i * cw) / BurnerHistory::SLOTS;
    int hh = static_cast<int>((ch - 2) * (bh.mod[i] / static_cast<float>(pk > 0 ? pk : 100)));
    if (hh < 1) hh = 1;
    s.drawFastVLine(x, base - hh, hh, col::PV);
  }
  for (int hh = 0; hh <= 24; hh += 6) {
    char hb[4];
    snprintf(hb, sizeof(hb), "%d", hh);
    plex::strCenter(s, plex::f10(), hb, cx + (hh * cw) / 24, base + 11, col::MUTE);
  }
}

void v3BoilerBottom(TFT_eSPI& tft, const vi::Model* bp) {
  tft.fillRect(0, 206, grid::W, 34, col::BG);
  tft.drawFastHLine(grid::MARGIN, 210, grid::W - 2 * grid::MARGIN, col::LINE);
  plex::str(tft, plex::f11(), "GAZ · DZIŚ", grid::MARGIN, 228, col::SECOND);
  if (bp && bp->valid && bp->hasGas) {
    // (v161) Zuzycie gazu to licznik NARASTAJACY w obrebie doby — stary odczyt jest
    // zanizony, nigdy zawyzony, wiec wolno go pokazac. Ale ma DWIE pulapki i obie sa
    // tu obsluzone, bo to dokladnie ten rodzaj bledu, ktory to wydanie usuwa:
    //
    //  1) POLNOC. Licznik `currentDay` z pieca zeruje sie o polnocy. Odczyt sprzed
    //     polnocy pokazany pod naglowkiem "GAZ · DZIŚ" o 00:30 to WCZORAJSZE zuzycie
    //     podpisane slowem "dziś" — ta sama klasa klamstwa, co wczorajsza krzywa PV
    //     pod napisem "DZIS" (patrz blok polnocy w netTask). Sprawdzamy to bez ani
    //     jednego nowego pola: wiek odczytu w sekundach kontra liczba sekund, ktore
    //     uplynely od lokalnej polnocy. Wiek wiekszy => odczyt jest z poprzedniej
    //     doby => liczby NIE POKAZUJEMY WCALE.
    //  2) WIEK. Poza tym przypadkiem liczba zostaje, ale gdy jest starsza niz
    //     cfg::VI_STALE_MS, rozbicie "woda / grzanie" ustepuje miejsca wiekowi.
    //     Zaden nowy wiersz: to ten sam wiersz, tylko z inna, wazniejsza trescia.
    //     Szerokosc: "12,3 m³ · sprzed 89 min" jest KROTSZE od dotychczasowego
    //     najdluzszego "12,3 m³ · woda 5,4 / grzanie 6,9", wiec pas na pewno mieści.
    const uint32_t age = okAgeS(bp->okAt);
    const bool old = !freshMs(bp->okAt, cfg::VI_STALE_MS);
    bool crossedMidnight = false;
    {
      const time_t now = time(nullptr);
      if (now > 1700000000) {
        struct tm tmv{};
        localtime_r(&now, &tmv);
        const uint32_t sinceMidnight = static_cast<uint32_t>(tmv.tm_hour) * 3600u +
                                       static_cast<uint32_t>(tmv.tm_min) * 60u +
                                       static_cast<uint32_t>(tmv.tm_sec);
        crossedMidnight = age > sinceMidnight;
      }
    }
    if (crossedMidnight) {
      plex::strRight(tft, plex::f13(), "licznik sprzed północy", grid::W - grid::MARGIN, 228,
                     col::WARN);
    } else {
      char g[52];
      if (old) {
        char ago[24];
        agoWords(ago, sizeof(ago), age);
        snprintf(g, sizeof(g), "%.1f m³ · %s", bp->gasDhwM3 + bp->gasHeatM3, ago);
      } else {
        snprintf(g, sizeof(g), "%.1f m³ · woda %.1f / grzanie %.1f",
                 bp->gasDhwM3 + bp->gasHeatM3, bp->gasDhwM3, bp->gasHeatM3);
      }
      for (char* p = g; *p; ++p)
        if (*p == '.') *p = ',';
      plex::strRight(tft, plex::f13(), g, grid::W - grid::MARGIN, 228,
                     old ? col::MUTE : col::PANEL);
    }
  } else {
    plex::strRight(tft, plex::f13(), "brak licznika gazu", grid::W - grid::MARGIN, 228, col::MUTE);
  }
}

// ============================================================ POWIETRZE ========
// Makieta 16. Pelnojasne tlo, badge klasy + skale PM.

void v3Air(TFT_eSPI& s, const AirModel* ap) {
  s.fillRect(0, 0, grid::W, 206, col::BG);
  static const AirModel kEmpty{};
  const AirModel& a = ap ? *ap : kEmpty;

  if (!a.ready) {
    lightHeader(s, "POWIETRZE", nullptr, Fresh::UNKNOWN);
    plex::strCenter(s, plex::f20(), "Brak danych", grid::W / 2, 110, col::MUTE);
    plex::strCenter(s, plex::f13(), a.errorMsg[0] ? a.errorMsg : "obie stacje milczą",
                    grid::W / 2, 134, col::MUTE);
    return;
  }

  char hdr[28];
  snprintf(hdr, sizeof(hdr), "POWIETRZE · %s", a.stationName);
  plex::str(s, plex::f11(), hdr, grid::MARGIN, 22, col::SECOND);
  {
    char smp[20] = "";
    const time_t now = time(nullptr);
    if (a.sampleEpoch > 0 && now > 1700000000) {
      struct tm tmv{};
      const time_t se = static_cast<time_t>(a.sampleEpoch);
      localtime_r(&se, &tmv);
      snprintf(smp, sizeof(smp), "próbka %02d:%02d", tmv.tm_hour, tmv.tm_min);
    }
    // (v158) Ten sam warunek co dotad, ale liczony przez airSampleFresh() — jedna
    // definicja swiezosci probki dla ekranu POWIETRZE, plakietki na ekranie glownym
    // i AirClienta (przelaczanie GA17 -> GA24). Prog: cfg::AIR_SAMPLE_STALE_S.
    const bool fresh = airSampleFresh(a);
    int rx = grid::W - grid::MARGIN;
    freshDot(s, rx - 3, 18, fresh ? Fresh::OK : Fresh::STALE);
    rx -= 12;
    // Stara probka: dopisek w col::WARN zamiast col::MUTE (ten sam kolor "nieswieze",
    // co wiek pogody na ekranie glownym). Napis sie NIE zmienia i nie przybywa —
    // godzina probki i tak juz tam stala.
    plex::strRight(s, plex::f13(), smp, rx, 22, fresh ? col::MUTE : col::WARN);
  }
  s.drawFastHLine(grid::MARGIN, 30, grid::W - 2 * grid::MARGIN, col::LINE);

  // Badge klasy.
  const char* cls = airIndexName(a.index);
  const uint16_t bc = airCol(a.index);
  const int tw = plex::width(plex::f20(), cls);
  s.fillRoundRect(grid::MARGIN, 42, tw + 22, 28, 6, bc);
  plex::str(s, plex::f20(), cls, grid::MARGIN + 11, 63, col::BG);

  char idx[20];
  snprintf(idx, sizeof(idx), "indeks %d z 6", a.index);
  plex::str(s, plex::f13(), idx, grid::MARGIN + tw + 36, 52, col::PANEL);
  plex::str(s, plex::f13(), a.indexPm25 >= a.indexPm10 ? "decyduje PM2,5" : "decyduje PM10",
            grid::MARGIN + tw + 36, 68, col::MUTE);

  // Skale PM2,5 i PM10.
  auto pmRow = [&](int y, const char* lbl, bool has, float val, int idxPm) {
    plex::str(s, plex::f11(), lbl, grid::MARGIN, y - 6, col::SECOND);
    if (has) {
      char v[16];
      snprintf(v, sizeof(v), "%.0f µg/m³", val);
      plex::strRight(s, plex::f13(), v, grid::DATA_R, y - 6, col::PANEL);
      tv3::scale5(s, grid::MARGIN, y + 2, grid::W - 2 * grid::MARGIN, 10,
                  clampf((idxPm - 0.5f) / 6.f, 0.f, 1.f));
    } else {
      plex::strRight(s, plex::f13(), "-", grid::DATA_R, y - 6, col::MUTE);
    }
  };
  pmRow(104, "PM2,5", a.hasPm25, a.pm25, a.indexPm25);
  pmRow(150, "PM10", a.hasPm10, a.pm10, a.indexPm10);
}

void v3AirBottom(TFT_eSPI& tft, const AirModel* ap) {
  tft.fillRect(0, 206, grid::W, 34, col::BG);
  tft.drawFastHLine(grid::MARGIN, 210, grid::W - 2 * grid::MARGIN, col::LINE);
  static const AirModel kEmpty{};
  const AirModel& a = ap ? *ap : kEmpty;
  if (a.ready && a.hasWeather) {
    char t[16];
    snprintf(t, sizeof(t), "%.0f° przy stacji", a.tempC);
    plex::str(tft, plex::f13(), t, grid::MARGIN, 228, col::PANEL);
    char h[16];
    snprintf(h, sizeof(h), "%.0f%% wilg.", a.rh);
    plex::strCenter(tft, plex::f13(), h, grid::W / 2, 228, col::PANEL);
    char p[16];
    snprintf(p, sizeof(p), "%.0f hPa", a.pressureHpa);
    plex::strRight(tft, plex::f13(), p, grid::W - grid::MARGIN, 228, col::PANEL);
  } else {
    plex::str(tft, plex::f13(), a.usingFallback ? "stacja zapasowa - bez pogody" : "brak pomiaru pogody",
              grid::MARGIN, 228, col::MUTE);
  }
}

// ============================================================ SAMOLOTY =========
// Makieta 12. Pelnojasne tlo, lista lotow.

// Obiekt naziemny (nie samolot): callsign zaczyna sie od "SPVAN" ALBO jest przy ziemi i
// wolno jedzie (altFt<=0 i gs<50 kt). Zabezpieczenie na wypadek, gdyby backend przepuscil
// pojazd naziemny do listy — pomijamy go i przy rysowaniu, i przy liczeniu "najbliższe".
// Pojazd naziemny lotniska / smieciowa ramka ADS-B — NIE samolot. Filtrujemy:
//  * callsign SPVAN* (wozy techniczne lotniska, zgloszone przez wlasciciela),
//  * altFt<=0 && gs<50 (stoi na plycie),
//  * gs>700 wezlow (~1300 km/h) — dla samolotu cywilnego niemozliwe; to bledna
//    ramka (u wlasciciela "SPSMIM 914 kt / 1,3 km" — bzdura), a nie realny lot,
//  * gs<40 wezlow przy niskim pulapie (<2000 ft) — kolowanie/pojazd przy lotnisku.
bool isGroundVehicle(const Flight& f) {
  if (strncmp(f.callsign, "SPVAN", 5) == 0) return true;
  if (f.altFt <= 0 && f.gs < 50) return true;
  if (f.gs > 700) return true;                       // predkosc niemozliwa = smieciowa ramka
  if (f.altFt > 0 && f.altFt < 2000 && f.gs < 40) return true;   // kolowanie / woz przy plycie
  return false;
}

void v3Flights(TFT_eSPI& s, const FlightModel& fl, uint32_t nowMs) {
  s.fillRect(0, 0, grid::W, 206, col::BG);
  // Loty NIE maja wieku per-samolot, ale CALA lista ma czas pobrania: diag().flightOkAt.
  // UWAGA: to znacznik MILLIS (ustawiany `diag().flightOkAt = millis()` w .ino), a NIE
  // epoch — dlatego wiek to (nowMs - flightOkAt)/1000, tak samo jak liczy ekran
  // diagnostyki. Wczesniej traktowano to jak epoch i wychodzilo "29743112 min temu"
  // (roznica millis vs epoch ~ 1,78 mld s). flightOkAt==0 = jeszcze nie pobrano.
  char hr[24] = "";
  Fresh fresh = Fresh::UNKNOWN;
  if (diag().flightOkAt == 0) {
    snprintf(hr, sizeof(hr), "nieodpytywane");
  } else {
    const uint32_t ageS = (nowMs - diag().flightOkAt) / 1000;
    // (v158) Bylo 60 s wpisane liczba; teraz cfg::FLIGHT_STALE_MS (45 s = 2,5 x
    // kadencja 15 s). Krocej, nie dluzej, i to celowo: przez 45 s samolot w rejsie
    // przesuwa sie o ~10 km, wiec wiersz starszy niz to juz nie opisuje nieba,
    // w ktore patrzy wlasciciel. Zaraz po wejsciu na ten ekran wiek bywa wiekszy
    // (loty odpytujemy TYLKO gdy ekran jest na wierzchu — patrz needsFlights),
    // wiec bursztynowa kropka przez pierwsza sekunde jest prawda, nie usterka.
    fresh = ageS < cfg::FLIGHT_STALE_MS / 1000 ? Fresh::OK : Fresh::STALE;
    if (ageS < 90) snprintf(hr, sizeof(hr), "odświeżono %lu s temu", static_cast<unsigned long>(ageS));
    else snprintf(hr, sizeof(hr), "odświeżono %lu min temu", static_cast<unsigned long>(ageS / 60));
  }
  lightHeader(s, "NAD NAMI", hr[0] ? hr : nullptr, fresh);

  if (!fl.ready) {
    plex::strCenter(s, plex::f20(), "Pobieram dane...", grid::W / 2, 110, col::MUTE);
    return;
  }
  const int rowY0 = 44, pitch = 52;
  int drawn = 0;
  for (int i = 0; i < fl.count && drawn < 3; ++i) {
    const Flight& f = fl.list[i];
    if (isGroundVehicle(f)) continue;   // pojazd naziemny — nie liczymy do wierszy
    const int y = rowY0 + drawn * pitch;

    if (drawn > 0)
      s.drawFastHLine(grid::MARGIN, y - 14, grid::W - 2 * grid::MARGIN, col::LINE);

    // Znacznik: trojkat (trasa znana) / kolko (nieznana).
    if (f.routeKnown) {
      s.fillTriangle(20, y, 14, y + 12, 26, y + 12, col::ACCENT);
    } else {
      s.drawCircle(20, y + 7, 6, col::MUTE);
    }

    // Callsign + typ.
    const int cw = plex::str(s, plex::f20(), f.callsign, 40, y + 8, col::PANEL);
    if (f.type[0]) plex::str(s, plex::f13(), f.type, 40 + cw + 6, y + 8, col::MUTE);

    // Trasa.
    char route[40];
    if (f.routeKnown) snprintf(route, sizeof(route), "%s → %s", cityOf(f.orig), cityOf(f.dest));
    else snprintf(route, sizeof(route), "trasa nieznana · kurs nieznany");
    plex::str(s, plex::f13(), route, 40, y + 26, f.routeKnown ? col::SECOND : col::MUTE);

    // JEDNOSTKI METRYCZNE. Wysokosc altFt jest w STOPACH -> metry (x0,3048): ponizej
    // 1000 m w metrach ("850 m"), wyzej w kilometrach ("10,4 km"). Predkosc gs jest w
    // WEZLACH -> km/h (x1,852): "796 km/h".
    char alt[16];
    const float altM = f.altFt * 0.3048f;
    if (altM >= 1000.f) {
      char km[8];
      fmt1(km, sizeof(km), altM / 1000.f);   // polski przecinek
      snprintf(alt, sizeof(alt), "%s km", km);
    } else {
      snprintf(alt, sizeof(alt), "%d m", altM > 0.f ? static_cast<int>(altM + 0.5f) : 0);
    }
    plex::strRight(s, plex::f13(), alt, grid::DATA_R, y + 8, col::PANEL);
    char gs[12];
    snprintf(gs, sizeof(gs), "%d km/h", static_cast<int>(f.gs * 1.852f + 0.5f));
    plex::strRight(s, plex::f13(), gs, grid::DATA_R, y + 26, col::MUTE);

    ++drawn;
  }

  // Puste niebo: brak lotow ALBO wszystkie z listy to pojazdy naziemne (odfiltrowane).
  if (drawn == 0) {
    plex::strCenter(s, plex::f20(), "Puste niebo", grid::W / 2, 100, col::MUTE);
    plex::strCenter(s, plex::f13(), "brak samolotów nad zatoką", grid::W / 2, 124, col::MUTE);
  }
}

void v3FlightsBottom(TFT_eSPI& tft, const FlightModel& fl) {
  tft.fillRect(0, 206, grid::W, 34, col::BG);
  tft.drawFastHLine(grid::MARGIN, 210, grid::W - 2 * grid::MARGIN, col::LINE);
  // "najbliższe" = ile REALNIE pokazuje lista (z pominieciem pojazdow naziemnych), max 3
  // — spojnie z v3Flights/isGroundVehicle, zeby licznik nie klamal wzgledem wierszy.
  int shown = 0;
  for (int i = 0; i < fl.count && shown < 3; ++i)
    if (!isGroundVehicle(fl.list[i])) ++shown;
  char b[56];
  snprintf(b, sizeof(b), "w zasięgu: %d · najbliższe %d · loty do Gdańska", fl.total, shown);
  // f10, nie f13: pelny podpis w f13 nie miescil sie w 296 px i ucinal "Gdańska"
  // do "Gdań". Makieta 12 ma tu maly tekst — f10 miesci calosc.
  plex::str(tft, plex::f10(), b, grid::MARGIN, 228, col::MUTE);
}

// ============================================================ DIAGNOSTYKA 1 ====
// Makieta 08 (VIEW_STATS). Ciemny naglowek + zrodla + ciemna stopka.

void v3Diag1(TFT_eSPI& s, uint32_t nowMs, const AirModel* ap) {
  s.fillRect(0, 0, grid::W, 206, col::BG);
  {
    char hr[40], ut[16];
    fmtUptime(ut, sizeof(ut), nowMs / 1000);
    // Licznik "1/2" przy prawej krawedzi — spojnie z v3Diag2 ("2/2"). Para diagnostyki:
    // STATS = 1/2 (zrodla), MEM = 2/2 (stan). FW/uptime zostaja po lewej od licznika.
    snprintf(hr, sizeof(hr), "FW %d · %s · 1/2", FW_VERSION, ut);
    darkHeader(s, "URZĄDZENIE", hr);
  }

  const Diag& d = diag();
  const time_t now = time(nullptr);
  char airName[24] = "powietrze";
  if (ap && ap->stationName[0]) snprintf(airName, sizeof(airName), "powietrze %s", ap->stationName);

  // (v158) KAZDE ZRODLO MA WLASNY PROG. Do v157 ta lista miala jeden warunek —
  // "okAt != 0" — i malowala na zielono godzine ostatniego udanego odczytu BEZ WZGLEDU
  // NA TO, ILE MIALA LAT. Piec (kadencja 3 min) wygladal wiec identycznie po minucie
  // i po dobie milczenia, a to jest dokladnie ten ekran, na ktory patrzy sie wtedy,
  // gdy cos nie dziala. Progi ida z Config.h (cfg::*_STALE_MS), po jednym na zrodlo,
  // bo kadencje roznia sie o dwa rzedy wielkosci: 30 s (falownik) do 15 min (pogoda).
  struct Src {
    const char* name;
    uint32_t okAt;
    uint32_t staleMs;
    bool off;
    const char* note;
  };
  const Src src[6] = {
      {"pogoda", d.weatherOkAt, cfg::WEATHER_STALE_MS, false, ""},
      {airName, d.airOkAt, cfg::AIR_FETCH_STALE_MS, false, ""},
      {"radar", d.radarOkAt, cfg::RADAR_STALE_MS, false, ""},
      // Falownik: prog zalezy od pory — po zachodzie netTask sam schodzi na 5 min
      // (PV_REFRESH_NIGHT_MS), wiec nocny odczyt sprzed 3 min jest zupelnie zdrowy.
      {"falownik", d.pvOkAt, d.pvAsleep ? cfg::PV_STALE_NIGHT_MS : cfg::PV_STALE_MS,
       d.pvAsleep, "śpi - noc"},
      {"piec", d.viOkAt, cfg::VI_STALE_MS, !settings().hasViessmann(), "wyłączony"},
      // Loty: NIE cfg::FLIGHT_STALE_MS (45 s) — ten prog opisuje wiersze na ekranie
      // SAMOLOTY, a nie te liste. Poza tym ekranem netTask lotow w ogole nie odpytuje
      // (gFlightsNeeded), wiec kilkuminutowy wiek jest tu NORMALNY. Stad osobna stala
      // cfg::FLIGHT_LIST_STALE_MS z wlasnym uzasadnieniem (Config.h).
      {"samoloty", d.flightOkAt, cfg::FLIGHT_LIST_STALE_MS, false, "nieodpytywane"},
  };

  const int y0 = 50, pitch = 25;
  for (int i = 0; i < 6; ++i) {
    const int y = y0 + i * pitch;
    plex::str(s, plex::f13(), src[i].name, grid::MARGIN, y, col::PANEL);

    if (src[i].off) {
      char n[24];
      snprintf(n, sizeof(n), "- %s", src[i].note);
      plex::strRight(s, plex::f13(), n, grid::W - grid::MARGIN, y, col::MUTE);
    } else if (src[i].okAt == 0) {
      char n[24];
      snprintf(n, sizeof(n), "- %s", src[i].note[0] ? src[i].note : "czekam");
      plex::strRight(s, plex::f13(), n, grid::W - grid::MARGIN, y, col::WARN);
    } else {
      // Znacznik czasu na zegar scienny: teraz minus wiek.
      // (v158) Trzeci stan: gdy wiek przekroczyl prog TEGO zrodla, godzina zostaje
      // (nadal jest prawdziwa i nadal chce sie ja znac), ale zmienia kolor na WARN
      // i traci "✓" — zamiast niego dostaje WIEK, bo przy szukaniu usterki liczy sie
      // "od ilu" bardziej niz "od kiedy". Szerokosci w f13: "12:34 ✓" = 46 px,
      // najszerszy wariant przeterminowany "12:34 · 45 min" = 82 px, a wiersz ma do
      // dyspozycji 306 px (od MARGIN=7 do W-MARGIN=313). Najdluzsza nazwa zrodla to
      // "powietrze SANDOMIERSKA" = 150 px, wiec 150 + 82 = 232 px i zostaje 74 px
      // przerwy — napisy nie maja jak sie zejsc.
      const uint32_t ageS = (nowMs - src[i].okAt) / 1000;
      const bool fresh = freshMs(src[i].okAt, src[i].staleMs);
      char hm[24] = "OK";
      if (now > 1700000000) {
        const time_t okEpoch = now - static_cast<time_t>(ageS);
        struct tm tmv{};
        localtime_r(&okEpoch, &tmv);
        if (fresh) {
          snprintf(hm, sizeof(hm), "%02d:%02d ✓", tmv.tm_hour, tmv.tm_min);
        } else if (ageS < 5400) {
          snprintf(hm, sizeof(hm), "%02d:%02d · %lu min", tmv.tm_hour, tmv.tm_min,
                   static_cast<unsigned long>(ageS / 60));
        } else {
          snprintf(hm, sizeof(hm), "%02d:%02d · %lu h", tmv.tm_hour, tmv.tm_min,
                   static_cast<unsigned long>(ageS / 3600));
        }
      }
      plex::strRight(s, plex::f13(), hm, grid::W - grid::MARGIN, y,
                     fresh ? col::OK : col::WARN);
    }
    s.drawFastHLine(grid::MARGIN, y + 6, grid::W - 2 * grid::MARGIN, col::LINE);
  }
}

void v3Diag1Bottom(TFT_eSPI& tft, uint32_t heapNow, float cpuTempC) {
  tft.fillRect(0, 206, grid::W, 34, col::PANEL);
  const Diag& d = diag();
  char b[64];
  const uint32_t minH = d.minHeap == 0xFFFFFFFF ? heapNow : d.minHeap;
  snprintf(b, sizeof(b), "RAM %luk min %luk · %.0f °C · WiFi %d · awarie: %u",
           static_cast<unsigned long>(heapNow / 1024), static_cast<unsigned long>(minH / 1024),
           cpuTempC, static_cast<int>(WiFi.RSSI()), static_cast<unsigned>(d.panicCount));
  plex::str(tft, plex::f13(), b, grid::MARGIN, 227, col::ONDARK_DIM);
}

// ============================================================ DIAGNOSTYKA 2 ====
// Makieta 10 (VIEW_MEM). Ciemny naglowek + wskazniki stanu + ciemna stopka.

void v3Diag2(TFT_eSPI& s, uint32_t heapNow, float cpuTempC) {
  s.fillRect(0, 0, grid::W, 206, col::BG);
  darkHeader(s, "STAN URZĄDZENIA", "2/2");

  const Diag& d = diag();
  const int lx = grid::MARGIN, rx = grid::W - grid::MARGIN, bw = grid::W - 2 * grid::MARGIN;

  // Wiersz ze wskaznikiem: etykieta, pasek/skala, wartosc z prawej.
  int y = 46;

  // RAM.
  plex::str(s, plex::f13(), "RAM wolna", lx, y, col::PANEL);
  {
    char v[40];
    const uint32_t minH = d.minHeap == 0xFFFFFFFF ? heapNow : d.minHeap;
    char a[16], mn[16];
    groupNum(a, sizeof(a), heapNow);
    groupNum(mn, sizeof(mn), minH);
    snprintf(v, sizeof(v), "%s B · min %s", a, mn);
    plex::strRight(s, plex::f13(), v, rx, y, col::SECOND);
  }
  tv3::bar(s, lx, y + 4, bw, 7, clampf(heapNow / static_cast<float>(ESP.getHeapSize()), 0.f, 1.f),
           col::OK, col::LINE);
  y += 26;

  // Temperatura ukladu.
  plex::str(s, plex::f13(), "temperatura układu", lx, y, col::PANEL);
  {
    char v[12];
    snprintf(v, sizeof(v), "%.0f °C", cpuTempC);
    plex::strRight(s, plex::f13(), v, rx, y, col::SECOND);
  }
  tv3::scale5(s, lx, y + 4, bw, 8, clampf((cpuTempC - 30.f) / 45.f, 0.f, 1.f));
  y += 26;

  // PSRAM.
  plex::str(s, plex::f13(), "PSRAM", lx, y, col::PANEL);
  {
    const uint32_t tot = ESP.getPsramSize(), freeP = ESP.getFreePsram();
    char v[40];
    snprintf(v, sizeof(v), "%.1f / %.0f MB · radar %lu kB", freeP / 1048576.f, tot / 1048576.f,
             static_cast<unsigned long>(radarmap::FRAMES * radarmap::W * radarmap::H / 1000));
    for (char* p = v; *p; ++p)
      if (*p == '.') *p = ',';
    plex::strRight(s, plex::f13(), v, rx, y, col::SECOND);
    tv3::bar(s, lx, y + 4, bw, 7,
             tot > 0 ? clampf((tot - freeP) / static_cast<float>(tot), 0.f, 1.f) : 0.f, col::PV, col::LINE);
  }
  y += 26;

  // RTC SLOW — pamiec .rtc_noinit przezywajaca restart programowy/OTA (trzyma dlugoterminowe
  // statystyki PIR/LDR: gPir/gLdr). NIE MA API runtime na jej zajetosc (jak getFreeHeap/
  // getFreePsram dla RAM/PSRAM), wiec liczymy ja z sizeof znanych struktur, a pojemnosc to
  // REALNY rozmiar sekcji (7680 B). DOKLADNIE te same liczby, co panel /api/memfull
  // (Portal.cpp) — zeby OBA miejsca pokazujace RTC SLOW mowily to samo. (Do v159 bylo ich
  // trzy: trzecim byl ekran PAMIEC motywow V1/V2, usuniety w v160.) UWAGA: gPvRtc (+40 B,
  // dodane w v113) jest TU i TAM pomijane; policzenie kompletu wymagaloby ZGODNEJ poprawki
  // w obu miejscach. Pasek jak RAM wyzej: udzial WOLNEGO miejsca, zielony (col::OK) —
  // mocno wypelniony == duzo zapasu.
  plex::str(s, plex::f13(), "RTC SLOW", lx, y, col::PANEL);
  {
    const uint32_t used = sizeof(PirRtc) + sizeof(LdrRtc);   // gPir + gLdr (jak /api/memfull)
    constexpr uint32_t usable = 7680;   // realny rozmiar .rtc_noinit — stala jak w Portal.cpp
    char v[40];
    snprintf(v, sizeof(v), "%lu / %lu B · przeżywa restart",
             static_cast<unsigned long>(used), static_cast<unsigned long>(usable));
    plex::strRight(s, plex::f13(), v, rx, y, col::SECOND);
    tv3::bar(s, lx, y + 4, bw, 7, clampf(1.f - used / static_cast<float>(usable), 0.f, 1.f),
             col::OK, col::LINE);
  }
  y += 26;

  // Rysowanie klatki.
  plex::str(s, plex::f13(), "rysowanie klatki", lx, y, col::PANEL);
  {
    const uint32_t per = d.framePeriodUs > 0 ? d.framePeriodUs : 50000;
    char v[44];
    snprintf(v, sizeof(v), "%lu ms · okres %lu ms · %lu kl./s",
             static_cast<unsigned long>(d.frameDrawUs / 1000), static_cast<unsigned long>(per / 1000),
             static_cast<unsigned long>(1000000UL / (per > 0 ? per : 1)));
    plex::strRight(s, plex::f13(), v, rx, y, col::SECOND);
    tv3::bar(s, lx, y + 4, bw, 7, clampf(d.frameDrawUs / static_cast<float>(per), 0.f, 1.f), col::SELF, col::LINE);
  }
  y += 26;

  // Siec. Realne SSID (do 32 znakow) + IP + dBm nie miescily sie w wierszu i wartosc
  // (strRight od rx) zamalowywala etykiete "sieć". Budzet dla SSID = szerokosc od rx do
  // prawej krawedzi etykiety (+8 px luzu), pomniejszony o niezmienna czesc " · IP · dBm";
  // fitSsid obcina SSID wielokropkiem. Dlugosci liczone runtime — zero SSID/IP w kodzie.
  plex::str(s, plex::f13(), "sieć", lx, y, col::PANEL);
  {
    char tail[40];
    snprintf(tail, sizeof(tail), " · %s · %d dBm", WiFi.localIP().toString().c_str(),
             static_cast<int>(WiFi.RSSI()));
    const int avail = rx - (lx + plex::width(plex::f13(), "sieć") + 8);
    char ssid[40];
    fitSsid(ssid, sizeof(ssid), WiFi.SSID().c_str(), avail - plex::width(plex::f13(), tail));
    char v[80];
    snprintf(v, sizeof(v), "%s%s", ssid, tail);
    plex::strRight(s, plex::f13(), v, rx, y, col::MUTE);
  }
  y += 22;

  // Restarty.
  plex::str(s, plex::f13(), "restarty", lx, y, col::PANEL);
  {
    char v[32];
    snprintf(v, sizeof(v), "awarie: %u · powód %u", static_cast<unsigned>(d.panicCount),
             static_cast<unsigned>(d.resetReason));
    plex::strRight(s, plex::f13(), v, rx, y, d.panicCount ? col::WARN : col::MUTE);
  }
}

void v3Diag2Bottom(TFT_eSPI& tft, uint32_t nowMs) {
  tft.fillRect(0, 206, grid::W, 34, col::PANEL);
  char l[24];
  snprintf(l, sizeof(l), "FW %d · stabilna", FW_VERSION);
  plex::str(tft, plex::f13(), l, grid::MARGIN, 227, col::ONDARK_DIM);
  char ut[16], m[24];
  fmtUptime(ut, sizeof(ut), nowMs / 1000);
  snprintf(m, sizeof(m), "praca %s", ut);
  plex::strCenter(tft, plex::f13(), m, grid::W / 2, 227, col::ONDARK_DIM);
  // ASCII "x", nie U+00D7 "×": Plex nie ma glifu mnozenia, a nieznany glif jest po
  // cichu POMIJANY przy rysowaniu — "2×" wychodzilo na ekranie jako "2 - wyjście".
  plex::strRight(tft, plex::f13(), "stuknij 2x - wyjście", grid::W - grid::MARGIN, 227, col::ONDARK_DIM);
}

// ============================================================ RUCH =============
// Brak osobnej makiety - uklad w stylu diagnostyki (08/10): PIR + LDR + fps.

void v3Motion(TFT_eSPI& s, uint32_t nowMs) {
  s.fillRect(0, 0, grid::W, 206, col::BG);
  const Diag& d = diag();
  {
    char hr[24];
    snprintf(hr, sizeof(hr), "%s · %u mV", d.pirState ? "ruch teraz" : "bez ruchu",
             static_cast<unsigned>(d.ldrMv));
    darkHeader(s, "RUCH · ŚWIATŁO", hr);
  }

  // PIR: rytm doby (24 slupki).
  plex::str(s, plex::f11(), "PIR · RYTM DOBY", grid::MARGIN, 48, col::SECOND);
  {
    char ago[24] = "brak od startu";
    if (d.pirLastAt != 0) {
      const uint32_t s2 = (nowMs - d.pirLastAt) / 1000;
      char a[20];
      agoWords(a, sizeof(a), s2);
      snprintf(ago, sizeof(ago), "ruch %s", a);
    }
    plex::strRight(s, plex::f13(), ago, grid::W - grid::MARGIN, 48, d.pirState ? col::OK : col::MUTE);
  }
  {
    const int cx = grid::MARGIN, cy = 56, cw = grid::W - 2 * grid::MARGIN, ch = 44;
    const int base = cy + ch;
    uint32_t mx = 1;
    for (int h = 0; h < 24; ++h)
      if (gPir.byHour[h] > mx) mx = gPir.byHour[h];
    int curH = -1;
    const time_t now = time(nullptr);
    if (now > 1700000000) {
      struct tm tmv{};
      localtime_r(&now, &tmv);
      curH = tmv.tm_hour;
    }
    const int pitch = cw / 24;
    for (int h = 0; h < 24; ++h) {
      const int x = cx + h * pitch;
      const int hh = static_cast<int>(ch * (gPir.byHour[h] / static_cast<float>(mx)));
      const uint16_t bc = (h == curH) ? col::ACCENT : col::RAIN3;
      if (hh > 0) s.fillRect(x, base - hh, pitch - 2, hh, bc);
      else s.drawFastHLine(x, base - 1, pitch - 2, col::LINE);
    }
    s.drawFastHLine(cx, base, cw, col::LINE);
    for (int h = 0; h <= 18; h += 6) {
      char hb[4];
      snprintf(hb, sizeof(hb), "%d", h);
      plex::strCenter(s, plex::f10(), hb, cx + h * pitch + pitch / 2, base + 11, col::MUTE);
    }
  }

  // LDR: poziomy swiatla (3 kolory = ciemno/polmrok/jasno).
  plex::str(s, plex::f11(), "LDR · POZIOMY ŚWIATŁA", grid::MARGIN, 130, col::SECOND);
  {
    char v[16];
    snprintf(v, sizeof(v), "%u mV", static_cast<unsigned>(d.ldrMv));
    plex::strRight(s, plex::f13(), v, grid::W - grid::MARGIN, 130, col::MUTE);
    const uint32_t l0 = gLdr.levelS[0], l1 = gLdr.levelS[1], l2 = gLdr.levelS[2];
    const uint32_t sum = l0 + l1 + l2;
    const int bx = grid::MARGIN, by = 138, bw = grid::W - 2 * grid::MARGIN, bh = 10;
    if (sum > 0) {
      int w0 = static_cast<int>(bw * (l0 / static_cast<float>(sum)));
      int w1 = static_cast<int>(bw * (l1 / static_cast<float>(sum)));
      s.fillRect(bx, by, w0, bh, col::MUTE);
      s.fillRect(bx + w0, by, w1, bh, col::PV);
      s.fillRect(bx + w0 + w1, by, bw - w0 - w1, bh, col::SUN);
    } else {
      s.fillRect(bx, by, bw, bh, col::LINE);
    }
  }

  // fps / rysowanie klatki.
  plex::str(s, plex::f11(), "WYDAJNOŚĆ", grid::MARGIN, 172, col::SECOND);
  {
    const uint32_t per = d.framePeriodUs > 0 ? d.framePeriodUs : 50000;
    char v[40];
    snprintf(v, sizeof(v), "%lu ms · %lu kl./s", static_cast<unsigned long>(d.frameDrawUs / 1000),
             static_cast<unsigned long>(1000000UL / (per > 0 ? per : 1)));
    plex::strRight(s, plex::f13(), v, grid::W - grid::MARGIN, 172, col::PANEL);
  }
}

void v3MotionBottom(TFT_eSPI& tft) {
  tft.fillRect(0, 206, grid::W, 34, col::PANEL);
  // Bez "PIR GPIO13 · LDR GPIO1" — numery pinow nic nie mowia uzytkownikowi.
  // "1,1%" opisane uczciwie: to udzial CZASU POMIARU z wykrytym ruchem (suma HIGH z PIR /
  // sekundy realnego zbierania). NIE "doby" — pomiar zbiera sie przez wiele dni (RTC
  // przezywa OTA), wiec "% doby" wprowadzaloby w blad.
  const float pct = gPir.collectedS ? (gPir.totalMs / 1000.f) / gPir.collectedS * 100.f : 0.f;
  char pcts[8];
  fmt1(pcts, sizeof(pcts), pct);   // polski przecinek dziesietny
  char b[56];
  snprintf(b, sizeof(b), "wyzwoleń PIR: %lu · ruch przez %s%% czasu",
           static_cast<unsigned long>(gPir.rises), pcts);
  plex::str(tft, plex::f13(), b, grid::MARGIN, 227, col::ONDARK_DIM);
}

}  // namespace

// Czy jest teraz noc "do zwiniecia ekranu": ciemno w pokoju (blTarget na poziomie nocnym)
// ORAZ pora nocna wg zegara (okno edytowalne z panelu, domyslnie 22..6). Bez NTP nie
// zgadujemy pory — zwracamy false, wiec przy braku czasu zostaje zwykly, dwukolumnowy ekran
// glowny. Czysty odczyt (time()/blTarget) — bezpieczny takze w watku zrzutu. METODA (nie
// file-static), bo wolaja ja i drawV3/drawV3Bottom (zegar nocny), i render() (wybudzanie
// dotykiem w nocy, WeatherUi.cpp). const — nic nie zmienia.
bool WeatherUi::isNightNow(uint8_t blTarget) const {
  // Poziom nocny jest edytowalny z panelu (settings().blNight), wiec gate porownuje z NIM,
  // nie ze stala cfg::BL_NIGHT — inaczej podbicie jasnosci nocnej w panelu rozspoiloby
  // "ciemno w pokoju" z decyzja o zwinieciu ekranu do zegara.
  if (blTarget != settings().blNight) return false;
  const time_t now = time(nullptr);
  if (now < 1700000000) return false;
  struct tm tmv{};
  localtime_r(&now, &tmv);
  // Okno nocne z settings (domyslnie 22..6). start>end => okno przez polnoc (godz. >= start
  // LUB < end); start<end => okno w obrebie doby; start==end => okno zdegenerowane =>
  // traktujemy jako BRAK nocy (nie "cala doba"), zeby literowka w panelu nie zwinela ekranu
  // na zawsze.
  const int h = tmv.tm_hour;
  const uint8_t s = settings().nightStartH, e = settings().nightEndH;
  if (s == e) return false;
  if (s < e) return h >= s && h < e;
  return h >= s || h < e;
}

// ============================================================ DISPATCHERY ======

void WeatherUi::drawV3(TFT_eSPI& spr, uint8_t view, int ox, float t, const WeatherModel& w,
                       const PvModel& pv, const PvHistory& hist, const FlightModel& fl,
                       uint32_t nowMs, uint32_t heapNow) {
  (void)ox;   // V3 nie ma slajdu - ox zawsze 0
  (void)t;    // rysujemy wprost, bez animacji wejscia
  switch (view) {
    case cfg::VIEW_RADAR:
      v3Radar(spr, w, radarModel_);
      break;
    case cfg::VIEW_DAYS:
      v3Days(spr, w);
      break;
    case cfg::VIEW_PV:
      v3Pv(spr, pv, hist);
      break;
    case cfg::VIEW_HOME:
      v3Home(spr, roomModel_, rooms_, nowMs);
      break;
    case cfg::VIEW_BOILER:
      v3Boiler(spr, boiler_, burner_);
      break;
    case cfg::VIEW_AIR:
      v3Air(spr, air_);
      break;
    case cfg::VIEW_PAYBACK:
      v3Payback(spr, cost_);   // (v181) zwrot z instalacji PV: historia + prognoza
      break;
    case cfg::VIEW_AUTO:
      v3Auto(spr, auto_);
      break;
    case cfg::VIEW_FLIGHTS:
      v3Flights(spr, fl, nowMs);
      break;
    case cfg::VIEW_STATS:   // DIAGNOSTYKA 1 (zrodla - makieta 08)
      v3Diag1(spr, nowMs, air_);
      break;
    case cfg::VIEW_MEM:     // DIAGNOSTYKA 2 (stan - makieta 10)
      v3Diag2(spr, heapNow, cpuTempC_);
      break;
    case cfg::VIEW_MOTION:
      v3Motion(spr, nowMs);
      break;
    case cfg::VIEW_NOW:
    default:
      // (v162) `default:` JEST TU ZABEZPIECZENIEM, NIE OZDOBA — nie kasowac. Lapie
      // KAZDY numer widoku bez wlasnej galezi: wycofane sloty 0 (RETRO) i 2 (GODZINY),
      // ktore nadal wolno przypiac przez POST /api/view?i=N (patrz Config.h), oraz
      // wszystko, co wpadnie tu po przyszlej pomylce. Urzadzenie jest tylko-OTA, wiec
      // "nieznany numer" ma pokazac ekran GLOWNY, a nigdy czarna plansze.
      // Wariant nocny (makieta 02): ciemno + pora nocna => minimalny zegar zamiast
      // dwukolumnowego ukladu. To JEDYNA zmiana w tej galezi — dzien rysuje v3Main
      // jak dotad, pozostale ekrany rotacji bez zmian.
      if (isNightNow(blTarget_)) v3MainNight(spr, w);
      else v3Main(spr, w, pv, cost_);   // (v180) koszt zakupu z sieci — modul PRAD
      break;
  }

  // PASEK POSTEPU V3 "Pasmowy" (2 px na samej gorze, y=0..1). Segmenty poziome —
  // jeden na DOSTEPNY ekran PETLI (kV3Loop, z pominieciem viewSkipped). Pokazuje, na
  // ktorym z ilu ekranow jestesmy: OBEJRZANE ciemniejsze (col::RAIN), PRZED nami
  // jasniejsze (col::RAIN4), AKTUALNY podswietlony (col::ACCENT). Przy WLACZONEJ
  // auto-rotacji aktualny segment wypelnia sie od lewej (col::RAIN2) w miare uplywu
  // dwellS — widac, ile zostalo do przelaczenia. Na ekranach spoza petli (diagnostyka)
  // v3ProgressPos zwraca false i paska nie ma. Rysowany PO tresci (nad nia); pozycje
  // liczy WeatherUi.cpp (kV3Loop/viewSkipped — te same, co rotacja i dotyk).
  // Pelne kwalifikatory tv3:: — drawV3() to metoda w zasiegu globalnym (poza anonimowym
  // namespace tego pliku), wiec alias `col`/`grid` koliduje tu z globalnym `col` z
  // Colors.h (wciaganym przez WeatherIcons.h->Moon.h). Ten sam wzorzec, co kropka
  // feedbacku nizej (tv3::col::OK / tv3::grid::W).
  int curSeg = 0, totSeg = 0;
  if (v3ProgressPos(curSeg, totSeg)) {
    const int barW = tv3::grid::W;   // 320
    for (int i = 0; i < totSeg; ++i) {
      // Krawedzie liczone od pelnej szerokosci (nie stalym segW): zaokraglenie nie
      // zostawia szczerby po prawej — ostatni segment domyka do barW. 1 px przerwy.
      const int x0 = (i * barW) / totSeg;
      const int x1 = ((i + 1) * barW) / totSeg;
      const int segW = (x1 - x0) - 1;
      if (segW <= 0) continue;
      const uint16_t base = (i < curSeg) ? tv3::col::RAIN     // obejrzany (ciemny)
                          : (i > curSeg) ? tv3::col::RAIN4    // przed nami (jasny)
                                         : tv3::col::ACCENT;  // aktualny (podswietlony)
      // Wysokosc 3 px (bylo 2): przy 2 px pasek byl bardzo malo czytelny z 2 m
      // (wlasciciel). 3 px to wciaz cienka listwa, ale wyraznie widoczna.
      spr.fillRect(x0, 0, segW, 3, base);
      if (i == curSeg && settings().autoRotate) {
        const uint32_t dwellMs = static_cast<uint32_t>(settings().dwellS) * 1000UL;
        float frac = dwellMs ? static_cast<float>(nowMs - viewStart_) / dwellMs : 0.f;
        frac = clampf(frac, 0.f, 1.f);
        const int fillW = static_cast<int>(segW * frac);
        if (fillW > 0) spr.fillRect(x0, 0, fillW, 3, tv3::col::RAIN2);
      }
    }

    // LICZNIK POZYCJI "3 z 7" (v158). Pasek segmentowy wyzej pokazuje to samo
    // graficznie, ale z dwoch metrow nie da sie policzyc kresek — wlasciciel chcial
    // liczby. MIANOWNIK JEST LICZONY, NIE STALY: bierze sie z v3ProgressPos(), czyli
    // z tej samej pary kV3Loop + viewSkipped(), ktora decyduje o pomijaniu ekranow
    // (radar bez opadu, pokoje bez czujnikow BLE, piec bez autoryzacji, powietrze bez
    // danych). Gdyby stalo tu cfg::VIEW_COUNT (13), licznik klamalby przez wiekszosc
    // doby — petla V3 ma osiem ekranow, z ktorych realnie dostepne bywaja cztery.
    //
    // GEOMETRIA (policzona, nie przymierzona): font f10, napis "N z M" = 23 px dla
    // kazdej kombinacji cyfr (M <= 8, wiec zawsze jedna cyfra; w f10 '1'..'9' i 'z'
    // maja te same szerokosci w tym ukladzie). Lewy gorny rog: x = MARGIN = 7, wiec
    // napis zajmuje x=7..29. Baseline 11 => wiersze pikseli 4..10 (glify f10 maja
    // yOffset -7 i wysokosc 7). Nad nim pasek segmentow konczy sie na wierszu 2 —
    // jeden wiersz przerwy. Pod nim najwyzszy element ekranow petli to "RADAR"
    // (f11, baseline 20 => wiersze 12..19) i etykiety lightHeader (f11, baseline 22
    // => wiersze 14..21), wiec zostaje co najmniej 1 px odstepu. Prawy gorny rog
    // celowo ODRZUCONY: siedzi tam kropka feedbacku dotyku (srodek 311,9, r=4).
    //
    // KOLOR: lewa gora jest CIEMNA na ekranie glownym (kolumna kontekstu x=0..119),
    // w nocnym wariancie (pelna czern) i na radarze (mapa) — tam ONDARK_DIM; na
    // pozostalych ekranach petli tlo jest jasne — tam MUTE. Na ekranach spoza petli
    // (STATS/MEM/RUCH) v3ProgressPos zwraca false, wiec licznika po prostu nie ma
    // i ciemny naglowek diagnostyki nie ma z czym kolidowac.
    // `& 15` NIE jest zabezpieczeniem przed bledem — obie liczby sa z definicji
    // z zakresu 1..kV3LoopN (8). Jest po to, zeby KOMPILATOR to wiedzial: bez maski
    // gcc zaklada dla "%d" pelny zakres int (do 11 znakow na liczbe) i przy
    // -Wformat-truncation zglasza mozliwe obciecie bufora. Z maska widzi 0..15,
    // czyli najwyzej "15 z 15" = 7 znakow + NUL, co miesci sie w 12 B z zapasem.
    char pos[12];
    snprintf(pos, sizeof(pos), "%d z %d", (curSeg + 1) & 15, totSeg & 15);
    // (v162) Zdjete stad VIEW_RETRO i VIEW_HOURS — oba ekrany skasowane. Warunek nie
    // musi lapac wycofanych slotow 0/2: licznik "x z y" rysuje sie TYLKO wtedy, gdy
    // v3ProgressPos() zwrocil true, a ten szuka biezacego widoku w kV3Loop — w ktorej
    // ani 0, ani 2 nigdy nie bylo. Dla przypietego wycofanego numeru licznika po
    // prostu nie ma, wiec nie ma tez czego kolorowac.
    const bool darkTop = (view == cfg::VIEW_RADAR) || (view == cfg::VIEW_NOW);
    plex::str(spr, plex::f10(), pos, tv3::grid::MARGIN, 11,
              darkTop ? tv3::col::ONDARK_DIM : tv3::col::MUTE);
  }

  // KROPKA FEEDBACKU DOTYKU (spec 7a). Zapala sie, gdy elektroda czuje palec —
  // w prawym gornym rogu: "urzadzenie slyszy". (v158) Nie jest juz obejsciem
  // opoznionej reakcji (SINGLE leci teraz natychmiast, patrz Touch.cpp), tylko
  // potwierdzeniem kontaktu przy dluzszym przytrzymaniu. rawTouchMs_ ustawia
  // noteRawTouch(), wolane z petli gdy touch::pressedRaw(). Rysowana PO widoku, wiec
  // lezy na wierzchu kazdego ekranu (jasnego i ciemnego radaru). Jej stan jest
  // wliczony w sygnature V3 (render()), wiec render nie pominie ani zapalenia, ani
  // zgasniecia. Kolor OK (zielony) — czytelny i na jasnym tle, i na ciemnym radarze.
  if (rawTouchMs_ != 0 && nowMs - rawTouchMs_ < 600u) {
    spr.fillCircle(tv3::grid::W - 9, 9, 4, tv3::col::OK);
  }
}

void WeatherUi::drawV3Bottom(TFT_eSPI& tft, uint8_t view, const WeatherModel& w, const PvModel& pv,
                             const PvHistory& hist, const FlightModel& fl, uint32_t nowMs,
                             uint32_t heapNow) {
  // Podczas planszy zdarzenia caly dolny pas jest ciemny (spojnie z drawV3Alert, zeby
  // stopka nie przeswitywala), z cienkim paskiem akcentu na dole. Niezalezne od widoku.
  if (alertActive_) {
    tft.fillRect(0, 206, tv3::grid::W, tv3::grid::H - 206, tv3::col::PANEL);
    tft.fillRect(tv3::grid::DATA_L, 233, 120, 4, alert_.color);
    return;
  }
  switch (view) {
    case cfg::VIEW_RADAR:
      v3RadarBottom(tft, radarModel_);
      break;
    case cfg::VIEW_DAYS:
      v3DaysBottom(tft, w);
      break;
    case cfg::VIEW_PV:
      v3PvBottom(tft, pv, hist);
      break;
    case cfg::VIEW_HOME:
      v3HomeBottom(tft, roomModel_);   // legenda: kropki kolorow + biezace temperatury
      break;
    case cfg::VIEW_BOILER:
      v3BoilerBottom(tft, boiler_);
      break;
    case cfg::VIEW_AIR:
      v3AirBottom(tft, air_);
      break;
    case cfg::VIEW_PAYBACK:
      v3PaybackBottom(tft, cost_);   // (v181) tempo zl/mies. + czas do pelnego zwrotu
      break;
    case cfg::VIEW_AUTO:
      v3AutoBottom(tft, auto_);   // kafelek trybu + stan kabla + udzial slonca
      break;
    case cfg::VIEW_FLIGHTS:
      v3FlightsBottom(tft, fl);
      break;
    case cfg::VIEW_STATS:
      v3Diag1Bottom(tft, heapNow, cpuTempC_);
      break;
    case cfg::VIEW_MEM:
      v3Diag2Bottom(tft, nowMs);
      break;
    case cfg::VIEW_MOTION:
      v3MotionBottom(tft);
      break;
    case cfg::VIEW_NOW:
    default:
      // (v162) Ta sama rola co `default:` w drawV3() wyzej: dolny pas dla KAZDEGO
      // numeru bez wlasnej galezi, wliczajac wycofane sloty 0 i 2. Bez tego przypiety
      // stary numer zostawilby pas 206..239 z poprzedniej klatki (rysujemy wprost na
      // TFT, nie do bufora — nikt tego nie czysci).
      if (isNightNow(blTarget_)) {
        tft.fillRect(0, 206, tv3::grid::W, tv3::grid::H - 206, 0x0000);   // czern w nocy
      } else if (!w.ready && nowMs < 90000UL) {
        v3StartBottom(tft);   // pasek techniczny startu (makieta 07), do ~90 s pracy
      } else {
        v3MainBottom(tft, air_);   // POWIETRZE (takze makieta 21: pogoda niepobrana)
      }
      break;
  }
}

// ============================================================ PLANSZA ZDARZENIA =
// Makiety 13 (burza) / 18 (mroz) / 19 (awaria). Rysowana zamiast drawV3() gdy
// alertActive_ (patrz WeatherUi::paintFrame). Uklad: ciemne tlo na cala wysokosc
// (dolny pas 206..239 domalowuje drawV3Bottom), po lewej DUZY glif pogody na ciemnym
// tle (STORM/HEAVY_RAIN/FROST/HEAT wg alert_.iconCode) albo trojkat ostrzegawczy z "!"
// (PV_FAULT/PV_OFFLINE/WIND — iconCode < 0), po prawej tytul (alert_.title) i tekst
// (alert_.text) akcentem alert_.color. `t` = postep wejscia (jak V1 drawAlert: 260 ms).
void WeatherUi::drawV3Alert(TFT_eSPI& spr, float t) {
  namespace col = tv3::col;
  namespace grid = tv3::grid;
  const uint16_t accent = alert_.color;

  spr.fillRect(0, 0, grid::W, 206, col::PANEL);

  // Wejscie jak V1 (postep t): prawa kolumna wsuwa sie o kilkanascie pikseli.
  float e = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
  e = 1.f - (1.f - e) * (1.f - e);   // easeOutQuad
  const int slide = static_cast<int>((1.f - e) * 16.f);

  // --- glif / trojkat po lewej ---
  const int gcx = 62, gcy = 98;
  if (alert_.iconCode >= 0) {
    tv3::wx::glyph(spr, alert_.iconCode, false, gcx, gcy, 44, false);   // onLight=false: ciemne tlo
  } else {
    const int R = 46;
    spr.fillTriangle(gcx, gcy - R, gcx - R, gcy + (R * 3) / 4, gcx + R, gcy + (R * 3) / 4, accent);
    spr.fillRect(gcx - 3, gcy - 20, 6, 26, col::PANEL);   // slupek "!" (tlo przeswituje)
    spr.fillRect(gcx - 3, gcy + 12, 6, 6, col::PANEL);    // kropka "!"
  }

  // --- prawa kolumna: tytul + tekst ---
  const int tx = grid::DATA_L + slide;
  const int availW = grid::W - grid::DATA_L - 8;
  int by;   // biezaca linia bazowa pod tytulem

  // Tytul alertu. Od v129 font wyroznika f52 ma PELNY alfabet, wiec krotkie tytuly
  // ("Burza", "Mroz") ida wielkim krojem jak na makietach 13/18. Dluzsze ("Awaria
  // falownika") nie zmieszcza sie w f52 w kolumnie danych — wtedy zawijamy je do
  // dwoch linii f20 (makieta 19). Tytul bialy, podtytul akcentem.
  {
    if (plex::width(plex::f52(), alert_.title) <= availW) {
      plex::str(spr, plex::f52(), alert_.title, tx, 96, col::ONDARK);
      by = 126;
    } else {
      char l1[48], l2[48];
      wrap2(plex::f20(), alert_.title, availW, l1, l2);
      if (l2[0]) {
        plex::str(spr, plex::f20(), l1, tx, 70, col::ONDARK);
        plex::str(spr, plex::f20(), l2, tx, 94, col::ONDARK);
        by = 124;
      } else {
        plex::str(spr, plex::f20(), l1, tx, 78, col::ONDARK);
        by = 108;
      }
    }
  }

  // Tekst: buildAlert sklada go czesto jako "glowny - dodatkowy" (np. "Status 0x0300 -
  // sprawdz instalacje"). Czesc glowna akcentem (f13, zawijana), dodatkowa wyciszona
  // (f13). Bez separatora: calosc idzie w akcent.
  char head[48] = {}, tail[48] = {};
  const char* dash = strstr(alert_.text, " - ");
  if (dash) {
    const size_t k = static_cast<size_t>(dash - alert_.text);
    strncpy(head, alert_.text, k < sizeof(head) ? k : sizeof(head) - 1);
    snprintf(tail, sizeof(tail), "%s", dash + 3);
  } else {
    snprintf(head, sizeof(head), "%s", alert_.text);
  }

  if (head[0]) {
    char h1[48], h2[48];
    wrap2(plex::f13(), head, availW, h1, h2);
    plex::str(spr, plex::f13(), h1, tx, by, accent);
    by += 18;
    if (h2[0]) { plex::str(spr, plex::f13(), h2, tx, by, accent); by += 18; }
  }
  if (tail[0]) plex::str(spr, plex::f13(), tail, tx, by, col::MUTE);
}
