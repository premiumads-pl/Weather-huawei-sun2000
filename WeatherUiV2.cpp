// Warianty ekranow w wygladzie V2 ("SCENA") — projekt zamowiony u projektanta,
// przelaczany z panelu WWW (Settings::theme, patrz Portal.cpp). V1 zostaje
// nietkniety: kazda funkcja tutaj ma swoj odpowiednik drawView*() w WeatherUi.cpp
// i czyta DOKLADNIE te same pola modeli — rozni sie wylacznie rysowaniem.
//
// DLACZEGO OSOBNY PLIK: WeatherUi.cpp ma juz ponad 4 tys. linii, a to jest drugi,
// rownolegly komplet widokow. Trzymanie ich obok siebie zamienioloby ten plik w
// dwa projekty w jednym. Metody naleza do klasy WeatherUi (maja dostep do rooms_,
// boiler_, air_ itd.), ale definicje moga stac w dowolnej jednostce kompilacji.
//
// UKLAD KAZDEGO EKRANU V2 (geometria w Config.h):
//   0..27    hudTop      — rysowany WSPOLNIE w paintFrame(), nie tutaj
//   29..31   hudSegments — j.w.
//   36..51   titleRow    — rysuje KAZDY ekran sam (zna swoja nazwe i licznik)
//   54..205  tresc       — tlo (sceniczne albo plaskie) + karty
// Stopka PV (206..239) jest wspolna dla V1 i V2 i tez rysuje sie osobno.

#include <Arduino.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "AirClient.h"   // airIndexName()
#include "BleGateway.h"
#include "BleSensors.h"
#include "Colors.h"
#include "Config.h"
#include "Log.h"
#include "OtaGuard.h"    // resetWasCrash()
#include "RadarMap.h"
#include "RoomHistory.h"
#include "Settings.h"
#include "ThemeV2.h"
#include "WeatherIcons.h"
#include "WeatherUi.h"

