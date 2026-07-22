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
#include "PlexText.h"
#include "CoastMap.h"
#include "MapDataRadar.h"   // gmapr:: granice (pozycja Gdyni na radarze)
#include "AirData.h"
#include "AirClient.h"       // airIndexName()
#include "RoomData.h"
#include "RoomHistory.h"   // RoomHistory:: (sparkline trendu 24 h w tle wierszy POKOJE)
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

// "%.1f" z polskim przecinkiem dziesietnym (mockupy: "3,2 kW", "22,4 st").
void fmt1(char* b, size_t n, float v) {
  snprintf(b, n, "%.1f", v);
  for (char* p = b; *p; ++p)
    if (*p == '.') { *p = ','; break; }
}

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

// Miesiace (dopelniacz) i dni tygodnia - DISPLAY, wiec z polskimi znakami.
const char* kMonth[12] = {"stycznia", "lutego", "marca", "kwietnia", "maja", "czerwca",
                          "lipca", "sierpnia", "września", "października", "listopada", "grudnia"};
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

// Czy jest noc "do zwiniecia ekranu": ciemno w pokoju (blTarget na poziomie nocnym)
// ORAZ pora nocna wg zegara (okno edytowalne z panelu, domyslnie 22..6). Bez NTP nie
// zgadujemy pory — zwracamy false, wiec przy braku czasu zostaje zwykly, dwukolumnowy
// ekran glowny. Czysty odczyt (time()/blTarget przekazane z metody) — bezpieczny
// takze w watku zrzutu.
bool isNightNow(uint8_t blTarget) {
  // Poziom nocny jest edytowalny z panelu (settings().blNight), wiec gate porownuje z
  // NIM, nie ze stala cfg::BL_NIGHT — inaczej podbicie jasnosci nocnej w panelu
  // rozspoiloby "ciemno w pokoju" z decyzja o zwinieciu ekranu do zegara.
  if (blTarget != settings().blNight) return false;
  const time_t now = time(nullptr);
  if (now < 1700000000) return false;
  struct tm tmv{};
  localtime_r(&now, &tmv);
  // Okno nocne z settings (domyslnie 22..6). Trzy przypadki: start>end => okno przez
  // polnoc (godz. >= start LUB < end); start<end => okno w obrebie doby; start==end =>
  // okno zdegenerowane, traktujemy jako BRAK nocy (nie "cala doba"), zeby literowka w
  // panelu nie zwinela ekranu na zawsze.
  const int h = tmv.tm_hour;
  const uint8_t s = settings().nightStartH, e = settings().nightEndH;
  if (s == e) return false;
  if (s < e) return h >= s && h < e;
  return h >= s || h < e;
}

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
      {"GOT", "Göteborg"}, {"HAM", "Hamburg"},   {"DTM", "Dortmund"}, {"EIN", "Eindhoven"},
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
void precipChart(TFT_eSPI& s, const WeatherModel& w, int x, int y, int wdt, int hgt) {
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
      const uint16_t cc = h.precipMm >= 0.5f ? col::RAIN : (prob >= 50 ? col::RAIN2 : col::RAIN3);
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
void mainPvModule(TFT_eSPI& s, const WeatherModel& w, const PvModel& pv, int top) {
  const int lx = grid::DATA_L;
  plex::str(s, plex::f11(), "PRĄD", lx, top, col::SECOND);

  if (!pv.online && !pv.asleep) {
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

  if (pv.asleep) {
    wx::glyph(s, 0, true, lx + 12, top + 30, 12, true);   // ksiezyc
    plex::str(s, plex::f20(), "śpi - noc", lx + 32, top + 34, col::SECOND);
    char sub[40];
    if (w.sunrise[0]) snprintf(sub, sizeof(sub), "wróci po wschodzie (%s)", w.sunrise);
    else snprintf(sub, sizeof(sub), "falownik milczy po zachodzie");
    plex::str(s, plex::f13(), sub, lx, top + 58, col::MUTE);
    return;
  }

  // Falownik ONLINE.
  const int prod = pv.data.powerAcW;
  const int load = pv.data.houseLoadW;
  const int gridW = pv.data.gridPowerW;   // >0 oddajemy, <0 pobor
  const bool producing = prod >= load && prod > 120;

  char today[16];
  fmt1(today, sizeof(today), pv.data.energyTodayKwh);
  char todayL[24];
  snprintf(todayL, sizeof(todayL), "dziś %s kWh", today);
  plex::strRight(s, plex::f13(), todayL, grid::DATA_R, top, col::MUTE);

  char big[16];
  fmt1(big, sizeof(big), (producing ? prod : load) / 1000.f);
  const int bwv = plex::str(s, plex::f20(), big, lx, top + 34, col::PANEL);
  plex::str(s, plex::f13(), producing ? " kW produkcji" : " kW pobór domu",
            lx + bwv, top + 34, col::SECOND);

  // --- PASEK PRZEPLYWU ENERGII (dynamiczny, wszystkie przypadki) ----------------
  // Rozklad mocy na trzy skladniki:
  //   selfUse = min(prod, load)  — PV zjadana przez dom  (NIEBIESKI col::SELF)
  //   export  = max(0, prod-load) — nadwyzka PV do sieci  (ZIELONY  col::OK)
  //   import  = max(0, load-prod) — dom dobiera z sieci   (CZERWONY col::GRID)
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
    const int sw = static_cast<int>(barW * (selfUse / static_cast<float>(span)) + 0.5f);
    if (sw > 0) s.fillRect(barX, barY, sw, barH, col::SELF);
    if (expW > 0) {
      s.fillRect(barX + sw, barY, barW - sw, barH, col::OK);
    } else if (impW > 0) {
      s.fillRect(barX + sw, barY, barW - sw, barH, col::GRID);
    }
  }

  // --- TRZY WARTOSCI POD PASKIEM (kolory = segmenty; jednostka kW z wielkiej liczby
  // wyzej, wiec wartosci bez jednostki). produkcja (zielony) · z PV do domu (niebieski)
  // · sieC: pobor (czerwony) albo oddawanie (zielony). Font f11 (waski) — trzy zmieszcza
  // sie w kolumnie danych z zapasem.
  const bool exporting = expW > 0;
  char va[10], vb[10], vc[10], sa[16], sb[16], sc[18];
  fmtKw(va, sizeof(va), prod / 1000.f);
  fmtKw(vb, sizeof(vb), selfUse / 1000.f);
  fmtKw(vc, sizeof(vc), (exporting ? expW : impW) / 1000.f);
  snprintf(sa, sizeof(sa), "prod %s", va);
  snprintf(sb, sizeof(sb), "z PV %s", vb);
  snprintf(sc, sizeof(sc), "%s %s", exporting ? "→sieć" : "z sieci", vc);
  int vx = lx;
  vx += plex::str(s, plex::f11(), sa, vx, top + 66, col::OK) + 8;
  vx += plex::str(s, plex::f11(), sb, vx, top + 66, col::SELF) + 8;
  plex::str(s, plex::f11(), sc, vx, top + 66, exporting ? col::OK : col::GRID);
  (void)producing;
  (void)gridW;
}

void v3Main(TFT_eSPI& s, const WeatherModel& w, const PvModel& pv) {
  sceneBg(s);

  // --- kolumna kontekstu (ciemna) ---
  drawClock(s, grid::MARGIN_CTX + 2, 34, col::ONDARK);
  {
    const time_t now = time(nullptr);
    if (now >= 1700000000) {
      struct tm tmv{};
      localtime_r(&now, &tmv);
      char d[24];
      snprintf(d, sizeof(d), "%s %d %s", kDowLo[tmv.tm_wday % 7], tmv.tm_mday, kMonth[tmv.tm_mon % 12]);
      plex::str(s, plex::f13(), d, grid::MARGIN_CTX + 2, 52, col::ONDARK_DIM);
    }
  }

  const bool haveWx = w.ready && w.current.valid;
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
    plex::str(s, plex::f52(), big, grid::MARGIN_CTX, 156, col::ONDARK);

    char feels[24];
    snprintf(feels, sizeof(feels), "odczuwalna %.0f°", c.feelsC);
    plex::str(s, plex::f13(), feels, grid::MARGIN_CTX, 178, col::ONDARK_DIM);

    // Opis pogody - zawijany do dwoch linii, gdy nie miesci sie w kolumnie. Nawet dluzszy
    // ("Częściowe zachmurzenie") miesci sie: druga linia na y=208 (<240).
    const char* desc = wxico::labelForCode(c.weatherCode, !c.isDay);
    const int maxw = grid::CTX_W - 2 * grid::MARGIN_CTX;
    if (plex::width(plex::f13(), desc) <= maxw) {
      plex::str(s, plex::f13(), desc, grid::MARGIN_CTX, 196, col::ONDARK);
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
      plex::str(s, plex::f13(), l1, grid::MARGIN_CTX, 194, col::ONDARK);
      if (l2[0]) plex::str(s, plex::f13(), l2, grid::MARGIN_CTX, 208, col::ONDARK);
    }

    // GLIF NA WIERZCHU (po liczbie): chmura ~y60..108 (nad temperatura, ktorej gora
    // ~y107), a krople/gwiazdki spadaja z niej na sama gore liczby — zachodza delikatnie
    // i przezroczyscie, jak chcial wlasciciel.
    wx::glyph(s, c.weatherCode, !c.isDay, 62, 84, 24, false);
  } else {
    // Mockup 21: pogoda nie pobrana - placeholder zamiast glifu i temperatury.
    s.drawRect(grid::MARGIN_CTX, 66, 60, 46, col::ONDARK_DIM);
    s.drawFastHLine(grid::MARGIN_CTX + 8, 130, 40, col::ONDARK_DIM);
    s.fillCircle(grid::MARGIN_CTX + 48, 130, 4, col::ONDARK_DIM);
    plex::str(s, plex::f13(), "czekam na prognozę", grid::MARGIN_CTX, 176, col::ONDARK_DIM);
    plex::str(s, plex::f13(), "brak internetu", grid::MARGIN_CTX, 194, col::MUTE);
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
    else if (bestProb >= 20 && bestHour >= 0)
      snprintf(hint, sizeof(hint), "~%d:00 · %d%%", bestHour, bestProb);
    else snprintf(hint, sizeof(hint), "sucho");
    int rx = grid::DATA_R;
    freshDot(s, rx - 3, 22, w.ready ? Fresh::OK : Fresh::UNKNOWN);
    rx -= 12;
    plex::strRight(s, plex::f13(), hint, rx, 26, col::MUTE);
  }
  precipChart(s, w, grid::DATA_L, 40, grid::DATA_W - 2 * grid::MARGIN, 34);

  moduleSep(s, 92);

  // --- kolumna danych: PRAD (albo JUTRO) ---
  mainPvModule(s, w, pv, 112);

  moduleSep(s, 203);
}