namespace {

constexpr int CY2 = cfg::V2_CONTENT_Y;              // 54
constexpr int CB2 = cfg::V2_CONTENT_BOTTOM;         // 206
constexpr int SW = 320;                             // szerokosc ekranu

// Animacja wejscia: karty "wyrastaja" od dolu. Ta sama krzywa co easeOutCubic
// w V1 — kopia, bo tamta siedzi w anonimowej przestrzeni WeatherUi.cpp.
float ease(float t) {
  const float u = 1.f - (t < 0.f ? 0.f : (t > 1.f ? 1.f : t));
  return 1.f - u * u * u;
}

// Siatka kart: `n` kart w `cols` kolumnach, wypelniajaca obszar tresci.
// Zwraca prostokat karty `i`. Marginesy 8 px, odstepy 6 px — te same, co
// w mockupie projektanta (szesc kart 3x2 na ekranie W DOMU).
struct Grid {
  int cols, rows, x0, y0, cw, ch, gap;
  Grid(int n, int c, int top = CY2, int bottom = CB2) {
    cols = c < 1 ? 1 : c;
    rows = (n + cols - 1) / cols;
    if (rows < 1) rows = 1;
    gap = 6;
    x0 = 8;
    y0 = top;
    cw = (SW - 2 * x0 - (cols - 1) * gap) / cols;
    ch = (bottom - top - (rows - 1) * gap) / rows;
  }
  void at(int i, int& x, int& y) const {
    x = x0 + (i % cols) * (cw + gap);
    y = y0 + (i / cols) * (ch + gap);
  }
};

// Karta z animacja wejscia — rosnie od dolu, z lekkim opoznieniem na kolejnych
// (ten sam efekt, co kafelki w V1). Ponizej 6 px nie rysujemy nic: zaokraglony
// prostokat o wysokosci 1-2 px wyglada jak artefakt, nie jak animacja.
void cardIn(TFT_eSPI& spr, const Grid& g, int i, float e, const char* label,
            const char* value, const char* value2 = nullptr, bool warn = false,
            uint16_t vcol = themev2::col2::TEXT) {
  int x, y;
  g.at(i, x, y);
  const float local = e * 1.35f - i * 0.06f;
  const int h = static_cast<int>(g.ch * (local < 0.f ? 0.f : (local > 1.f ? 1.f : local)));
  if (h < 6) return;
  themev2::card(spr, x, y + (g.ch - h), g.cw, h, label, value, value2, warn, vcol);
}

// Skala barw temperatury i kolor klasy jakosci powietrza. Wlasne kopie — te z V1
// (tempColor/airIndexColor) siedza w anonimowej przestrzeni WeatherUi.cpp, wiec sa
// niewidoczne z innej jednostki kompilacji, a zadanie zabrania ruszac tamten plik
// w tych miejscach. PROGI SA TE SAME, zeby ten sam pomiar nie mial dwoch roznych
// barw zaleznie od wygladu.
uint16_t tempColorV2(float c) {
  if (c <= 0.f) return col::T_FREEZE;
  if (c <= 10.f) return col::T_COLD;
  if (c <= 20.f) return col::T_MILD;
  if (c <= 27.f) return col::T_WARM;
  return col::T_HOT;
}

uint16_t airIndexColorV2(int index) {
  switch (index) {
    case 1: return col::AIR_GOOD;
    case 2: return col::AIR_FAIR;
    case 3: return col::AIR_MODERATE;
    case 4: return col::AIR_POOR;
    case 5: return col::AIR_BAD;
    case 6: return col::AIR_SEVERE;
    default: return themev2::col2::DIM;
  }
}

// "X min temu" / "X h temu" ze znacznika millis(). Wspolne dla kilku ekranow V2.
void agoStr(char* buf, size_t n, uint32_t okAt, uint32_t nowMs) {
  if (okAt == 0) {
    snprintf(buf, n, "BRAK");
    return;
  }
  const uint32_t s = (nowMs - okAt) / 1000;
  if (s < 90) snprintf(buf, n, "%lus", static_cast<unsigned long>(s));
  else if (s < 5400) snprintf(buf, n, "%lu MIN", static_cast<unsigned long>(s / 60));
  else snprintf(buf, n, "%lu H", static_cast<unsigned long>(s / 3600));
}

// Sylwetka domu — dla ekranu W DOMU (mockup 3a projektanta). Wspolrzedne
// wzgledem lewej krawedzi ekranu; sceneBackground dodaje `ox` samo.
const themev2::SceneRect kHouse[] = {
    {60, 96, 200, 86},   // bryla
    {88, 78, 144, 20},   // pietro
    {120, 62, 80, 18},   // szczyt
    {236, 66, 14, 30},   // komin
};

}  // namespace

// ============================================================ TERAZ (V2) ====
void WeatherUi::drawViewNowV2(TFT_eSPI& spr, int ox, float t, const WeatherModel& w) {
  const float e = ease(t);
  const auto& c = w.current;

  themev2::sceneBackground(spr, ox, CY2, CB2, CB2 - 34);
  themev2::titleRow(spr, ox, "TERAZ", nullptr, c.isDay ? "DZIEN" : "NOC");

  // Wielka temperatura po lewej — glowna liczba ekranu, tak jak w V1 i w RETRO.
  char big[12];
  snprintf(big, sizeof(big), "%.0f", c.tempC);
  themev2::textShadowed(spr, ox + 12, CY2 + 10, 6, themev2::col2::TEXT, big);
  const int bw = themev2::textWidth(big, 6);
  themev2::textShadowed(spr, ox + 12 + bw + 4, CY2 + 16, 3, themev2::col2::LABEL, "*C");

  char feels[24];
  snprintf(feels, sizeof(feels), "ODCZUW %.0f", c.feelsC);
  themev2::textShadowed(spr, ox + 12, CY2 + 62, 2, themev2::col2::TEXT, feels);

  char desc[24];
  themev2::foldAscii(desc, sizeof(desc), wxico::labelForCode(c.weatherCode, c.isDay));
  themev2::textShadowed(spr, ox + 12, CY2 + 82, 2, themev2::col2::LABEL, desc);

  // Trzy karty po prawej: wiatr, wilgotnosc, cisnienie.
  const int cx = ox + 186, cw = 126, chh = 30, cgap = 5;
  char v[16];
  struct { const char* lbl; char val[16]; } rows[3];
  snprintf(rows[0].val, sizeof(rows[0].val), "%.0f KM/H", c.windKmh);
  rows[0].lbl = "WIATR";
  snprintf(rows[1].val, sizeof(rows[1].val), "%d%%", c.humidity);
  rows[1].lbl = "WILGOC";
  snprintf(rows[2].val, sizeof(rows[2].val), "%.0f", c.pressureHpa);
  rows[2].lbl = "HPA";
  for (int i = 0; i < 3; ++i) {
    const float local = e * 1.3f - i * 0.08f;
    const int h = static_cast<int>(chh * (local < 0.f ? 0.f : (local > 1.f ? 1.f : local)));
    if (h < 6) continue;
    const int y = CY2 + 8 + i * (chh + cgap);
    themev2::card(spr, cx, y + (chh - h), cw, h, rows[i].lbl, rows[i].val);
  }
  (void)v;
}

// ========================================================== GODZINY (V2) ====
void WeatherUi::drawViewHoursV2(TFT_eSPI& spr, int ox, float t, const WeatherModel& w) {
  const float e = ease(t);
  themev2::sceneBackground(spr, ox, CY2, CB2, CB2 - 26);
  themev2::titleRow(spr, ox, "GODZINY", "24 H", nullptr);

  // Slupki temperatury + pas opadu pod nimi. Projektant prosil, zeby opady
  // dostaly WIECEJ miejsca kosztem temperatury (uwaga z briefu) — stad podzial
  // 60/40 zamiast dominujacej krzywej temperatury jak w V1.
  int n = 0;
  float tmin = 1e9f, tmax = -1e9f;
  for (int i = 0; i < WX_HOURS && i < 24; ++i) {
    if (!w.hours[i].valid) continue;
    const float v = w.hours[i].data.tempC;
    if (v < tmin) tmin = v;
    if (v > tmax) tmax = v;
    ++n;
  }
  if (n == 0) {
    themev2::textShadowed(spr, ox + 12, CY2 + 40, 2, themev2::col2::DIM, "BRAK PROGNOZY");
    return;
  }
  if (tmax - tmin < 2.f) { const float m = (tmin + tmax) / 2.f; tmin = m - 1.f; tmax = m + 1.f; }

  const int gx0 = ox + 10, gw = SW - 20;
  const int tTop = CY2 + 6, tH = 62;          // temperatura
  const int pTop = CY2 + 76, pH = 44;         // opad — celowo szeroki pas
  const int bw = gw / (n > 0 ? n : 1);

  int k = 0;
  for (int i = 0; i < WX_HOURS && i < 24; ++i) {
    if (!w.hours[i].valid) continue;
    const auto& h = w.hours[i].data;
    const int x = gx0 + k * bw;
    const float fr = (h.tempC - tmin) / (tmax - tmin);
    const int bh = static_cast<int>(tH * fr * e);
    if (bh > 0) spr.fillRect(x, tTop + tH - bh, bw - 1, bh, tempColorV2(h.tempC));
    // opad: 0..100% na pelna wysokosc pasa
    const int ph = static_cast<int>(pH * (h.precipProb / 100.f) * e);
    if (ph > 0) spr.fillRect(x, pTop + pH - ph, bw - 1, ph, themev2::col2::VALUE2);
    ++k;
  }
  spr.drawFastHLine(gx0, pTop + pH, gw, themev2::col2::CARD_TOP);
  themev2::text(spr, ox + 10, pTop + pH + 4, 1, themev2::col2::DIM, "OPAD %");
  char lbl[16];
  snprintf(lbl, sizeof(lbl), "%.0f..%.0f*C", tmin, tmax);
  themev2::text(spr, ox + SW - 10 - themev2::textWidth(lbl, 1), pTop + pH + 4, 1, themev2::col2::DIM, lbl);
}