void v3MainBottom(TFT_eSPI& tft, const AirModel* air) {
  sceneBottom(tft);
  // Trzeci modul: POWIETRZE (mockup 01/09/17/21).
  plex::str(tft, plex::f11(), "POWIETRZE", grid::DATA_L, 226, col::SECOND);
  if (air && air->ready) {
    const uint16_t bc = airCol(air->index);
    // Sama KLASA jakosci, bez "· liczba". Wlasciciel pytal, skad ta kropka i po co —
    // to byl separator przed wartoscia PM, ktora i tak jest na ekranie POWIETRZE.
    // Bez niej plakietka jest czysta i nie ma sierocego "·".
    const char* nm = airIndexName(air->index);
    const int tw = plex::width(plex::f11(), nm);
    const int bx = grid::DATA_R - tw - 14;
    // Plakietka wyrownana do linii "POWIETRZE": tekst na baseline 226 (jak etykieta),
    // pigulka wysrodkowana wzgledem niego — wczesniej tekst byl 2 px nizej.
    tft.fillRoundRect(bx, 215, tw + 14, 17, 5, bc);
    plex::str(tft, plex::f11(), nm, bx + 7, 226, col::BG);
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

  // Temperatura na zewnatrz (makieta: "12° na zewnatrz").
  if (w.ready && w.current.valid) {
    char tline[24];
    snprintf(tline, sizeof(tline), "%.0f° na zewnątrz", w.current.tempC);
    plex::strCenter(s, plex::f20(), tline, grid::W / 2, 152, col::BG);
  }

  // Spodziewany opad: najblizsza godzina z realnym prawdopodobienstwem/iloscia opadu.
  if (w.ready) {
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
  char b[64];
  if (WiFi.status() == WL_CONNECTED) {
    snprintf(b, sizeof(b), "sieć: %s · %s · %d dBm", WiFi.SSID().c_str(),
             WiFi.localIP().toString().c_str(), static_cast<int>(WiFi.RSSI()));
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
        freshDot(s, rx - 3, 16, w.radarAgeSec < 1200 ? Fresh::OK : Fresh::STALE);
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
    char age[28] = "pomiar";
    if (rm.frameEpoch > 0) {
      const time_t now = time(nullptr);
      if (now > 1700000000 && now >= static_cast<time_t>(rm.frameEpoch)) {
        char a[24];
        agoWords(a, sizeof(a), static_cast<uint32_t>(now - static_cast<time_t>(rm.frameEpoch)));
        snprintf(age, sizeof(age), "pomiar %s", a);
      }
    }
    int rx = grid::W - grid::MARGIN;
    freshDot(s, rx - 3, 16, rm.hasRain ? Fresh::OK : Fresh::STALE);
    rx -= 12;
    plex::strRight(s, plex::f13(), age, rx, 20, col::ONDARK_DIM);
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
  char hr[24] = "";
  if (w.updatedAt[0]) snprintf(hr, sizeof(hr), "prognoza z %.5s", w.updatedAt + 11);
  lightHeader(s, "5 DNI", w.ready ? hr : nullptr, w.ready ? Fresh::OK : Fresh::UNKNOWN);

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

  // Naglowek kolumn po prawej — wlasciciel pytal, co to 19° i 15°: to MAX i MIN
  // temperatura dnia. Opisujemy je (i kolumne opadu) drobnym naglowkiem.
  plex::strRight(s, plex::f10(), "max  min · opad", grid::DATA_R, 46, col::MUTE);

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

    wx::glyph(s, d.weatherCode, false, 78, y + 4, 9, true);

    // Pasek temperatury na wspolnej skali (plaski kolor wg tempMax). Zwezony ze 96 do 84,
    // zeby oddac szerokosc kolumnom liczb po prawej (temp + opad nie kolidowaly).
    const int barX = 108, barW = 84;
    const int xa = barX + static_cast<int>((d.tempMin - wkMin) / (wkMax - wkMin) * barW);
    const int xb = barX + static_cast<int>((d.tempMax - wkMin) / (wkMax - wkMin) * barW);
    s.fillRect(barX, y + 1, barW, 6, col::LINE);
    s.fillRoundRect(xa, y, (xb - xa > 4 ? xb - xa : 4), 8, 3, tempCol(d.tempMax));

    // UKLAD PRAWEJ STRONY (naprawa: "11 mm" nachodzilo na temperature). Dwie ROZLACZNE
    // kolumny wyrownane do prawej:
    //   [ ... temp max° min° ]  <odstep>  [ OPAD ]|DATA_R
    // Opad ma STALA kolumne przy prawej krawedzi — rezerwujemy szerokosc "88 mm", wiec
    // jej lewa granica (precipL) nie zalezy od tego, czy opad ma 1 czy 2 cyfry ani czy w
    // ogole jest. Temperatura wyrownana do prawej do precipL - odstep, rosnie w LEWO (ku
    // paskowi), NIGDY w kolumne opadu. Wszystkie granice liczone z plex::width na zywo.
    char pm[10];
    const bool hasP = d.precipMm >= 0.1f;
    if (hasP) snprintf(pm, sizeof(pm), "%.0f mm", d.precipMm);
    else      snprintf(pm, sizeof(pm), "-");
    plex::strRight(s, plex::f13(), pm, grid::DATA_R, y + 8, hasP ? col::RAIN : col::MUTE);
    const int precipL = grid::DATA_R - plex::width(plex::f13(), "88 mm");

    char hi[8], lo[8];
    snprintf(hi, sizeof(hi), "%.0f°", d.tempMax);
    snprintf(lo, sizeof(lo), "%.0f°", d.tempMin);
    const int loX = precipL - 12 - plex::width(plex::f13(), lo);
    plex::str(s, plex::f13(), lo, loX, y + 8, col::MUTE);
    plex::str(s, plex::f20(), hi, loX - 6 - plex::width(plex::f20(), hi), y + 8, col::PANEL);
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
  if (!any) return;
  char lo[8], hi[8];
  snprintf(lo, sizeof(lo), "%.0f°", wkMin);
  snprintf(hi, sizeof(hi), "%.0f°", wkMax);
  plex::str(tft, plex::f13(), lo, grid::MARGIN, 228, col::SECOND);
  plex::strRight(tft, plex::f13(), hi, grid::W - grid::MARGIN, 228, col::SECOND);
  plex::strCenter(tft, plex::f11(), "SKALA WSPÓLNA DLA TYGODNIA", grid::W / 2, 227, col::MUTE);
}

// ============================================================ PRAD =============
// Makieta 05. Pelnojasne tlo, wielka moc + wykres doby.

void v3Pv(TFT_eSPI& s, const PvModel& pv, const PvHistory& hist) {
  s.fillRect(0, 0, grid::W, 206, col::BG);
  char hr[20] = "";
  {
    const time_t now = time(nullptr);
    if (now > 1700000000) {
      struct tm tmv{};
      localtime_r(&now, &tmv);
      snprintf(hr, sizeof(hr), "odczyt %02d:%02d", tmv.tm_hour, tmv.tm_min);
    }
  }
  lightHeader(s, "PRĄD", pv.online ? hr : (pv.asleep ? "śpi - noc" : "offline"),
              pv.online ? Fresh::OK : (pv.asleep ? Fresh::UNKNOWN : Fresh::STALE));

  if (!pv.online && !pv.asleep) {
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

  // Wielka moc.
  const int prod = pv.data.powerAcW;
  const int load = pv.data.houseLoadW;
  const int gridW = pv.data.gridPowerW;
  char big[16];
  fmt1(big, sizeof(big), prod / 1000.f);
  const int bw = plex::str(s, plex::f52(), big, grid::MARGIN, 70, col::PANEL);
  plex::str(s, plex::f20(), "kW", grid::MARGIN + bw + 6, 66, col::SECOND);

  // Srodek: dom / siec.
  char l[24];
  fmt1(l, sizeof(l), load / 1000.f);
  char l2[24];
  snprintf(l2, sizeof(l2), "dom %s kW", l);
  plex::str(s, plex::f13(), l2, 150, 48, col::PANEL);
  char g[24];
  fmt1(g, sizeof(g), (gridW < 0 ? -gridW : gridW) / 1000.f);
  char g2[28];
  snprintf(g2, sizeof(g2), gridW >= 0 ? "do sieci +%s kW" : "z sieci −%s kW", g);
  plex::str(s, plex::f13(), g2, 150, 66, gridW >= 0 ? col::OK : col::GRID);

  // Prawa: energia.
  char e[16];
  fmt1(e, sizeof(e), pv.data.energyTodayKwh);
  char e2[24];
  snprintf(e2, sizeof(e2), "dziś %s kWh", e);
  plex::strRight(s, plex::f13(), e2, grid::DATA_R, 48, col::MUTE);
  char tot[24];
  snprintf(tot, sizeof(tot), "łącznie %.0f", pv.data.energyTotalKwh);
  // y=84, nie 66: przy duzym poborze "z sieci −11,5 kW" (x=150) siega w prawo tak
  // daleko, ze na wspolnym wierszu naslo by na "łącznie". Wlasny wiersz nizej —
  // wciaz nad wykresem (cy=90).
  plex::strRight(s, plex::f13(), tot, grid::DATA_R, 84, col::MUTE);

  // Wykres doby: WYPELNIONY (nie linia — na zywo linia byla nieczytelna). Sluply
  // POBORU domu (niebieski) jako tlo + PRODUKCJA (bursztyn) na wierzchu. Efekt:
  // wykres jest "pelny" o kazdej porze (dom zawsze cos ciagnie), a w dzien bursztyn
  // slonca narasta na niebieskim tle. Slupki maja pelna szerokosc slotu, bez przerw.
  const int cx = grid::MARGIN, cy = 90, cw = grid::W - 2 * grid::MARGIN, ch = 84;
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
        const int lh = static_cast<int>((ch - 2) * (hist.load[i] / static_cast<float>(peak)));
        if (lh > 0) s.fillRect(x0, base - lh, bw, lh, col::SELF);   // pobor domu (tlo)
        const int ph = static_cast<int>((ch - 2) * (hist.watts[i] / static_cast<float>(peak)));
        if (ph > 0) s.fillRect(x0, base - ph, bw, ph, col::PV);     // produkcja (na wierzchu)
      } else if (i > curSlot && curSlot >= 0) {
        s.fillRect(x0, base - 2, bw, 2, col::LINE);   // reszta doby - jeszcze przed nami
      }
    }
  } else {
    plex::strCenter(s, plex::f13(), "brak profilu doby", grid::W / 2, base - 30, col::MUTE);
  }
}

void v3PvBottom(TFT_eSPI& tft, const PvModel& pv) {
  (void)pv;
  tft.fillRect(0, 206, grid::W, 34, col::BG);
  // Osie godzin wykresu (wyrownane z v3Pv).
  const int cx = grid::MARGIN, cw = grid::W - 2 * grid::MARGIN;
  for (int hh = 0; hh <= 24; hh += 6) {
    char hb[4];
    snprintf(hb, sizeof(hb), "%d", hh);
    const int x = cx + (hh * cw) / 24;
    plex::strCenter(tft, plex::f10(), hb, x, 216, col::MUTE);
  }
  // Legenda.
  tft.fillRect(grid::MARGIN, 226, 10, 8, col::PV);
  plex::str(tft, plex::f13(), "produkcja", grid::MARGIN + 14, 233, col::SECOND);
  tft.fillRect(120, 226, 10, 8, col::SELF);   // próbka wypełniona, jak wykres (nie linia)
  plex::str(tft, plex::f13(), "pobór domu", 134, 233, col::SECOND);
  // "reszta doby" zamiast "reszta doby przed nami" — pelny napis naslo by na
  // "pobór domu" (trzy podpisy w jednym pasku f13 nie miesczą sie w 296 px).
  plex::strRight(tft, plex::f13(), "reszta doby", grid::W - grid::MARGIN, 233, col::MUTE);
}

// ============================================================ POKOJE ===========
// Makieta 14. Pelnojasne tlo, wiersze czujnikow.

// Sparkline trendu 24 h w TLE wiersza czujnika (makieta 14). Cienka, delikatna linia
// (col::RAIN4) laczaca probki temperatury z RoomHistory dla danego slotu pokoju,
// wpasowana w prostokat [x..x+w] x [yTop..yTop+h]. Rysowana PRZED tekstem, wiec nazwa/
// temperatura/kropka lezą na wierzchu. Skala pionowa jest per pokoj (min..max z jego
// wlasnej doby), zeby drobne wahania byly widoczne. Dziura w historii (NO_T, np. przerwa
// w zasilaniu) PRZERYWA linie — nie laczymy w poprzek luki. Gdy pokoj ma <2 probek,
// nie rysujemy nic (wiersz czysty).
void roomSparkline(TFT_eSPI& s, const RoomHistory& rh, int slot, int x, int yTop, int w, int h) {
  float mn = 1e9f, mx = -1e9f;
  int valid = 0;
  for (int i = 0; i < RoomHistory::SLOTS; ++i) {
    const int16_t v = rh.t10[slot][rh.idx(i)];
    if (v == RoomHistory::NO_T) continue;
    const float t = v / 10.f;
    if (t < mn) mn = t;
    if (t > mx) mx = t;
    ++valid;
  }
  if (valid < 2) return;
  const float range = mx - mn;
  int prevX = -1, prevY = -1;
  for (int i = 0; i < RoomHistory::SLOTS; ++i) {
    const int16_t v = rh.t10[slot][rh.idx(i)];
    if (v == RoomHistory::NO_T) { prevX = -1; continue; }   // dziura — przerwij linie
    // Temperatura prawie stala (range < 0,5) -> linia w polowie pasma, a nie przyklejona
    // do dolu (inaczej wygladalaby jak mylace podkreslenie, ktore mamy tu wlasnie usunac).
    const float norm = range < 0.5f ? 0.5f : (v / 10.f - mn) / range;
    const int px = x + (i * w) / (RoomHistory::SLOTS - 1);
    const int py = yTop + (h - 1) - static_cast<int>(norm * (h - 1) + 0.5f);
    if (prevX >= 0) s.drawLine(prevX, prevY, px, py, col::RAIN4);
    prevX = px;
    prevY = py;
  }
}

void v3Home(TFT_eSPI& s, const RoomModel* rmp, const RoomHistory* rhp, uint32_t nowMs) {
  (void)nowMs;
  s.fillRect(0, 0, grid::W, 206, col::BG);
  static const RoomModel kEmpty{};
  const RoomModel& rm = rmp ? *rmp : kEmpty;

  char hr[28];
  snprintf(hr, sizeof(hr), "%d czujników · %d z odczytem", rm.sensorCount, rm.count);
  lightHeader(s, "POKOJE", rm.sensorCount ? hr : nullptr);

  if (rm.count == 0) {
    plex::strCenter(s, plex::f20(), "Brak czujników", grid::W / 2, 110, col::MUTE);
    plex::strCenter(s, plex::f13(), "dodaj czujnik BLE w panelu", grid::W / 2, 134, col::MUTE);
    return;
  }

  const int rowY0 = 50, pitch = 31;
  int drawn = 0;
  for (int i = 0; i < rm.count && drawn < 5; ++i) {
    const RoomRow& r = rm.rows[i];
    const int y = rowY0 + drawn * pitch;
    const bool stale = r.ageS >= 900;

    // TLO wiersza: sparkline trendu 24 h (przed tekstem). Tylko czujniki ze slotem
    // historii (r.slot 0..ROOMS-1) i gdy warstwa danych podpiela RoomHistory. Kolumna
    // x=96..216: na prawo od najczestszych nazw, na lewo od wilgotnosci/temperatury i
    // kropki swiezosci — nie tyka ich ukladu.
    if (rhp && r.slot >= 0 && r.slot < RoomHistory::ROOMS)
      roomSparkline(s, *rhp, r.slot, 96, y - 6, 120, 26);

    // Nazwa (albo MAC) - pogrubiona.
    plex::str(s, plex::f20(), r.name ? r.name : "-", grid::MARGIN, y + 6,
              stale ? col::MUTE : col::PANEL);

    // Podtytul: stan / bateria / wilgotnosc.
    char sub[32] = "";
    if (r.slot < 0 && r.hasTemp) snprintf(sub, sizeof(sub), "bez nazwy - nadaj w panelu");
    else if (!r.hasTemp) snprintf(sub, sizeof(sub), "widzę czujnik - brak klucza");
    else if (stale) agoWords(sub, sizeof(sub), r.ageS);
    else if (r.batteryPct > 0 && r.batteryPct < 25)
      snprintf(sub, sizeof(sub), "bat. %d%%", r.batteryPct);
    if (sub[0]) plex::str(s, plex::f13(), sub, grid::MARGIN, y + 22, col::MUTE);

    // Wilgotnosc (srodek-prawo).
    if (r.hasHum) {
      char hm[8];
      snprintf(hm, sizeof(hm), "%.0f%%", r.humidity);
      plex::strRight(s, plex::f13(), hm, 250, y + 6, col::MUTE);
    }

    // Temperatura (wielka) + kropka swiezosci.
    if (r.hasTemp) {
      char tv[10];
      fmt1(tv, sizeof(tv), r.tempC);
      char tv2[12];
      snprintf(tv2, sizeof(tv2), "%s°", tv);
      plex::strRight(s, plex::f20(), tv2, grid::DATA_R, y + 8, stale ? col::MUTE : col::PANEL);
    } else {
      plex::strRight(s, plex::f20(), "-", grid::DATA_R, y + 8, col::MUTE);
    }
    freshDot(s, grid::W - 6, y + 2, !r.hasTemp ? Fresh::UNKNOWN : (stale ? Fresh::STALE : Fresh::OK));

    // Separatory drawFastHLine USUNIETE — wyglądały jak popsute podkreślenia i myliły
    // sie z trendem. Wiersze rozdziela teraz odstep (pitch) oraz sam sparkline w tle.
    ++drawn;
  }
}

// ============================================================ OGRZEWANIE =======
// Makieta 15. Pelnojasne tlo, CWU + wykres palnika.

void v3Boiler(TFT_eSPI& s, const vi::Model* bp, const BurnerHistory* bhp) {
  s.fillRect(0, 0, grid::W, 206, col::BG);

  if (!bp || !bp->valid) {
    lightHeader(s, "OGRZEWANIE", nullptr, Fresh::UNKNOWN);
    plex::strCenter(s, plex::f20(), "Piec nie odpowiada", grid::W / 2, 110, col::MUTE);
    plex::strCenter(s, plex::f13(), diag().viErr[0] ? diag().viErr : "skonfiguruj w panelu",
                    grid::W / 2, 134, col::MUTE);
    return;
  }
  const vi::Model& b = *bp;

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
    } else if (dl >= 0) {
      snprintf(badge, sizeof(badge), "dostęp %d dni", dl);
      plex::strRight(s, plex::f13(), badge, grid::W - grid::MARGIN, 22, col::MUTE);
    }
  }
  s.drawFastHLine(grid::MARGIN, 30, grid::W - 2 * grid::MARGIN, col::LINE);

  // Wielka CWU.
  char big[12];
  snprintf(big, sizeof(big), b.hasDhwTemp ? "%.0f°" : "-", b.dhwTempC);
  const int bw = plex::str(s, plex::f52(), big, grid::MARGIN, 74, col::PANEL);
  char sub[32];
  if (b.hasDhwTarget) snprintf(sub, sizeof(sub), "ciepła woda · zadane %.0f°", b.dhwTargetC);
  else snprintf(sub, sizeof(sub), "ciepła woda");
  plex::str(s, plex::f13(), sub, grid::MARGIN, 92, col::SECOND);
  (void)bw;

  // Prawa: zasilanie + tryb.
  if (b.hasSupplyTemp) {
    char sup[20];
    snprintf(sup, sizeof(sup), "zasilanie %.0f°", b.supplyTempC);
    plex::strRight(s, plex::f13(), sup, grid::DATA_R, 50, col::PANEL);
  }
  {
    const char* mode = strcmp(b.dhwMode, "comfort") == 0   ? "komfort"
                       : strcmp(b.dhwMode, "eco") == 0     ? "eko"
                       : strcmp(b.dhwMode, "off") == 0     ? "wyłączona"
                       : b.dhwMode[0]                      ? b.dhwMode
                                                           : "-";
    char tr[24];
    snprintf(tr, sizeof(tr), "tryb: %s", mode);
    plex::strRight(s, plex::f13(), tr, grid::DATA_R, 68, col::SECOND);
  }

  // Wykres palnika (modulacja doby).
  plex::str(s, plex::f11(), "PALNIK DZIŚ", grid::MARGIN, 120, col::SECOND);
  {
    const char* st = !b.hasBurnerState ? "brak odczytu" : (b.burnerActive ? "teraz: włączony" : "teraz: wyłączony");
    plex::strRight(s, plex::f13(), st, grid::DATA_R, 120, b.burnerActive ? col::PV : col::MUTE);
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
    char g[48];
    snprintf(g, sizeof(g), "%.1f m³ · woda %.1f / grzanie %.1f",
             bp->gasDhwM3 + bp->gasHeatM3, bp->gasDhwM3, bp->gasHeatM3);
    for (char* p = g; *p; ++p)
      if (*p == '.') *p = ',';
    plex::strRight(tft, plex::f13(), g, grid::W - grid::MARGIN, 228, col::PANEL);
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
    const bool fresh = now > 1700000000 && a.sampleEpoch > 0 &&
                       (now - static_cast<time_t>(a.sampleEpoch)) < static_cast<time_t>(AIR_STALE_S);
    int rx = grid::W - grid::MARGIN;
    freshDot(s, rx - 3, 18, fresh ? Fresh::OK : Fresh::STALE);
    rx -= 12;
    plex::strRight(s, plex::f13(), smp, rx, 22, col::MUTE);
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
    fresh = ageS < 60 ? Fresh::OK : Fresh::STALE;
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

  struct Src {
    const char* name;
    uint32_t okAt;
    bool off;
    const char* note;
  };
  const Src src[6] = {
      {"pogoda", d.weatherOkAt, false, ""},
      {airName, d.airOkAt, false, ""},
      {"radar", d.radarOkAt, false, ""},
      {"falownik", d.pvOkAt, d.pvAsleep, "śpi - noc"},
      {"piec", d.viOkAt, !settings().hasViessmann(), "wyłączony"},
      {"samoloty", d.flightOkAt, false, "nieodpytywane"},
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
      char hm[12] = "OK";
      if (now > 1700000000) {
        const time_t okEpoch = now - static_cast<time_t>((nowMs - src[i].okAt) / 1000);
        struct tm tmv{};
        localtime_r(&okEpoch, &tmv);
        snprintf(hm, sizeof(hm), "%02d:%02d ✓", tmv.tm_hour, tmv.tm_min);
      }
      plex::strRight(s, plex::f13(), hm, grid::W - grid::MARGIN, y, col::OK);
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

  // Siec.
  plex::str(s, plex::f13(), "sieć", lx, y, col::PANEL);
  {
    char v[48];
    snprintf(v, sizeof(v), "%s · %s · %d dBm", WiFi.SSID().c_str(),
             WiFi.localIP().toString().c_str(), static_cast<int>(WiFi.RSSI()));
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
  plex::strRight(tft, plex::f13(), "stuknij 2× - wyjście", grid::W - grid::MARGIN, 227, col::ONDARK_DIM);
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
    case cfg::VIEW_RETRO:   // w V3 pomijane w rotacji - defensywnie rysuj GLOWNY
    case cfg::VIEW_HOURS:
    default:
      // Wariant nocny (makieta 02): ciemno + pora nocna => minimalny zegar zamiast
      // dwukolumnowego ukladu. To JEDYNA zmiana w tej galezi — dzien rysuje v3Main
      // jak dotad, pozostale ekrany rotacji bez zmian.
      if (isNightNow(blTarget_)) v3MainNight(spr, w);
      else v3Main(spr, w, pv);
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
  }

  // KROPKA FEEDBACKU DOTYKU (spec 7a). Natychmiast po surowym dotyku elektrody —
  // jeszcze przed rozroznieniem 1x/2x (okno ~550 ms w touch::poll) — w prawym gornym
  // rogu zapala sie mala kropka: "urzadzenie slyszy". rawTouchMs_ ustawia
  // noteRawTouch(), wolane z petli gdy touch::pressedRaw(). Rysowana PO widoku, wiec
  // lezy na wierzchu kazdego ekranu (jasnego i ciemnego radaru). Jej stan jest
  // wliczony w sygnature V3 (render()), wiec render nie pominie ani zapalenia, ani
  // zgasniecia. Kolor OK (zielony) — czytelny i na jasnym tle, i na ciemnym radarze.
  if (rawTouchMs_ != 0 && nowMs - rawTouchMs_ < 600u) {
    spr.fillCircle(tv3::grid::W - 9, 9, 4, tv3::col::OK);
  }
}

void WeatherUi::drawV3Bottom(TFT_eSPI& tft, uint8_t view, const WeatherModel& w, const PvModel& pv,
                             const FlightModel& fl, uint32_t nowMs, uint32_t heapNow) {
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
      v3PvBottom(tft, pv);
      break;
    case cfg::VIEW_HOME:
      tft.fillRect(0, 206, tv3::grid::W, 34, tv3::col::BG);
      break;
    case cfg::VIEW_BOILER:
      v3BoilerBottom(tft, boiler_);
      break;
    case cfg::VIEW_AIR:
      v3AirBottom(tft, air_);
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
    case cfg::VIEW_RETRO:
    case cfg::VIEW_HOURS:
    default:
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