// ============================================================ 5 DNI (V2) ====
void WeatherUi::drawViewDaysV2(TFT_eSPI& spr, int ox, float t, const WeatherModel& w) {
  const float e = ease(t);
  themev2::sceneBackground(spr, ox, CY2, CB2, CB2 - 30);
  themev2::titleRow(spr, ox, "5 DNI", nullptr, nullptr);

  int n = 0;
  for (int i = 0; i < WX_DAYS && i < 5; ++i) if (w.days[i].valid) ++n;
  if (n == 0) {
    themev2::textShadowed(spr, ox + 12, CY2 + 40, 2, themev2::col2::DIM, "BRAK PROGNOZY");
    return;
  }
  const Grid g(n, n);
  static const char* kDow[7] = {"ND", "PN", "WT", "SR", "CZ", "PT", "SB"};
  const time_t now = time(nullptr);
  int idx = 0;
  for (int i = 0; i < WX_DAYS && i < 5; ++i) {
    if (!w.days[i].valid) continue;
    char lbl[8] = "---";
    if (now > 1700000000) {
      const time_t d = now + static_cast<time_t>(w.days[i].dayOffset) * 86400;
      struct tm tmv;
      localtime_r(&d, &tmv);
      snprintf(lbl, sizeof(lbl), "%s", kDow[tmv.tm_wday % 7]);
    }
    char hi[10], lo[12];
    snprintf(hi, sizeof(hi), "%.0f", w.days[i].tempMax);
    snprintf(lo, sizeof(lo), "MIN %.0f", w.days[i].tempMin);
    cardIn(spr, g, idx, e, lbl, hi, lo, false, tempColorV2(w.days[i].tempMax));
    ++idx;
  }
}

// ============================================================ RADAR (V2) ====
void WeatherUi::drawViewRadarV2(TFT_eSPI& spr, int ox, float t, const WeatherModel& w,
                                uint32_t nowMs) {
  (void)w;
  // Radar to obraz, nie karty — wiec V2 rysuje DOKLADNIE te sama mape i animacje
  // co V1 (to ten sam pomiar i ta sama geometria), zmienia sie tylko wiersz
  // tytulu i brak belki V1. Powielanie rysowania opadu byloby kopiowaniem
  // kilkuset linii bez zadnej roznicy wizualnej.
  themev2::titleRow(spr, ox, "RADAR", radarmap::hasRain() ? "OPAD" : "BEZ OPADU", nullptr);
  drawViewRadar(spr, ox, t, w, nowMs);
}

// =========================================================== W DOMU (V2) ====
void WeatherUi::drawViewHomeV2(TFT_eSPI& spr, int ox, float t, const WeatherModel& w) {
  (void)w;
  const float e = ease(t);
  themev2::sceneBackground(spr, ox, CY2, CB2, CB2 - 24, kHouse,
                           sizeof(kHouse) / sizeof(kHouse[0]));

  const int n = ble::count();
  char info[24];
  snprintf(info, sizeof(info), "* %d CZUJNIKOW", n);
  themev2::titleRow(spr, ox, "W DOMU", info, "BLE");

  if (n == 0) {
    themev2::textShadowed(spr, ox + 12, CY2 + 40, 2, themev2::col2::DIM, "BRAK CZUJNIKOW");
    return;
  }
  // Siatka 3x2 jak w mockupie; przy mniejszej liczbie czujnikow kolumny sie
  // dopasowuja, zeby karty nie zostawaly nienaturalnie waskie.
  const int cols = n >= 5 ? 3 : (n >= 3 ? 3 : n);
  const Grid g(n > 6 ? 6 : n, cols);
  // Nazwa pokoju NIE jest w czujniku — siedzi w ustawieniach, znajdowana po MAC
  // (ten sam wzorzec co drawViewHome w V1: bleFind + slot). Czujnik bez wpisu
  // w ustawieniach pomijamy, bo nie wiadomo, jak go podpisac.
  int drawn = 0;
  for (int i = 0; i < ble::count() && drawn < 6; ++i) {
    const ble::Sensor s = ble::get(i);
    if (!s.valid) continue;
    const Settings::BleCfg* cfg = settings().bleFind(s.mac);
    char lbl[14];
    themev2::foldAscii(lbl, sizeof(lbl), (cfg && cfg->name[0]) ? cfg->name : s.mac);
    char val[10] = "--";
    if (s.hasTemp) snprintf(val, sizeof(val), "%.1f", s.tempC);
    char sub[12];
    // Bateria ponizej 20% jest wazniejsza niz wilgotnosc — to ona mowi, ze czujnik
    // zaraz zamilknie. Stad podmiana wartosci dodatkowej i ostrzezenie.
    const bool lowBat = s.batteryPct > 0 && s.batteryPct < 20;
    if (lowBat) snprintf(sub, sizeof(sub), "BAT!");
    else if (s.hasHum) snprintf(sub, sizeof(sub), "%.0f%%", s.humidity);
    else snprintf(sub, sizeof(sub), "---");
    cardIn(spr, g, drawn, e, lbl, val, sub, lowBat,
           s.hasTemp ? tempColorV2(s.tempC) : themev2::col2::DIM);
    ++drawn;
  }
}

// ============================================================= PIEC (V2) ====
void WeatherUi::drawViewBoilerV2(TFT_eSPI& spr, int ox, float t) {
  const float e = ease(t);
  themev2::flatBackground(spr, ox, CY2, CB2);

  const vi::Model* b = boiler_;
  const bool on = b && b->burnerActive;
  themev2::titleRow(spr, ox, "PIEC", on ? "PALNIK PRACUJE" : "PALNIK STOI",
                    on ? "ON" : "OFF");
  if (!b || !b->valid) {
    themev2::textShadowed(spr, ox + 12, CY2 + 40, 2, themev2::col2::DIM, "BRAK DANYCH");
    return;
  }
  const Grid g(4, 2);
  char v1[12], v2[12], v3[12], v4[12];
  snprintf(v1, sizeof(v1), "%.1f", b->dhwTempC);
  snprintf(v2, sizeof(v2), "%.1f", b->supplyTempC);
  snprintf(v3, sizeof(v3), "%.2f", (b->gasDhwM3 + b->gasHeatM3));
  snprintf(v4, sizeof(v4), "%d%%", b->modulationPct);
  cardIn(spr, g, 0, e, "CWU", v1, "*C");
  cardIn(spr, g, 1, e, "ZASILANIE", v2, "*C");
  cardIn(spr, g, 2, e, "GAZ DZIS", v3, "M3");
  cardIn(spr, g, 3, e, "MODULACJA", v4, nullptr, false,
         on ? themev2::col2::WARN : themev2::col2::TEXT);
}

// ===================================================== FOTOWOLTAIKA (V2) ====
void WeatherUi::drawViewPvV2(TFT_eSPI& spr, int ox, float t, const PvModel& pv,
                             const PvHistory& hist) {
  const float e = ease(t);
  themev2::sceneBackground(spr, ox, CY2, CB2, CB2 - 28);
  themev2::titleRow(spr, ox, "FOTOWOLTAIKA", pv.asleep ? "FALOWNIK SPI" : nullptr,
                    pv.online ? "ON" : "OFF");
  if (!pv.online && !pv.asleep) {
    themev2::textShadowed(spr, ox + 12, CY2 + 40, 2, themev2::col2::DIM, "BRAK DANYCH");
    return;
  }
  const Grid g(4, 2, CY2, CB2 - 44);
  char a[12], b[12], c[12], d[12];
  snprintf(a, sizeof(a), "%d", pv.data.powerAcW);
  snprintf(b, sizeof(b), "%d", pv.data.houseLoadW);
  snprintf(c, sizeof(c), "%.1f", pv.data.energyTodayKwh);
  const int grid = pv.data.gridPowerW;
  snprintf(d, sizeof(d), "%d", grid < 0 ? -grid : grid);
  cardIn(spr, g, 0, e, "PRODUKCJA", a, "W", false, col::PV_SOLAR);
  cardIn(spr, g, 1, e, "DOM", b, "W", false, col::PV_HOUSE);
  cardIn(spr, g, 2, e, "DZIS", c, "KWH");
  cardIn(spr, g, 3, e, grid >= 0 ? "ODDAJE" : "POBOR", d, "W", grid < 0,
         grid >= 0 ? col::PV_EXPORT : col::PV_IMPORT);

  // Profil dnia — pasek u dolu, zeby bylo widac ksztalt produkcji.
  const int py = CB2 - 38, ph = 34, px0 = ox + 8, pw = SW - 16;
  spr.fillRect(px0, py, pw, ph, themev2::col2::CARD);
  spr.fillRect(px0, py, pw, 2, themev2::col2::CARD_TOP);
  const uint16_t peak = hist.peak();
  if (peak > 0) {
    for (int i = 0; i < PvHistory::SLOTS; ++i) {
      if (!hist.filled[i]) continue;
      const int x = px0 + (i * pw) / PvHistory::SLOTS;
      const int h = static_cast<int>((ph - 4) * (hist.watts[i] / static_cast<float>(peak)) * e);
      if (h > 0) spr.fillRect(x, py + ph - h, 1, h, col::PV_SOLAR);
    }
  }
}

// ========================================================= SAMOLOTY (V2) ====
void WeatherUi::drawViewFlightsV2(TFT_eSPI& spr, int ox, float t, const FlightModel& fl) {
  const float e = ease(t);
  themev2::sceneBackground(spr, ox, CY2, CB2, CB2 - 20);
  char info[20];
  snprintf(info, sizeof(info), "* %d W ZASIEGU", fl.total);
  themev2::titleRow(spr, ox, "SAMOLOTY", info, nullptr);
  if (fl.count == 0) {
    themev2::textShadowed(spr, ox + 12, CY2 + 40, 2, themev2::col2::DIM, "PUSTE NIEBO");
    return;
  }
  const int n = fl.count > 4 ? 4 : fl.count;
  const Grid g(n, 2);
  for (int i = 0; i < n; ++i) {
    const auto& f = fl.list[i];
    char lbl[12];
    themev2::foldAscii(lbl, sizeof(lbl), f.callsign);
    char alt[12];
    snprintf(alt, sizeof(alt), "%ld FT", static_cast<long>(f.altFt));
    char route[14] = "";
    if (f.routeKnown) themev2::foldAscii(route, sizeof(route), f.route);
    cardIn(spr, g, i, e, lbl, alt, route[0] ? route : nullptr);
  }
}

// ======================================================== POWIETRZE (V2) ====
void WeatherUi::drawViewAirV2(TFT_eSPI& spr, int ox, float t, const WeatherModel& w) {
  (void)w;
  const float e = ease(t);
  themev2::flatBackground(spr, ox, CY2, CB2);

  static const AirModel kEmpty{};
  const AirModel& a = air_ ? *air_ : kEmpty;
  if (!a.ready) {
    themev2::titleRow(spr, ox, "POWIETRZE", nullptr, nullptr);
    themev2::textShadowed(spr, ox + 12, CY2 + 40, 2, themev2::col2::DIM, "BRAK DANYCH");
    return;
  }
  char st[18];
  themev2::foldAscii(st, sizeof(st), a.stationName);
  themev2::titleRow(spr, ox, "POWIETRZE", st, a.usingFallback ? "ZAPAS" : nullptr);

  // Indeks slowny — najwazniejsza informacja tego ekranu, wiec duzy i centralnie.
  char idx[20];
  themev2::foldAscii(idx, sizeof(idx), airIndexName(a.index));
  const int iw = themev2::textWidth(idx, 3);
  themev2::textShadowed(spr, ox + (SW - iw) / 2, CY2 + 8, 3, airIndexColorV2(a.index), idx);

  const Grid g(2, 2, CY2 + 44, CB2 - 22);
  char p10[12] = "---", p25[12] = "---";
  if (a.hasPm10) snprintf(p10, sizeof(p10), "%.1f", a.pm10);
  if (a.hasPm25) snprintf(p25, sizeof(p25), "%.1f", a.pm25);
  cardIn(spr, g, 0, e, "PM10", p10, "UG/M3", false,
         a.hasPm10 ? airIndexColorV2(a.indexPm10) : themev2::col2::DIM);
  cardIn(spr, g, 1, e, "PM2.5", p25, "UG/M3", false,
         a.hasPm25 ? airIndexColorV2(a.indexPm25) : themev2::col2::DIM);

  // Wiek POMIARU (nie naszego pobrania) — wlasciciel prosil o to wprost.
  const time_t now = time(nullptr);
  char age[24] = "WIEK NIEZNANY";
  if (a.sampleEpoch > 0 && now > 1700000000 && now >= static_cast<time_t>(a.sampleEpoch)) {
    const uint32_t s = static_cast<uint32_t>(now - static_cast<time_t>(a.sampleEpoch));
    if (s < 5400) snprintf(age, sizeof(age), "POMIAR %lu MIN TEMU", static_cast<unsigned long>(s / 60));
    else snprintf(age, sizeof(age), "POMIAR %.1f H TEMU", s / 3600.f);
  }
  themev2::text(spr, ox + 10, CB2 - 16, 1, themev2::col2::DIM, age);
}

// =========================================================== PAMIEC (V2) ====
void WeatherUi::drawViewMemV2(TFT_eSPI& spr, int ox, float t, uint32_t heapNow) {
  const float e = ease(t);
  themev2::flatBackground(spr, ox, CY2, CB2);
  themev2::titleRow(spr, ox, "PAMIEC", "WSZYSTKIE RODZAJE", nullptr);

  const Diag& d = diag();
  const Grid g(6, 3);
  char a[14], b[14], c[14], dd[14], ee[14], ff[14];
  snprintf(a, sizeof(a), "%lu K", static_cast<unsigned long>(heapNow / 1024));
  snprintf(b, sizeof(b), "%lu K",
           static_cast<unsigned long>(ESP.getMaxAllocHeap() / 1024));
  snprintf(c, sizeof(c), "%.1f M", ESP.getFreePsram() / 1048576.f);
  snprintf(dd, sizeof(dd), "%.2f M", d.sketchBytes / 1048576.f);
  snprintf(ee, sizeof(ee), "%lu B", static_cast<unsigned long>(408));
  snprintf(ff, sizeof(ff), "%lu K", static_cast<unsigned long>(d.stackNet / 1024));
  cardIn(spr, g, 0, e, "SRAM", a, "WOLNE");
  cardIn(spr, g, 1, e, "BLOK", b, "CIAGLY");
  cardIn(spr, g, 2, e, "PSRAM", c, "WOLNE");
  cardIn(spr, g, 3, e, "FIRMWARE", dd, nullptr);
  cardIn(spr, g, 4, e, "RTC", ee, "Z 7680");
  cardIn(spr, g, 5, e, "STOS NET", ff, "ZAPAS");
}

// ============================================================= RUCH (V2) ====
void WeatherUi::drawViewMotionV2(TFT_eSPI& spr, int ox, float t, uint32_t nowMs) {
  const float e = ease(t);
  themev2::flatBackground(spr, ox, CY2, CB2);
  themev2::titleRow(spr, ox, "RUCH", "RYTM DOBY", "PIR");

  // Slupki godzinowe — 24 sztuki, aktualna godzina wyrozniona.
  uint32_t mx = 1;
  for (int i = 0; i < 24; ++i) if (gPir.byHour[i] > mx) mx = gPir.byHour[i];
  const int gx0 = ox + 10, gw = SW - 20, gy = CY2 + 6, gh = 66;
  const int bw = gw / 24;
  const time_t now = time(nullptr);
  int curH = -1;
  if (now > 1700000000) { struct tm tmv; localtime_r(&now, &tmv); curH = tmv.tm_hour; }
  for (int i = 0; i < 24; ++i) {
    const int h = static_cast<int>(gh * (gPir.byHour[i] / static_cast<float>(mx)) * e);
    const uint16_t col = (i == curH) ? themev2::col2::VALUE2 : themev2::col2::CARD_TOP;
    if (h > 0) spr.fillRect(gx0 + i * bw, gy + gh - h, bw - 1, h, col);
  }
  spr.drawFastHLine(gx0, gy + gh, gw, themev2::col2::CARD_TOP);
  themev2::text(spr, gx0, gy + gh + 4, 1, themev2::col2::DIM, "0");
  themev2::text(spr, gx0 + gw / 2 - 6, gy + gh + 4, 1, themev2::col2::DIM, "12");
  themev2::text(spr, gx0 + gw - 14, gy + gh + 4, 1, themev2::col2::DIM, "23");

  const Grid g(3, 3, CY2 + 90, CB2);
  char a[14], b[14], c[14];
  snprintf(a, sizeof(a), "%lu", static_cast<unsigned long>(gPir.rises));
  snprintf(b, sizeof(b), "%.1f H", gPir.collectedS / 3600.f);
  agoStr(c, sizeof(c), diag().pirLastAt, nowMs);
  cardIn(spr, g, 0, e, "WYZWOLEN", a, nullptr);
  cardIn(spr, g, 1, e, "ZBIERAM", b, nullptr);
  cardIn(spr, g, 2, e, "OSTATNI", c, nullptr);
}

// ======================================================= STATYSTYKI (V2) ====
void WeatherUi::drawViewStatsV2(TFT_eSPI& spr, int ox, float t, uint32_t nowMs,
                                uint32_t heapNow) {
  const float e = ease(t);
  themev2::flatBackground(spr, ox, CY2, CB2);
  const Diag& d = diag();
  themev2::titleRow(spr, ox, "STATYSTYKI", nullptr, resetWasCrash() ? "CRASH" : "OK");

  // Zrodla danych: kazde jako karta z wiekiem ostatniego udanego odczytu.
  const Grid g(6, 3);
  char v[6][14];
  agoStr(v[0], sizeof(v[0]), d.weatherOkAt, nowMs);
  agoStr(v[1], sizeof(v[1]), d.pvOkAt, nowMs);
  agoStr(v[2], sizeof(v[2]), d.viOkAt, nowMs);
  agoStr(v[3], sizeof(v[3]), d.airOkAt, nowMs);
  agoStr(v[4], sizeof(v[4]), d.mqttOkAt, nowMs);
  snprintf(v[5], sizeof(v[5]), "%.0f*C", cpuTempC_);
  static const char* kLbl[6] = {"POGODA", "FALOWNIK", "PIEC", "POWIETRZE", "MQTT", "CPU"};
  for (int i = 0; i < 6; ++i) {
    const bool bad = (i < 5) && (strcmp(v[i], "BRAK") == 0);
    cardIn(spr, g, i, e, kLbl[i], v[i], nullptr, bad,
           bad ? themev2::col2::WARN : themev2::col2::TEXT);
  }
  (void)heapNow;
}
