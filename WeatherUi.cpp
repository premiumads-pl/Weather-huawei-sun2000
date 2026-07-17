#include "WeatherUi.h"
#include "RoomHistory.h"

#include <Arduino.h>
#include <cmath>
#include <cstdio>
#include <ctime>

#include "BleGateway.h"
#include "Colors.h"
#include "Config.h"
#include "Moon.h"
#include "GasMeter.h"
#include "Viessmann.h"
#include "MapDataWide.h"
#include "RadarMap.h"
#include "PlText.h"
#include "BleSensors.h"
#include "Log.h"
#include "OtaGuard.h"
#include "RadarClient.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include "Settings.h"
#include "Version.h"
#include "WeatherIcons.h"

// ---------------------------------------------------------------- pomocnicze --

namespace {

constexpr int W = cfg::SCREEN_W;
constexpr int CY = cfg::CONTENT_Y;                  // 34
constexpr int CH = cfg::CONTENT_H;                  // 172
// CB usuniete — bylo martwe (zero uzyc) i bylo TRZECIM bytem opisujacym liczbe 206,
// obok VIEW_H (jedyny zywy) i skasowanego juz cfg::FOOTER_Y, ktory twierdzil 208.

float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

float easeOutCubic(float t) {
  const float u = 1.f - clampf(t, 0.f, 1.f);
  return 1.f - u * u * u;
}

uint16_t lerp565(uint16_t a, uint16_t b, float t) {
  t = clampf(t, 0.f, 1.f);
  const int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  const int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  const int r = ar + static_cast<int>((br - ar) * t);
  const int g = ag + static_cast<int>((bg - ag) * t);
  const int bl = ab + static_cast<int>((bb - ab) * t);
  return static_cast<uint16_t>((r << 11) | (g << 5) | bl);
}

// Kolor wg temperatury bezwzględnej — stała skala, żeby oko przywykło.
uint16_t tempColor(float c) {
  if (c <= -5.f) return col::T_FREEZE;
  if (c < 4.f) return lerp565(col::T_FREEZE, col::T_COLD, (c + 5.f) / 9.f);
  if (c < 14.f) return lerp565(col::T_COLD, col::T_MILD, (c - 4.f) / 10.f);
  if (c < 24.f) return lerp565(col::T_MILD, col::T_WARM, (c - 14.f) / 10.f);
  if (c < 32.f) return lerp565(col::T_WARM, col::T_HOT, (c - 24.f) / 8.f);
  return col::T_HOT;
}

const char* windDirName(int deg) {
  static const char* kDirs[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  int i = static_cast<int>((deg + 22) / 45) % 8;
  if (i < 0) i = 0;
  return kDirs[i];
}

void fmtTemp(char* buf, size_t n, float v) {
  snprintf(buf, n, "%.1f", v);
}

void fmtTempInt(char* buf, size_t n, float v) {
  snprintf(buf, n, "%d°", static_cast<int>(lroundf(v)));
}

// moc -> tekst: <1000 W => "850", >=1000 => "1.68"
void fmtPower(char* buf, size_t n, char* unit, size_t un, int32_t w) {
  const int32_t a = w < 0 ? -w : w;
  if (a >= 1000) {
    snprintf(buf, n, "%.2f", w / 1000.f);
    snprintf(unit, un, "kW");
  } else {
    snprintf(buf, n, "%ld", static_cast<long>(w));
    snprintf(unit, un, "W");
  }
}

}  // namespace

// ---- cienkie wrappery na tekst ------------------------------------------------

#define PLF14 pltxt::font14()
#define PLF18 pltxt::font18()

void plStr(TFT_eSPI& s, const pltxt::FontSet& f, const char* t, int x, int baseline,
           uint16_t c) {
  pltxt::drawString(s, f, t, x, baseline, c, c);  // bg == fg => przezroczysto
}

void plCenter(TFT_eSPI& s, const pltxt::FontSet& f, const char* t, int cx, int baseline,
              uint16_t c) {
  plStr(s, f, t, cx - pltxt::stringWidth(f, t) / 2, baseline, c);
}

void plRight(TFT_eSPI& s, const pltxt::FontSet& f, const char* t, int right, int baseline,
             uint16_t c) {
  plStr(s, f, t, right - pltxt::stringWidth(f, t), baseline, c);
}

// Male podpisy. Do v81 szly przez wbudowany GLCD (font 1) — a ten nie ma polskich
// znakow ani stopnia, wiec w kolko wracalo "CIEP A WODA", "52.4[]C", "m-|".
// Teraz to PlFont10 z pelnym zestawem. GLCD zniknal z projektu i blad razem z nim.
// Kotwica zostaje u GORY (jak TL_DATUM), zeby nie przestawiac 58 miejsc w ukladzie.
void gl(TFT_eSPI& s, const char* t, int x, int y, uint16_t c) {
  pltxt::drawString(s, pltxt::font10(), t, x, y + PlFont10Ascent, c, c);
}

void glCenter(TFT_eSPI& s, const char* t, int cx, int y, uint16_t c) {
  const int w = pltxt::stringWidth(pltxt::font10(), t);
  pltxt::drawString(s, pltxt::font10(), t, cx - w / 2, y + PlFont10Ascent, c, c);
}

void glRight(TFT_eSPI& s, const char* t, int right, int y, uint16_t c) {
  const int w = pltxt::stringWidth(pltxt::font10(), t);
  pltxt::drawString(s, pltxt::font10(), t, right - w, y + PlFont10Ascent, c, c);
}

int bigStr(TFT_eSPI& s, const GFXfont* f, const char* t, int x, int baseline, uint16_t c) {
  s.setFreeFont(f);
  s.setTextDatum(L_BASELINE);
  s.setTextColor(c);
  s.drawString(t, x, baseline);
  const int w = s.textWidth(t);
  s.setTextFont(1);
  return w;
}

// Łuk ze zaokrąglonymi końcami — WŁASNY, bo TFT_eSPI::drawSmoothArc jest niebezpieczne
// przy przesuniętym viewporcie. Jego końcówki rysuje drawWedgeLine, która MIESZA UKŁADY
// WSPÓŁRZĘDNYCH: bounding box przycina razem z datumem (współrzędne fizyczne bufora),
// ale pętle skanujące jadą po współrzędnych użytkownika i zapisują przez
// setWindow()/pushColor(). W dolnym pasie (datum -103) setWindow przepuszcza y=113,
// bo height() zwraca wtedy 206, a bufor ma tylko 103 wiersze — zapis leci ~7 kB
// ZA koniec bufora i rozwala stertę. (Sprawdzone w TFT_eSPI 2.5.43.)
//
// Składamy więc łuk z prymitywów, które viewport honorują poprawnie:
//   drawArc          -> drawPixel / drawFastHLine / drawFastVLine (wirtualne),
//   fillSmoothCircle -> drawFastHLine + drawPixel z alfą (też wirtualne).
// Efekt wizualny ten sam.
void smoothArc(TFT_eSPI& s, int cx, int cy, int r, int ir, int a0, int a1, uint16_t fg,
               uint16_t bg) {
  if (a1 <= a0 || r <= ir) {
    return;
  }
  const float mid = (r + ir) * 0.5f;
  const int cap = (r - ir) / 2;
  for (int i = 0; i < 2; ++i) {
    const float a = static_cast<float>(i == 0 ? a0 : a1) * 0.01745329f;
    const int ex = cx + static_cast<int>(lroundf(-sinf(a) * mid));
    const int ey = cy + static_cast<int>(lroundf(cosf(a) * mid));
    s.fillSmoothCircle(ex, ey, cap, fg, bg);
  }
  s.drawArc(cx, cy, r, ir, static_cast<uint32_t>(a0), static_cast<uint32_t>(a1), fg, bg, true);
}

// ------------------------------------------------------------------- pasy -----

// Sprite fizycznie ma tylko `bandH` wierszy, ale kod rysujący nie chce o tym wiedzieć.
// setViewport z vpDatum = true przesuwa układ współrzędnych o -top i przycina rysowanie
// do rzeczywistego bufora: rysunek w globalnym y=150 trafia w pasie dolnym do wiersza
// 150-103 = 47, a w pasie górnym jest po prostu wycinany.
//
// Sprawdzone w źródłach TFT_eSPI 2.5.43: drawPixel / drawChar / drawLine / drawFastVLine /
// drawFastHLine / fillRect / readPixel są WIRTUALNE, a TFT_eSprite nadpisuje je wersjami,
// które honorują _xDatum/_yDatum i przycinają do viewportu. Pozostałe prymitywy, których
// używamy (fillCircle, fillRoundRect, fillTriangle, drawCircle, drawRect, drawArc,
// fillSmoothCircle, drawString...), są zbudowane wyłącznie na tych wirtualnych, więc
// dziedziczą przesunięcie za darmo.
// JEDYNY wyjątek to drawSmoothArc — patrz smoothArc() wyżej. Jeśli będziesz dokładać
// nowe prymitywy TFT_eSPI, sprawdź najpierw, czy nie piszą przez setWindow()/pushColor().
//
// virtH = wysokość układu współrzędnych (206 dla ekranu, 240 dla zrzutu ze stopką);
// width()/height() sprite'a zwracają wtedy wymiary WIRTUALNE, czego wymagają
// wxico::draw (obcinanie ikon) i TFT_eSPI::drawString.
void WeatherUi::setBand(TFT_eSprite& s, int top, int virtH) {
  s.setViewport(0, -top, W, virtH, true);
}

// UWAGA: przy aktywnym viewporcie NIE WOLNO wołać fillSprite() — jego szybka ścieżka
// robi memset(_img, ..., _iwidth * _yHeight * 2), czyli w naszym przypadku 132 kB
// do bufora 66 kB (rozwaliłoby stertę). Zamiast tego wszędzie fillRect(0, 0, W, VIEW_H).

template <typename F>
void WeatherUi::pushBands(F&& paint) {
  for (int b = 0; b < BAND_N; ++b) {
    const int top = b * BAND_H;
    setBand(spr_, top, VIEW_H);
    paint(static_cast<TFT_eSPI&>(spr_));
    spr_.pushSprite(0, top);
  }
  spr_.resetViewport();
}

// ---------------------------------------------------------------------- init --

bool WeatherUi::begin() {
  pinMode(cfg::PIN_TFT_BL, OUTPUT);
  ledcAttach(cfg::PIN_TFT_BL, cfg::BL_PWM_FREQ, cfg::BL_PWM_BITS);
  ledcWrite(cfg::PIN_TFT_BL, 0);

  tft_.init();
  tft_.setRotation(cfg::TFT_ROTATION);
  tft_.invertDisplay(cfg::TFT_INVERT_DISPLAY);
  tft_.fillScreen(col::BG);

  spr_.setColorDepth(16);
  if (spr_.createSprite(cfg::SCREEN_W, BAND_H) == nullptr) {
    return false;
  }
  spr_.setSwapBytes(false);
  spr_.fillRect(0, 0, W, BAND_H, col::BG);   // viewport swiezy = wspolrzedne pasa
  spr_.pushSprite(0, 0);
  spr_.pushSprite(0, BAND_H);
  tft_.fillRect(0, VIEW_H, cfg::SCREEN_W, cfg::SCREEN_H - VIEW_H, col::BG);

  blCurrent_ = 0;
  blTarget_ = cfg::BL_DAY;
  ready_ = true;
  return true;
}

void WeatherUi::tickBacklight() {
  if (blCurrent_ == blTarget_) {
    return;
  }
  const int step = 6;
  int v = blCurrent_;
  if (v < blTarget_) {
    v = (v + step > blTarget_) ? blTarget_ : v + step;
  } else {
    v = (v - step < blTarget_) ? blTarget_ : v - step;
  }
  blCurrent_ = static_cast<uint8_t>(v);
  ledcWrite(cfg::PIN_TFT_BL, blCurrent_);
}

// --------------------------------------------------------------- ekrany bazowe --

void WeatherUi::drawBoot(const char* status, int attempt) {
  if (!ready_) return;
  // Faza animacji liczona RAZ — inaczej oba pasy mogłyby wypaść z innej klatki.
  const int bx = 70, bw = 180, by = 182;
  const uint32_t ph = (millis() / 12) % (bw + 60);

  pushBands([&](TFT_eSPI& spr) {
    spr.fillRect(0, 0, W, VIEW_H, col::BG);

    // delikatna poświata u góry
    for (int y = 0; y < 60; ++y) {
      const uint16_t c = lerp565(col::HEADER, col::BG, y / 60.f);
      spr.drawFastHLine(0, y, W, c);
    }

    wxico::draw(spr, 0, W / 2, 92, 64);
    plCenter(spr, PLF18, settings().city, W / 2, 148, col::TEXT);
    plCenter(spr, PLF14, status, W / 2, 172, col::TEXT_DIM);

    // pasek postępu — animowany "knight rider"
    spr.fillRoundRect(bx, by, bw, 6, 3, col::PV_TRACK);
    const int sx = bx + static_cast<int>(ph) - 60;
    for (int i = 0; i < 60; ++i) {
      const int x = sx + i;
      if (x < bx || x >= bx + bw) continue;
      spr.drawFastVLine(x, by, 6, lerp565(col::PV_TRACK, col::ACCENT, i / 59.f));
    }

    if (attempt > 1) {
      char b[32];
      snprintf(b, sizeof(b), "próba %d", attempt);
      plCenter(spr, PLF14, b, W / 2, 202, col::TEXT_MUTE);
    }
  });

  tft_.fillRect(0, VIEW_H, W, cfg::SCREEN_H - VIEW_H, col::BG);
  if (blTarget_ == 0) blTarget_ = cfg::BL_DAY;
  tickBacklight();
}

void WeatherUi::drawFatal(const char* msg) {
  if (!ready_) return;
  pushBands([&](TFT_eSPI& spr) {
    spr.fillRect(0, 0, W, VIEW_H, col::BG);
    spr.fillRoundRect(20, 78, W - 40, 86, 8, col::ALERT_BG);
    spr.drawRoundRect(20, 78, W - 40, 86, 8, col::ERR);
    plCenter(spr, PLF18, "Błąd", W / 2, 112, col::ERR);
    plCenter(spr, PLF14, msg, W / 2, 142, col::TEXT);
  });
}

void WeatherUi::drawColorTest() {
  if (!ready_) return;
  pushBands([&](TFT_eSPI& spr) {
    spr.fillRect(0, 0, W, 68, TFT_RED);
    spr.fillRect(0, 68, W, 68, TFT_GREEN);
    spr.fillRect(0, 136, W, 70, TFT_BLUE);
    plStr(spr, PLF18, "CZERWONY", 12, 42, TFT_WHITE);
    plStr(spr, PLF18, "ZIELONY", 12, 110, TFT_BLACK);
    plStr(spr, PLF18, "NIEBIESKI", 12, 178, TFT_WHITE);
  });
  blTarget_ = cfg::BL_DAY;
}

// ------------------------------------------------------------------- chrome ----

// Nazwy ekranow — JEDNO zrodlo prawdy dla belki gornej i dla panelu WWW.
// Krotkie, bo w belce na tytul zostaje 152 px: "STATYSTYKI URZĄDZENIA" mialo 178 px
// i nie mieszczilo sie, a "FOTOWOLTAIKA" (najdluzsza z tych ponizej) ma 112 px.
// Indeks = cfg::VIEW_*, pilnuje tego static_assert w drawView().
const char* const kViewNames[cfg::VIEW_COUNT] = {
    "TERAZ", "GODZINY", "RADAR", "5 DNI", "W DOMU", "PIEC", "FOTOWOLTAIKA",
    "SAMOLOTY", "STATYSTYKI"};

// Zdrowie calego systemu w jednej liczbie: 0 = OK, 1 = uwaga, 2 = awaria.
//
// To jest DOKLADNIE to samo, co osiem kropek na ekranie statystyk, tylko zwiniete
// do jednej. Kropka w belce gornej jest widoczna zawsze, a ekran statystyk raz na
// dziewiec obrotow — bez tego podsumowania awaria potrafila wisiec niezauwazona
// przez kilka minut.
//
// "Wylaczone" NIE jest awaria: falownik spi po zachodzie, MQTT i piec moga byc
// swiadomie wylaczone, bramka moze nie istniec. Zolto swieci tylko to, co MIALO
// dzialac i jeszcze nie dostarczylo; czerwono to, co zglosilo blad.
int systemHealth(bool wifiOk) {
  if (!wifiOk) return 2;   // bez sieci nie dziala nic innego — nie ma po co liczyc dalej

  const Diag& d = diag();
  // `soft` = blad tego zrodla to najwyzej ostrzezenie, nigdy awaria calego systemu.
  struct S { uint32_t okAt; const char* err; bool off; bool soft; };
  const S s[8] = {
      {d.weatherOkAt, d.weatherErr, false, false},
      {d.radarOkAt, d.radarErr, false, false},
      {d.pvOkAt, d.pvErr, d.pvAsleep, false},
      {d.viOkAt, d.viErr, !settings().hasViessmann(), false},
      {d.flightOkAt, d.flightErr, false, false},
      {d.mqttOkAt, d.mqttErr, !settings().hasMqtt(), false},
      // blegw::lastError() niesie blad PIERWSZEJ padnietej bramki z listy, a nie stan
      // calej listy. Przy trzech bramkach jeden restartujacy sie Shelly zapalal przez
      // to czerwono na wszystkich dziewieciu ekranach, podczas gdy ekran statystyk
      // w tej samej klatce spokojnie pisal "2 z 3 zyje": dwa miejsca, dwie odpowiedzi.
      // Jedna cicha bramka to `soft`: zolto. Awaria to dopiero cisza WSZYSTKICH,
      // sprawdzana ponizej przez online() == 0.
      {blegw::lastOkAt(), blegw::lastError(), blegw::configured() == 0, true},
      {d.otaOkAt, "", false, false},
  };

  int worst = 0;
  for (const S& e : s) {
    if (e.off) continue;
    if (e.err[0] != '\0') {
      if (!e.soft) return 2;               // blad = czerwono, dalej nie ma co szukac
      worst = 1;
    } else if (e.okAt == 0) {
      worst = 1;                           // jeszcze nic nie przyslal
    }
  }

  // Bramki: czerwono dopiero wtedy, gdy zamilkly WSZYSTKIE skonfigurowane. Wtedy
  // faktycznie nie ma zadnego zrodla odczytow z bramek i to juz jest awaria.
  if (blegw::configured() > 0 && blegw::online() == 0) return 2;

  // Zdrowie samego urzadzenia — te progi maja juz swoje karty na statystykach,
  // wiec kropka tylko je powtarza, zamiast wprowadzac nowe reguly.
  //
  // Sterta BIEZACA, nie dolek historyczny. Progi HEAP_* opisuja stan "TERAZ":
  // HEAP_DANGER (25000) to poziom, ponizej ktorego radar nie zdekoduje PNG, a TLS
  // zaczyna sie dlawic. To prognoza na najblizsza klatke, a nie fakt z przeszlosci.
  // diag().minHeap to ESP.getMinFreeHeap(), czyli dolek DOZYWOTNI: nigdy nie rosnie
  // i na urzadzeniu stoi na 22044 B, bo tyle zostalo w najciezszym momencie setup().
  // Karmienie nim tych progow zapalaloby kropke na czerwono raz na zawsze, na kazdym
  // ekranie, do konca swiata, mimo ze sterta dawno wrocila do ~150 kB i wszystko dziala.
  // Dolek jest metryka HISTORYCZNA i ma swoje miejsce: biala kreska na wskazniku na
  // ekranie statystyk. Nie jest stanem biezacym i nie moze karmic progow stanu biezacego.
  const uint32_t heapNow = ESP.getFreeHeap();
  if (heapNow < cfg::HEAP_DANGER) return 2;
  if (heapNow < cfg::HEAP_WARN) worst = 1;
  if (otaTrialActive()) worst = 1;   // wersja probna jeszcze nie potwierdzila, ze dziala
  return worst;
}

void WeatherUi::drawHeader(TFT_eSPI& spr, const WeatherModel& w, bool wifiOk, uint32_t nowMs) {
  // Belka mieści się w całości w pasie górnym — w dolnym nie ma co liczyć.
  if (!spr.checkViewport(0, 0, W, cfg::HEADER_H)) {
    return;
  }
  spr.fillRect(0, 0, W, cfg::HEADER_H, col::HEADER);
  spr.drawFastHLine(0, cfg::HEADER_H - 1, W, col::DIVIDER);

  // Kropka to STAN CALEGO SYSTEMU, nie samo WiFi. Wczesniej mowila wylacznie
  // "jest siec" — czyli swiecila na zielono, gdy piec nie odpowiadal, radar sie
  // sypal i MQTT nie publikowal. Teraz zwija osiem kropek z ekranu statystyk do
  // jednej, widocznej na kazdym z dziewieciu ekranow.
  const int health = systemHealth(wifiOk);
  uint16_t dot = health == 0 ? col::OK : (health == 1 ? col::WARN : col::ERR);
  // Puls tylko przy awarii — zolte "cos nie dostarczylo" nie musi migac.
  if (health == 2 && ((nowMs / 500) % 2)) {
    dot = col::BG_CARD;
  }
  spr.fillCircle(12, 14, 4, dot);

  // Nazwa miasta USUNIETA — uzytkownik wie, gdzie mieszka. Zwolnione miejsce
  // bierze tytul ekranu, ktory dotad zjadal osobna linie w obszarze tresci.
  const time_t now = time(nullptr);

  // Zegar jeszcze nie ustawiony: nie ma daty, wiec nie ma tez okna dla tytulu.
  // Napis o synchronizacji zajmuje wtedy cale srodkowe pole.
  if (now < 1700000000) {
    plCenter(spr, PLF14, "synchronizacja czasu", W / 2, 19, col::TEXT_MUTE);
    plRight(spr, PLF18, "--:--", W - 10, 21, col::TEXT_MUTE);
    return;
  }

  struct tm tmv{};
  localtime_r(&now, &tmv);
  static const char* kMon[12] = {"sty", "lut", "mar", "kwi", "maj", "cze",
                                 "lip", "sie", "wrz", "paź", "lis", "gru"};
  static const char* kDow[7] = {"niedz", "pon", "wt", "śr", "czw", "pt", "sob"};

  char clk[8];
  snprintf(clk, sizeof(clk), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
  char dt[24];
  snprintf(dt, sizeof(dt), "%s %d %s", kDow[tmv.tm_wday % 7], tmv.tm_mday,
           kMon[tmv.tm_mon % 12]);

  // KAZDY element mierzony SWOJA czcionka. Wczesniej data i zegar byly sklejane
  // w jeden napis i mierzone razem jednym PLF14, a zegar rysuje sie PLF18.
  // Zmierzone na tablicach glifow: "18:49" to 36 px w PLF14, ale 50 px w PLF18,
  // czyli 14 px niedomiaru; do tego ginela 8 px przerwa miedzy data a zegarem.
  // Razem 22 px bledu przy marginesie, ktory wynosil 14 px. Tytul miescil sie
  // wylacznie dlatego, ze jest wysrodkowany, a nie dlatego, ze cos to liczylo.
  const int clkW = pltxt::stringWidth(PLF18, clk);
  const int rightW = pltxt::stringWidth(PLF14, dt) + 8 + clkW + 8;

  plRight(spr, PLF18, clk, W - 10, 21, col::ACCENT);
  // Data TUZ PRZED zegarem, nie na srodku — srodek nalezy teraz do tytulu.
  plRight(spr, PLF14, dt, W - 10 - clkW - 8, 19, col::TEXT_DIM);

  // Tytul wysrodkowany w tym, co ZOSTALO miedzy kropka a prawym blokiem, nie na
  // srodku ekranu. Zmierzone: przy najdluzszej dacie ("niedz 10 mar" = 83 px)
  // prawy blok zaczyna sie na x=169, wiec tytul wysrodkowany na W/2=160 wjezdzalby
  // w date. Okno ma wtedy 129 px, a najdluzszy tytul ("FOTOWOLTAIKA") 112 px, wiec
  // miesci sie z zapasem 25 px po kazdej stronie.
  const int lo = 24, hi = W - 10 - rightW - 8;
  if (hi > lo) {
    plCenter(spr, PLF14, kViewNames[view_ % cfg::VIEW_COUNT], (lo + hi) / 2, 19,
             col::ACCENT);
  }
}

void WeatherUi::drawProgress(TFT_eSPI& spr, uint32_t nowMs) {
  if (!spr.checkViewport(0, cfg::PROG_Y, W, cfg::PROG_H + 2)) {
    return;
  }
  spr.fillRect(0, cfg::PROG_Y, W, cfg::PROG_H + 2, col::BG);

  const int segW = W / cfg::VIEW_COUNT;
  float frac = 0.f;
  if (alertActive_) {
    frac = 1.f;
  } else if (transitioning_) {
    frac = 0.f;
  } else {
    frac = clampf(static_cast<float>(nowMs - viewStart_) / holdFor(view_), 0.f, 1.f);
  }

  for (int i = 0; i < cfg::VIEW_COUNT; ++i) {
    const int x = i * segW;
    const int wSeg = (i == cfg::VIEW_COUNT - 1) ? (W - x) : (segW - 3);

    // Ekran pominiety w rotacji (radar bez opadu, "w domu" bez czujnikow) dostaje
    // wlasny, przygaszony kolor — widac, ze istnieje, ale nie ma czego pokazac.
    const bool skipped = (i == cfg::VIEW_RADAR && !radarmap::hasRain()) ||
                         (i == cfg::VIEW_HOME && ble::count() == 0) ||
                         (i == cfg::VIEW_BOILER && !settings().hasViessmann());
    if (skipped) {
      spr.fillRect(x, cfg::PROG_Y, wSeg, cfg::PROG_H, lerp565(col::BG, col::RAIN, 0.30f));
      continue;
    }

    spr.fillRect(x, cfg::PROG_Y, wSeg, cfg::PROG_H, col::PV_TRACK);
    if (i < view_) {
      spr.fillRect(x, cfg::PROG_Y, wSeg, cfg::PROG_H, col::GRID_HI);
    } else if (i == view_) {
      spr.fillRect(x, cfg::PROG_Y, static_cast<int>(wSeg * frac), cfg::PROG_H,
                    alertActive_ ? alert_.color : col::ACCENT);
    }
  }
}

void WeatherUi::drawFooter(const PvModel& pv, bool wifiOk) {
  const int32_t ac = pv.online ? pv.data.powerAcW : 0;
  const int32_t g = pv.online ? pv.data.gridPowerW : 0;
  const int kwh = pv.online ? static_cast<int>(pv.data.energyTodayKwh * 10.f) : 0;

  const uint32_t now = millis();
  if (now - cpuTempAt_ > 10000 || cpuTempAt_ == 0) {
    cpuTempC_ = temperatureRead();
    cpuTempAt_ = now;
  }
  const int cpu = static_cast<int>(lroundf(cpuTempC_));

  // Stopka leży POZA buforem — rysujemy ją wprost na TFT, tylko przy zmianie danych.
  // pv.asleep MUSI być w tym porównaniu: przy zasypianiu falownika o zmroku
  // wszystkie liczby zostają zerami i bez tego stopka nigdy by się nie odświeżyła
  // (czerwone "nie odpowiada" wisiałoby całą noc).
  if (footerInit_ && ac == lastAc_ && g == lastGrid_ && kwh == lastKwh_ &&
      cpu == lastCpu_ && pv.online == lastOnline_ && pv.asleep == lastAsleep_) {
    return;
  }
  footerInit_ = true;
  lastAc_ = ac;
  lastGrid_ = g;
  lastKwh_ = kwh;
  lastCpu_ = cpu;
  lastOnline_ = pv.online;
  lastAsleep_ = pv.asleep;

  drawFooterTo(tft_, pv, wifiOk);
}

// Ta sama stopka, ale na dowolnym celu — TFT albo pasek zrzutu ekranu. Zawsze rysuje
// się na GLOBALNYM y=206; cel (viewport) sam decyduje, czy to w niego wpada.
void WeatherUi::drawFooterTo(TFT_eSPI& dst, const PvModel& pv, bool wifiOk) {
  const int y = VIEW_H;   // 206
  if (!dst.checkViewport(0, y, W, cfg::SCREEN_H - VIEW_H)) {
    return;
  }

  dst.fillRect(0, y, W, 34, col::HEADER);
  dst.drawFastHLine(0, y, W, col::DIVIDER);

  if (!pv.online) {
    const bool connecting = (pv.errorMsg[0] == '\0');
    // Uśpiony falownik to stan neutralny, nie awaria — kropka szara, nie czerwona.
    const uint16_t dot =
        pv.asleep ? col::TEXT_MUTE : (connecting ? col::WARN : col::ERR);
    dst.fillCircle(14, y + 17, 4, dot);
    const char* msg = connecting ? "Łączę z falownikiem..." : pv.errorMsg;
    plStr(dst, PLF14, wifiOk ? msg : "Brak WiFi", 26, y + 22, col::TEXT_MUTE);
    drawSysBoxTo(dst, y);
    return;
  }

  const PvSnapshot& d = pv.data;
  const int32_t ac = d.powerAcW;
  const int32_t g = d.gridPowerW;
  const int labelY = y + 4;
  const int baseY = y + 28;
  char v[16], u[8];

  dst.drawFastVLine(112, y + 6, 22, col::DIVIDER);
  dst.drawFastVLine(206, y + 6, 22, col::DIVIDER);

  dst.fillCircle(13, baseY - 5, 5, ac > 0 ? col::PV_SOLAR : col::TEXT_MUTE);
  gl(dst, "PRODUKCJA", 24, labelY, col::TEXT_MUTE);
  fmtPower(v, sizeof(v), u, sizeof(u), ac);
  int x = 24 + pltxt::drawString(dst, PLF18, v, 24, baseY, col::PV_SOLAR, col::PV_SOLAR);
  gl(dst, u, x + 4, baseY - 12, col::TEXT_DIM);

  gl(dst, "DZIS", 122, labelY, col::TEXT_MUTE);
  snprintf(v, sizeof(v), "%.1f", d.energyTodayKwh);
  int x2 = 122 + pltxt::drawString(dst, PLF18, v, 122, baseY, col::TEXT, col::TEXT);
  gl(dst, "kWh", x2 + 4, baseY - 12, col::TEXT_DIM);

  const bool exporting = g >= 0;
  const uint16_t gc = exporting ? col::PV_EXPORT : col::PV_IMPORT;
  gl(dst, exporting ? "ODDAJE" : "POBOR", 226, labelY, col::TEXT_MUTE);

  const int ax = 214, ay = baseY - 7;
  if (exporting) {
    dst.fillTriangle(ax, ay - 7, ax - 5, ay + 1, ax + 5, ay + 1, gc);
    dst.fillRect(ax - 2, ay + 1, 4, 7, gc);
  } else {
    dst.fillTriangle(ax, ay + 8, ax - 5, ay, ax + 5, ay, gc);
    dst.fillRect(ax - 2, ay - 7, 4, 7, gc);
  }
  fmtPower(v, sizeof(v), u, sizeof(u), g < 0 ? -g : g);
  int x3 = 226 + pltxt::drawString(dst, PLF18, v, 226, baseY, gc, gc);
  gl(dst, u, x3 + 4, baseY - 12, col::TEXT_DIM);

  drawSysBoxTo(dst, y);
}

void WeatherUi::drawSysBoxTo(TFT_eSPI& dst, int y) {
  dst.drawFastVLine(292, y + 6, 22, col::DIVIDER);
  char b[12];
  snprintf(b, sizeof(b), "v%d", FW_VERSION);
  plRight(dst, PLF14, b, W - 4, y + 15, col::TEXT_MUTE);
  const uint16_t tc = cpuTempC_ >= 75.f ? col::WARN : col::TEXT_DIM;
  snprintf(b, sizeof(b), "%.0f°", cpuTempC_);
  plRight(dst, PLF14, b, W - 4, y + 30, tc);
}

// Czyścimy CAŁY obszar rysowania (0..205), a nie tylko treść (34..205).
// Ten sam bufor obsługuje oba pasy, więc piksel, którego klatka nie zamaluje,
// zostaje z POPRZEDNIEGO PASA. Konkretnie: wiersz 28 leży między belką (0..27)
// a paskiem postępu (29..33) i nikt go nie rysuje — w pasie górnym wyświetliłby
// się wtedy kawałek wiersza 131 z pasa dolnego poprzedniej klatki.
void WeatherUi::drawContentBg(TFT_eSPI& spr) {
  spr.fillRect(0, 0, W, VIEW_H, col::BG);
}

// ------------------------------------------------------- nagłówek sekcji ------
// JEDEN nagłówek dla wszystkich widoków. Wcześniej każdy ekran rysował go po
// swojemu: raz GLCD, raz PlFont, linia bazowa 38 / 42 / 46, tytuł raz turkusowy,
// raz żółty. Efekt: ekrany wyglądały jak z trzech różnych aplikacji.
// Zasada: chrome (tytuł) zawsze tak samo, kolorem mówią wyłącznie DANE.

constexpr int HDR_Y = 46;  // wspólna linia bazowa tytułu i etykiety po prawej

// Tytul NIE jest tu juz rysowany: od v95 siedzi w belce gornej, wysrodkowany, w
// miejscu po nazwie miasta (patrz kViewNames i drawHeader). Zostawianie go w obu
// miejscach oznaczaloby ten sam napis dwa razy, jeden pod drugim. Dlatego funkcja
// nie przyjmuje juz tytulu: martwy parametr z `(void)title;` byl pulapka. Wygladal
// na zywy, wiec ktos predzej czy pozniej zmienilby go w wywolaniu i szukal, czemu
// ekran sie nie zmienil. Zrodlem prawdy dla tytulu jest wylacznie kViewNames.
static void viewHeader(TFT_eSPI& spr, int ox, const char* right = nullptr,
                       uint16_t rightCol = col::TEXT_MUTE, uint16_t dotCol = 0) {
  if (right == nullptr || right[0] == '\0') return;

  const int rw = pltxt::stringWidth(PLF14, right);
  if (dotCol != 0) spr.fillCircle(ox + W - 10 - rw - 11, HDR_Y - 5, 4, dotCol);
  plRight(spr, PLF14, right, ox + W - 10, HDR_Y, rightCol);
}

// ----------------------------------------------------- opis pogody pod ikona --
// Do 2 linii, lamane przy spacji tak, zeby linie byly mozliwie rowne.
// Dlaczego 2, a nie 1: "Częściowe zachmurzenie" ma 161 px, a pod ikona (srodek
// x=258) miesci sie najwyzej 116 px do krawedzi ekranu. Kazde POJEDYNCZE slowo
// sie miesci (najdluzsze, "Zachmurzenie", ma 91 px), wiec lamanie przy spacji
// zawsze wystarcza — sprawdzone dla wszystkich 28 kodow WMO.
constexpr int kDescMaxW = 116;

static void drawWeatherDesc(TFT_eSPI& spr, int cx, const char* text, const char* extra,
                            uint16_t color, uint16_t extraColor) {
  char a[48] = {}, b[48] = {};

  if (pltxt::stringWidth(PLF14, text) <= kDescMaxW) {
    snprintf(a, sizeof(a), "%s", text);
  } else {
    int bestDiff = INT32_MAX;
    for (const char* p = strchr(text, ' '); p != nullptr; p = strchr(p + 1, ' ')) {
      char la[48] = {};
      const size_t n = static_cast<size_t>(p - text);
      if (n == 0 || n >= sizeof(la)) continue;
      memcpy(la, text, n);
      const int wa = pltxt::stringWidth(PLF14, la);
      const int wb = pltxt::stringWidth(PLF14, p + 1);
      if (wa > kDescMaxW || wb > kDescMaxW) continue;
      const int diff = wa > wb ? wa - wb : wb - wa;
      if (diff < bestDiff) {
        bestDiff = diff;
        snprintf(a, sizeof(a), "%s", la);
        snprintf(b, sizeof(b), "%s", p + 1);
      }
    }
    if (a[0] == '\0') snprintf(a, sizeof(a), "%s", text);   // awaryjnie: bez lamania
  }

  // Jedna linia opisu + faza ksiezyca pod spodem (noc, czyste niebo).
  if (b[0] == '\0' && extra != nullptr) {
    plCenter(spr, PLF14, a, cx, 111, color);
    plCenter(spr, PLF14, extra, cx, 125, extraColor);
    return;
  }
  if (b[0] == '\0') {
    plCenter(spr, PLF14, a, cx, 118, color);
    return;
  }
  plCenter(spr, PLF14, a, cx, 111, color);
  plCenter(spr, PLF14, b, cx, 125, color);
}

// ------------------------------------------------------------ WIDOK 1: TERAZ --

void WeatherUi::drawViewNow(TFT_eSPI& spr, int ox, float t, const WeatherModel& w) {
  const WeatherSnapshot& c = w.current;
  const float e = easeOutCubic(t);

  char hdr[24];
  snprintf(hdr, sizeof(hdr), "dane z %s", w.updatedAt[0] ? w.updatedAt : "--:--");
  viewHeader(spr, ox, hdr);

  // --- wielka temperatura ---
  char big[12];
  fmtTemp(big, sizeof(big), c.tempC);
  const uint16_t tc = tempColor(c.tempC);
  const int bw = bigStr(spr, &FreeSansBold24pt7b, big, ox + 12, 102, tc);
  plStr(spr, PLF18, "°C", ox + 16 + bw, 76, col::TEXT_DIM);
  gl(spr, "TEMPERATURA", ox + 13, 60, col::TEXT_MUTE);

  char feels[32];
  snprintf(feels, sizeof(feels), "odczuwalna %d°C", static_cast<int>(lroundf(c.feelsC)));
  plStr(spr, PLF14, feels, ox + 12, 118, col::TEXT_DIM);

  // --- ikona + opis (swiadome pory doby) ---
  // Do v72 o polnocy swiecilo tu slonce z podpisem "Słonecznie". Teraz po zachodzie
  // pojawia sie ksiezyc w AKTUALNEJ FAZIE — takiej, jaka realnie widac nad Gdynia.
  // Ikona zeszla z 62 na 54 px, zeby zrobic miejsce na DWIE linie opisu. Sam obrazek
  // i tak nie niesie tyle informacji co "Częściowe zachmurzenie" zamiast "Częściowo".
  const int icx = ox + 258;
  const int size = 40 + static_cast<int>(14 * e);
  const bool night = !c.isDay;
  const float mp = moon::phase(time(nullptr));

  wxico::draw(spr, c.weatherCode, icx, 72, size, night, mp);

  // W nocy przy czystym niebie druga linia to faza ksiezyca — sam sierp nic nie mowi.
  const bool clearNight = night && wxico::iconForCode(c.weatherCode) == wxico::SUN;
  drawWeatherDesc(spr, icx, wxico::descForCode(c.weatherCode, night),
                  clearNight ? moon::name(mp) : nullptr, col::TEXT, col::TEXT_MUTE);

  // --- 4 kafelki ---
  struct Card {
    const char* label;
    char value[8];
    const char* unit;
    const char* extra;
    uint16_t color;
  } cards[4];

  cards[0] = {"Wiatr", {0}, "km/h", windDirName(c.windDir), col::WIND};
  snprintf(cards[0].value, sizeof(cards[0].value), "%d", static_cast<int>(lroundf(c.windKmh)));

  cards[1] = {"Wilgoć", {0}, "%", nullptr, col::HUMID};
  snprintf(cards[1].value, sizeof(cards[1].value), "%d", c.humidity);

  cards[2] = {"Ciśnienie", {0}, "hPa", nullptr, col::PRESS};
  snprintf(cards[2].value, sizeof(cards[2].value), "%d", static_cast<int>(lroundf(c.pressureHpa)));

  cards[3] = {"Chmury", {0}, "%", nullptr, col::CLOUD};
  snprintf(cards[3].value, sizeof(cards[3].value), "%d", c.cloudCover);

  const int cy0 = 131;   // nizej niz 122: druga linia opisu siega y=128
  const int chh = 52;
  for (int i = 0; i < 4; ++i) {
    const int x = ox + 6 + i * 78;
    const int cw = 74;
    const int grow = static_cast<int>(chh * clampf(e * 1.3f - i * 0.08f, 0.f, 1.f));
    if (grow < 4) continue;
    const int yy = cy0 + (chh - grow);
    spr.fillRoundRect(x, yy, cw, grow, 6, col::BG_CARD);
    if (grow < chh - 2) continue;

    spr.fillRect(x, cy0, 3, chh, cards[i].color);
    spr.fillRoundRect(x, cy0, 3, chh, 1, cards[i].color);

    plStr(spr, PLF14, cards[i].label, x + 8, cy0 + 17, col::TEXT_DIM);
    if (cards[i].extra != nullptr) {
      plRight(spr, PLF14, cards[i].extra, x + cw - 7, cy0 + 17, cards[i].color);
    }
    const int vw = pltxt::drawString(spr, PLF18, cards[i].value, x + 8, cy0 + 43, col::TEXT,
                                     col::TEXT);
    gl(spr, cards[i].unit, x + 10 + vw, cy0 + 36, col::TEXT_MUTE);
  }

  // --- pasek dolny: wschód / zachód / UV ---
  const int by = 196;
  spr.fillCircle(ox + 14, by - 4, 5, col::SUN);
  spr.fillRect(ox + 8, by - 3, 13, 5, col::BG);
  char b[24];
  snprintf(b, sizeof(b), "%s", w.sunrise[0] ? w.sunrise : "--:--");
  plStr(spr, PLF14, b, ox + 26, by, col::TEXT_DIM);

  // Zachód dosunięty do wschodu — cała wolna przestrzeń zostaje po prawej, dla
  // opadu i UV. Wcześniej "opad 12h ..." wjeżdżał wprost na godzinę zachodu.
  spr.fillCircle(ox + 86, by - 4, 5, col::ACCENT_WARM);
  spr.fillRect(ox + 80, by - 9, 13, 5, col::BG);
  snprintf(b, sizeof(b), "%s", w.sunset[0] ? w.sunset : "--:--");
  plStr(spr, PLF14, b, ox + 98, by, col::TEXT_DIM);
  const int sunEnd = ox + 98 + pltxt::stringWidth(PLF14, b);

  // UV BIEŻĄCE (nie dobowe maksimum — po zachodzie ma być 0).
  // W ciągu dnia w nawiasie dopisujemy dzisiejszy szczyt.
  const float uv = c.uvIndex;
  uint16_t uvc = col::TEXT_MUTE;
  if (uv >= 6.f) uvc = col::ERR;
  else if (uv >= 3.f) uvc = col::WARN;
  else if (uv >= 1.f) uvc = col::OK;

  if (uv < 0.5f) {
    snprintf(b, sizeof(b), "UV 0");
  } else {
    snprintf(b, sizeof(b), "UV %.0f", uv);
  }
  plRight(spr, PLF14, b, ox + W - 10, by, uvc);
  const int uvW = pltxt::stringWidth(PLF14, b);

  // Opad — priorytet ma RADAR (realny pomiar). Model bywa ślepy na lokalne ulewy.
  char pb[24] = {};
  uint16_t pc = col::RAIN;
  float next12 = 0.f;

  if (w.radarValid && w.radarLevel > 0) {
    pc = w.radarLevel >= 4 ? col::ERR : (w.radarLevel == 3 ? col::WARN : col::RAIN);
    snprintf(pb, sizeof(pb), "RADAR: %s", radarLabel(w.radarLevel));
  } else if (c.precipMm > 0.05f) {
    snprintf(pb, sizeof(pb), "deszcz %.1f mm/h", c.precipMm);
  } else {
    for (int i = 0; i < WX_HOURS; ++i) {
      if (w.hours[i].valid) next12 += w.hours[i].data.precipMm;
    }
    if (next12 > 0.2f) {
      pc = col::RAIN_DK;
      snprintf(pb, sizeof(pb), "opad 12h %.1f mm", next12);
    }
  }

  // Ten napis siedział na sztywnym środku i wjeżdżał w godzinę zachodu
  // ("21:15opad 12h 0.5 mm"). Teraz dokleja się do UV od prawej, a jeśli i tak
  // nie mieści się obok zachodu — skraca się sam, zamiast nachodzić na sąsiada.
  if (pb[0]) {
    const int right = ox + W - 10 - uvW - 14;
    if (right - pltxt::stringWidth(PLF14, pb) < sunEnd + 10 && next12 > 0.2f) {
      snprintf(pb, sizeof(pb), "opad %.1f mm", next12);
    }
    if (right - pltxt::stringWidth(PLF14, pb) >= sunEnd + 6) {
      plRight(spr, PLF14, pb, right, by, pc);
    }
  }
}

// --------------------------------------------------------- WIDOK 2: GODZINY --
void WeatherUi::drawViewHours(TFT_eSPI& spr, int ox, float t, const WeatherModel& w) {
  const float e = easeOutCubic(t);

  viewHeader(spr, ox, "TEMP / OPAD");

  // N punktow: teraz + WX_HOURS godzin. Ta liczba MUSI byc wyprowadzona, nie wpisana.
  // Wczesniej stalo tu "13", sprzezone z WX_HOURS=12 wylacznie przez wiare, a w petlach
  // nizej lezaly gole 12 i 13. Zmiana WX_HOURS na 24 (calkiem realna: "chce prognoze
  // na dobe") kompilowala sie bez ostrzezenia i pisala 12 elementow POZA koniec szesciu
  // tablic na stosie rdzenia rysujacego. To nie cicha korupcja — to restart w petli
  // na scianie, bez logu, bo log ginie razem z restartem.
  constexpr int N = WX_HOURS + 1;
  float temp[N];
  float rain[N];
  int code[N];
  int hourLbl[N];
  bool ok[N];
  bool day[N];   // is_day z API — bez tego o 22:00 swiecilo slonce

  temp[0] = w.current.tempC;
  rain[0] = w.current.precipMm;
  code[0] = w.current.weatherCode;
  day[0] = w.current.isDay;
  ok[0] = true;
  {
    time_t now = time(nullptr);
    struct tm tmv{};
    localtime_r(&now, &tmv);
    hourLbl[0] = tmv.tm_hour;
  }
  for (int i = 0; i < WX_HOURS; ++i) {
    const HourSlot& s = w.hours[i];
    ok[i + 1] = s.valid;
    temp[i + 1] = s.data.tempC;
    rain[i + 1] = s.data.precipMm;
    code[i + 1] = s.data.weatherCode;
    day[i + 1] = s.data.isDay;
    hourLbl[i + 1] = s.hourOfDay;
  }

  const float mp = moon::phase(time(nullptr));

  float vmin = 1e9f, vmax = -1e9f, rmax = 0.f;
  for (int i = 0; i < N; ++i) {
    if (!ok[i]) continue;
    if (temp[i] < vmin) vmin = temp[i];
    if (temp[i] > vmax) vmax = temp[i];
    if (rain[i] > rmax) rmax = rain[i];
  }
  if (vmin > vmax) {
    drawNoData(spr, ox, "Brak danych godzinowych");
    return;
  }
  if (vmax - vmin < 2.f) {
    const float mid = (vmax + vmin) / 2.f;
    vmin = mid - 1.f;
    vmax = mid + 1.f;
  }
  const float pad = (vmax - vmin) * 0.18f;
  vmin -= pad;
  vmax += pad;

  const int yTop = 58, yBot = 124;
  // Szerokosc na punkt liczona z WX_HOURS, nie zaszyta. Bylo 24, dobrane tak, zeby
  // 16 + 12*24 = 304 zmiescilo sie w 320 — czyli liczba zalezna od WX_HOURS udajaca stala.
  const int step = (W - 32) / WX_HOURS;
  auto px = [&](int i) { return ox + 16 + i * step; };
  auto py = [&](int i) {
    const float f = (temp[i] - vmin) / (vmax - vmin);
    return yBot - static_cast<int>(f * (yBot - yTop));
  };

  // siatka
  for (int g = 0; g <= 2; ++g) {
    const int y = yTop + g * (yBot - yTop) / 2;
    for (int x = ox + 14; x < ox + W - 10; x += 6) {
      spr.drawPixel(x, y, col::GRID);
    }
  }

  // wypełnienie pod krzywą + linia
  for (int i = 0; i < WX_HOURS; ++i) {
    if (!ok[i] || !ok[i + 1]) continue;
    const int x0 = px(i), x1 = px(i + 1);
    const int y0 = py(i), y1 = py(i + 1);
    for (int x = x0; x <= x1; ++x) {
      const float f = (x1 == x0) ? 0.f : static_cast<float>(x - x0) / (x1 - x0);
      const int y = y0 + static_cast<int>((y1 - y0) * f);
      const int ya = yBot - static_cast<int>((yBot - y) * e);
      const uint16_t lc = tempColor(temp[i] + (temp[i + 1] - temp[i]) * f);
      spr.drawFastVLine(x, ya, yBot - ya, lerp565(col::BG, lc, 0.16f));
      spr.drawFastVLine(x, ya - 1, 3, lc);
    }
  }

  // punkty + wartości co 2 godziny
  for (int i = 0; i < N; ++i) {
    if (!ok[i]) continue;
    const int x = px(i);
    const int y = yBot - static_cast<int>((yBot - py(i)) * e);
    if (i % 2 == 0) {
      spr.fillCircle(x, y, 3, col::BG);
      spr.fillCircle(x, y, 2, tempColor(temp[i]));
      char b[8];
      fmtTempInt(b, sizeof(b), temp[i]);
      plCenter(spr, PLF14, b, x, y - 7, col::TEXT);
    }
  }

  // opady
  const int rBase = 156;
  spr.drawFastHLine(ox + 12, rBase, W - 24, col::GRID_HI);
  if (rmax < 0.05f) {
    int prob = 0;
    for (int i = 0; i < WX_HOURS; ++i) {
      if (w.hours[i].valid && w.hours[i].data.precipProb > prob) {
        prob = w.hours[i].data.precipProb;
      }
    }
    char b[40];
    snprintf(b, sizeof(b), "bez opadów  (max szansa %d%%)", prob);
    plCenter(spr, PLF14, b, ox + W / 2, rBase - 6, col::TEXT_MUTE);
  } else {
    const float scale = rmax < 1.f ? 1.f : rmax;
    for (int i = 1; i < N; ++i) {
      if (!ok[i] || rain[i] <= 0.f) continue;
      const int h = static_cast<int>((rain[i] / scale) * 26.f * e);
      if (h < 1) continue;
      const int x = px(i) - 6;
      spr.fillRoundRect(x, rBase - h, 12, h, 2, col::RAIN);
    }
    char b[16];
    snprintf(b, sizeof(b), "%.1f mm", rmax);
    glRight(spr, b, ox + W - 12, rBase - 34, col::RAIN);
    gl(spr, "OPAD", ox + 12, rBase - 34, col::RAIN_DK);
  }

  // godziny + ikony
  for (int i = 0; i <= WX_HOURS; i += 2) {
    if (!ok[i]) continue;
    const int x = px(i);
    char h[8];
    snprintf(h, sizeof(h), "%02d", hourLbl[i]);
    glCenter(spr, h, x, 160, i == 0 ? col::ACCENT : col::TEXT_DIM);
    wxico::draw(spr, code[i], x, 186, 24, !day[i], mp);
  }
}
// --------------------------------------------------------- WIDOK 3: 5 DNI ----

void WeatherUi::drawViewDays(TFT_eSPI& spr, int ox, float t, const WeatherModel& w) {
  const float e = easeOutCubic(t);

  viewHeader(spr, ox, "MAX / MIN °C");

  float vmin = 1e9f, vmax = -1e9f, rmax = 0.f;
  int n = 0;
  for (int i = 0; i < WX_DAYS; ++i) {
    const DaySlot& d = w.days[i];
    if (!d.valid) continue;
    ++n;
    if (d.tempMin < vmin) vmin = d.tempMin;
    if (d.tempMax > vmax) vmax = d.tempMax;
    if (d.precipMm > rmax) rmax = d.precipMm;
  }
  if (n == 0) {
    drawNoData(spr, ox, "Brak prognozy dziennej");
    return;
  }
  if (vmax - vmin < 4.f) {
    const float mid = (vmax + vmin) / 2.f;
    vmin = mid - 2.f;
    vmax = mid + 2.f;
  }

  const int yTop = 100, yBot = 156;
  auto mapY = [&](float v) {
    const float f = (v - vmin) / (vmax - vmin);
    return yBot - static_cast<int>(f * (yBot - yTop));
  };

  for (int i = 0; i < WX_DAYS; ++i) {
    const DaySlot& d = w.days[i];
    if (!d.valid) continue;
    const int cx = ox + 32 + i * 64;

    wxico::draw(spr, d.weatherCode, cx, 66, 28);

    char b[12];
    fmtTempInt(b, sizeof(b), d.tempMax);
    plCenter(spr, PLF14, b, cx, 95, tempColor(d.tempMax));

    // słupek zakresu temperatur — rośnie od dołu
    const int y1 = mapY(d.tempMax);
    const int y0 = mapY(d.tempMin);
    const int full = y0 - y1;
    const int h = static_cast<int>(full * clampf(e * 1.25f - i * 0.06f, 0.f, 1.f));
    const int bx = cx - 8;
    spr.fillRoundRect(bx, yTop, 16, yBot - yTop, 8, col::PV_TRACK);
    if (h > 2) {
      const int top = y0 - h;
      for (int y = top; y <= y0; ++y) {
        const float f = (y0 == top) ? 0.f : static_cast<float>(y0 - y) / (y0 - top);
        const float tv = d.tempMin + (d.tempMax - d.tempMin) * f;
        spr.drawFastHLine(bx + 1, y, 14, tempColor(tv));
      }
      spr.fillRoundRect(bx, top, 16, 5, 2, tempColor(d.tempMax));
      spr.fillRoundRect(bx, y0 - 4, 16, 5, 2, tempColor(d.tempMin));
    }

    // Caly dolny blok przesuniety o 2-3 px w gore. Powod: data byla PRZYCIETA.
    // gl()/glCenter() kotwicza GORA, ale glify PlFont10 siegaja od baseline-11 do
    // baseline+2, wiec glCenter(y=197) rysowal realnie 196..209 — a obszar tresci
    // konczy sie na 205 (VIEW_H=206, dalej jest stopka). Trzy piksele dat scinala
    // stopka, a do tego napis wchodzil na nazwe dnia (ta konczyla sie na 197).
    // Teraz: temp 170, pasek opadu 174..178, nazwa 179..192, data 194..204.
    fmtTempInt(b, sizeof(b), d.tempMin);
    plCenter(spr, PLF14, b, cx, 170, col::TEXT_DIM);

    // opad — pasek pod słupkiem
    if (d.precipMm > 0.2f) {
      const float scale = rmax < 4.f ? 4.f : rmax;
      int rw = static_cast<int>((d.precipMm / scale) * 44.f * e);
      if (rw < 3) rw = 3;
      if (rw > 44) rw = 44;
      spr.fillRoundRect(cx - 22, 174, 44, 4, 2, col::PV_TRACK);
      spr.fillRoundRect(cx - 22, 174, rw, 4, 2, col::RAIN);
    }

    // Pierwszy dzień był po prostu NIEBIESKI — kolor niósł informację "to jutro",
    // której nikt nie miał jak odczytać. Piszemy to słowem, w tym samym kolorze
    // co reszta. Kolor zostaje wyłącznie dla danych (temperatura, opad).
    plCenter(spr, PLF14, i == 0 ? "JUTRO" : d.name, cx, 192, col::TEXT);
    glCenter(spr, d.date, cx, 194, col::TEXT_MUTE);
  }
}

void WeatherUi::drawNoData(TFT_eSPI& spr, int ox, const char* msg, const char* sub) {
  spr.fillRoundRect(ox + 40, CY + 50, W - 80, 60, 8, col::BG_CARD);
  if (sub && sub[0]) {
    plCenter(spr, PLF14, msg, ox + W / 2, CY + 78, col::TEXT_DIM);
    plCenter(spr, PLF14, sub, ox + W / 2, CY + 96, col::TEXT_MUTE);
  } else {
    plCenter(spr, PLF14, msg, ox + W / 2, CY + 86, col::TEXT_DIM);
  }
}

// ------------------------------------------------------------- WIDOK 4: PV ----

void WeatherUi::drawViewPv(TFT_eSPI& spr, int ox, float t, const PvModel& pv, const PvHistory& hist) {
  const float e = easeOutCubic(t);
  if (!pv.online) {
    viewHeader(spr, ox, pv.asleep ? "uśpiony" : "brak łączności",
               col::TEXT_MUTE, pv.asleep ? col::TEXT_MUTE : col::ERR);
    // Noc: falownik ma prawo milczeć (Huawei wyłącza Modbus TCP po zachodzie).
    // Piszemy to wprost, zamiast straszyć awarią do rana.
    drawNoData(spr, ox, pv.errorMsg[0] ? pv.errorMsg : "Falownik nie odpowiada",
               pv.asleep ? "Modbus TCP wyłączony po zachodzie" : nullptr);
    return;
  }

  const PvSnapshot& d = pv.data;

  // status falownika — jedyny kolorowy element nagłówka, bo niesie informację
  const bool fault = pvStatusIsFault(d.statusCode);
  const uint16_t sc = fault ? col::ERR : (pvStatusIsRunning(d.statusCode) ? col::OK : col::WARN);
  viewHeader(spr, ox, pvStatusLabel(d.statusCode), sc, sc);

  // --- wielka moc AC (animowana) ---
  const int32_t acNow = static_cast<int32_t>(lroundf(animAcW_));
  char v[16], u[8];
  fmtPower(v, sizeof(v), u, sizeof(u), acNow);
  gl(spr, "MOC CHWILOWA", ox + 13, 54, col::TEXT_MUTE);
  const int pw = bigStr(spr, &FreeSansBold24pt7b, v, ox + 12, 100, col::PV_SOLAR);
  // Jednostka na linii bazowej LICZBY (100), nie w indeksie gornym (74).
  // Indeks gorny dzialal tylko w dzien: przy "5.20" jednostka ladowala na x=110 i
  // mijala etykiete. Noca produkcja to "0" — 27 px zamiast 98 — wiec "W" siadalo
  // na x=43, prosto w napis "MOC CHWILOWA" (ktory zajmuje x=13..83, y=54..64,
  // a jednostka w indeksie gornym y=60..74). Kolizja byla widoczna co noc.
  // Na linii bazowej liczby "0 W" i "5.20 kW" czytaja sie tak samo i nie ma jak
  // nachodzic na cokolwiek nad soba.
  plStr(spr, PLF18, u, ox + 16 + pw, 100, col::TEXT_DIM);

  char sub[40];
  snprintf(sub, sizeof(sub), "DC %ld W   |   %.0f V", static_cast<long>(d.powerDcW),
           d.pvVoltageV);
  // 104, nie 108: gl() kotwiczy tekst GORA (TL_DATUM), wiec zajmuje 104..112.
  // Nizej jest "PRODUKCJA / ZUŻYCIE" w PlFont14 (13 px wysokosci) — przy 108
  // te dwa napisy na siebie wchodzily.
  gl(spr, sub, ox + 13, 104, col::TEXT_DIM);

  // --- wskaźnik (arc) ---
  // Skala = DOKLADNIE moc wpisana w panelu. Wczesniej pvScaleW_ bylo zapadka: roslo,
  // gdy produkcja przekroczyla deklarowana moc (AC * 1.05), i NIGDY nie wracalo —
  // nawet po zmianie ustawienia. Uzytkownik wpisywal 6.0 kWp, a ekran uparcie
  // pokazywal "z 7.0 kWp" (bo 6667 W * 1.05 = 7000) i nie dalo sie tego zrozumiec.
  const float peakW = settings().pvPeakW > 0 ? static_cast<float>(settings().pvPeakW) : 6000.f;

  const int gx = ox + 266, gy = 88, gr = 34, gir = 26;
  const float frac = clampf(animAcW_ / peakW, 0.f, 1.f) * e;
  glCenter(spr, "OBCIAZENIE", gx, 48, col::TEXT_MUTE);
  smoothArc(spr, gx, gy, gr, gir, 30, 330, col::PV_TRACK, col::BG);
  if (frac > 0.005f) {
    const int end = 30 + static_cast<int>(300.f * frac);
    const uint16_t ac = lerp565(col::PV_SOLAR, col::PV_EXPORT, clampf(frac, 0.f, 1.f));
    smoothArc(spr, gx, gy, gr, gir, 30, end, ac, col::BG);
  }
  // Procentu NIE przycinamy do 100: produkcja powyzej mocy nominalnej zdarza sie
  // realnie (zimne, sloneczne dni) i jest ciekawa informacja, a nie bledem. Luk
  // dobija do konca, ale liczba mowi prawde.
  const int pctVal = static_cast<int>(lroundf(animAcW_ / peakW * 100.f * e));
  char pct[8];
  snprintf(pct, sizeof(pct), "%d%%", pctVal);
  plCenter(spr, PLF18, pct, gx, gy + 3, pctVal > 100 ? col::PV_EXPORT : col::TEXT);
  char peak[14];
  snprintf(peak, sizeof(peak), "z %.1f kWp", peakW / 1000.f);
  glCenter(spr, peak, gx, gy + 9, col::TEXT_MUTE);

  // --- profil dnia: produkcja + zużycie w jednej skali ---
  // Kolory są DOKŁADNIE te, którymi świeci dioda RGB — jeden kod barw w całym
  // urządzeniu, więc wykres czyta się bez legendy:
  //   zielony   = nadwyżka oddana do sieci      (dioda zielona)
  //   niebieski = zużycie pokryte z własnej PV  (dioda niebieska — równowaga)
  //   czerwony  = prąd dobrany z sieci          (dioda czerwona)
  plStr(spr, PLF14, "PRODUKCJA / ZUŻYCIE", ox + 12, 128, col::TEXT_MUTE);
  char tot[24];
  snprintf(tot, sizeof(tot), "RAZEM %.0f kWh", d.energyTotalKwh);
  glRight(spr, tot, ox + W - 12, 122, col::TEXT_MUTE);

  const int sx = ox + 12, sy = 132, sw = W - 24, sh = 26;
  spr.fillRoundRect(sx, sy, sw, sh, 3, col::CHART_SPARK_BG);
  const int s0 = 30, s1 = 137;  // 05:00 .. 22:50
  uint16_t hpk = hist.peak();
  if (hpk < 500) hpk = 500;
  const int base = sy + sh - 1;
  const float scale = static_cast<float>(sh - 3) * e / static_cast<float>(hpk);

  for (int s = s0; s <= s1; ++s) {
    if (!hist.filled[s]) continue;
    const uint16_t prod = hist.watts[s];
    const uint16_t used = hist.load[s];
    if (prod == 0 && used == 0) continue;

    const int x = sx + ((s - s0) * (sw - 2)) / (s1 - s0);
    const int hp = static_cast<int>(prod * scale);
    const int hu = static_cast<int>(used * scale);

    if (hu >= hp) {
      // Zużycie przewyższa produkcję. Dół słupka (to, co pokryła PV) jest niebieski,
      // a czego zabrakło — sterczy nad nim na czerwono. To ten "pik" z sieci.
      if (hp > 0) {
        spr.drawFastVLine(x, base - hp, hp, col::PV_HOUSE);
        spr.drawFastVLine(x + 1, base - hp, hp, col::PV_HOUSE);
      }
      const int deficit = hu - hp;
      if (deficit > 0) {
        spr.drawFastVLine(x, base - hu, deficit, col::PV_IMPORT);
        spr.drawFastVLine(x + 1, base - hu, deficit, col::PV_IMPORT);
      }
    } else {
      // Produkcja pokrywa dom: na dole niebieskie zużycie własne, nad nim zielona
      // nadwyżka, która poszła do sieci.
      spr.drawFastVLine(x, base - hp, hp - hu, col::PV_EXPORT);
      spr.drawFastVLine(x + 1, base - hp, hp - hu, col::PV_EXPORT);
      if (hu > 0) {
        spr.drawFastVLine(x, base - hu, hu, col::PV_HOUSE);
        spr.drawFastVLine(x + 1, base - hu, hu, col::PV_HOUSE);
      }
    }
  }
  // znaczniki godzin 8 / 12 / 16 / 20
  for (int hh = 8; hh <= 20; hh += 4) {
    const int s = hh * 6;
    const int x = sx + ((s - s0) * (sw - 2)) / (s1 - s0);
    spr.drawFastVLine(x, sy + sh - 3, 3, col::GRID_HI);
  }

  // --- 4 kafelki ---
  struct PvCard {
    const char* label;
    char value[12];
    char unit[6];
    uint16_t color;
  } cards[4];

  cards[0] = {"Dziś", {0}, {0}, col::PV_SOLAR};
  snprintf(cards[0].value, sizeof(cards[0].value), "%.1f", d.energyTodayKwh);
  snprintf(cards[0].unit, sizeof(cards[0].unit), "kWh");

  cards[1] = {"Dom", {0}, {0}, col::PV_HOUSE};
  fmtPower(v, sizeof(v), u, sizeof(u), static_cast<int32_t>(lroundf(animLoadW_)));
  snprintf(cards[1].value, sizeof(cards[1].value), "%s", v);
  snprintf(cards[1].unit, sizeof(cards[1].unit), "%s", u);

  const int32_t g = static_cast<int32_t>(lroundf(animGridW_));
  const bool exporting = g >= 0;
  cards[2] = {exporting ? "Oddaję" : "Pobór", {0}, {0},
              exporting ? col::PV_EXPORT : col::PV_IMPORT};
  fmtPower(v, sizeof(v), u, sizeof(u), g < 0 ? -g : g);
  snprintf(cards[2].value, sizeof(cards[2].value), "%s", v);
  snprintf(cards[2].unit, sizeof(cards[2].unit), "%s", u);

  cards[3] = {"Falownik", {0}, {0}, d.inverterTempC > 65.f ? col::WARN : col::TEXT_DIM};
  snprintf(cards[3].value, sizeof(cards[3].value), "%.0f", d.inverterTempC);
  snprintf(cards[3].unit, sizeof(cards[3].unit), "°C");

  const int cy0 = 164, chh = 41;
  for (int i = 0; i < 4; ++i) {
    const int x = ox + 6 + i * 78;
    const int grow = static_cast<int>(chh * clampf(e * 1.3f - i * 0.08f, 0.f, 1.f));
    if (grow < 4) continue;
    spr.fillRoundRect(x, cy0 + (chh - grow), 74, grow, 6, col::BG_CARD);
    if (grow < chh - 2) continue;
    spr.fillRoundRect(x, cy0, 3, chh, 1, cards[i].color);
    plStr(spr, PLF14, cards[i].label, x + 8, cy0 + 14, col::TEXT_DIM);
    const int vw = pltxt::drawString(spr, PLF18, cards[i].value, x + 8, cy0 + 37,
                                     cards[i].color, cards[i].color);

    // "°C" ma znak spoza ASCII, którego GLCD nie zna — rysował krzaczek. Jednostki
    // z polskimi/typograficznymi znakami idą przez PlFont, reszta (kW, kWh) zostaje
    // na GLCD, bo jest węższa i mieści się obok dużej liczby.
    bool ascii = true;
    for (const char* p = cards[i].unit; *p; ++p) {
      if (static_cast<unsigned char>(*p) > 127) ascii = false;
    }
    if (ascii) {
      gl(spr, cards[i].unit, x + 11 + vw, cy0 + 30, col::TEXT_MUTE);
    } else {
      plStr(spr, PLF14, cards[i].unit, x + 11 + vw, cy0 + 37, col::TEXT_MUTE);
    }
  }
}

// ------------------------------------------------------- wskaźnik ze strefami --
// Pionowy słupek, w którym TŁO niesie tyle samo informacji co wypełnienie:
// przygaszone pasy pokazują, gdzie kończy się strefa bezpieczna, a gdzie zaczyna
// niebezpieczna. Dzięki temu widać nie tylko "ile jest", ale też "ile jeszcze można".
// Sama liczba (53 °C, 112 kB) nic nie mówi, jeśli nie wiadomo, co jest granicą.

struct GaugeZone {
  float upTo;    // górna granica strefy (w jednostkach wartości)
  uint16_t col;  // kolor strefy
};

// DWIE kolumny obok siebie, nie jedna. Wcześniej wypełnienie szło od dołu i
// zamalowywało strefy — przy RAM-ie strefa niebezpieczna JEST na dole, więc nigdy
// nie było jej widać. Teraz:
//   lewa, wąska  = skala stref (zawsze widoczna: gdzie jest granica)
//   prawa, szersza = poziom (ile jest teraz), w kolorze strefy
static void zoneGauge(TFT_eSPI& spr, int x, int y, int w, int h, float v, float vmin,
                      float vmax, const GaugeZone* z, int nz, float mark, bool hasMark) {
  const float span = vmax - vmin;
  auto pos = [&](float val) {  // wartość -> y (od dołu)
    return y + h - static_cast<int>(clampf((val - vmin) / span, 0.f, 1.f) * h);
  };

  const int sw = 4;             // skala stref
  const int bx = x + sw + 2;    // słupek poziomu
  const int bw = w - sw - 2;

  // --- skala stref (pełne kolory, przygaszone) ---
  float lo = vmin;
  for (int i = 0; i < nz; ++i) {
    const int y0 = pos(z[i].upTo), y1 = pos(lo);
    if (y1 > y0) spr.fillRect(x, y0, sw, y1 - y0, lerp565(col::BG, z[i].col, 0.55f));
    lo = z[i].upTo;
  }

  // --- słupek poziomu ---
  uint16_t fc = z[nz - 1].col;
  for (int i = 0; i < nz; ++i) {
    if (v <= z[i].upTo) {
      fc = z[i].col;
      break;
    }
  }
  spr.fillRect(bx, y, bw, h, col::PV_TRACK);
  const int fy = pos(v);
  if (y + h - fy > 0) spr.fillRect(bx, fy, bw, y + h - fy, fc);

  // znacznik: minimum sterty w historii / granica z noty katalogowej.
  // Idzie po SKALI, nie po słupku — inaczej ginie w wypełnieniu.
  if (hasMark) {
    const int my = pos(mark);
    if (my > y && my < y + h) spr.drawFastHLine(x - 1, my, sw + 2, col::TEXT);
  }

  spr.drawRect(bx, y, bw, h, col::GRID_HI);
}

// ------------------------------------------------------------------- ALERT ----

void WeatherUi::drawAlert(TFT_eSPI& spr, float t) {
  const float e = easeOutCubic(t);
  const int pad = static_cast<int>(20 * (1.f - e));
  const int x = 8 + pad, y = CY + 4 + pad / 2;
  const int w = W - 16 - pad * 2, h = CH - 12 - pad;

  spr.fillRoundRect(x, y, w, h, 10, col::ALERT_BG);
  spr.drawRoundRect(x, y, w, h, 10, alert_.color);
  spr.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 9, alert_.color);

  if (alert_.iconCode >= 0) {
    wxico::draw(spr, alert_.iconCode, x + 52, y + 76, 56);
  } else {
    // trójkąt ostrzegawczy
    const int cx = x + 52, cy = y + 76;
    spr.fillTriangle(cx, cy - 26, cx - 28, cy + 22, cx + 28, cy + 22, alert_.color);
    spr.fillRect(cx - 3, cy - 12, 6, 20, col::ALERT_BG);
    spr.fillRect(cx - 3, cy + 12, 6, 6, col::ALERT_BG);
  }

  plStr(spr, PLF18, alert_.title, x + 100, y + 62, alert_.color);
  plStr(spr, PLF14, alert_.text, x + 100, y + 92, col::TEXT);
}

void WeatherUi::raiseAlert(const Alert& a, uint32_t nowMs) {
  alert_ = a;
  alertActive_ = true;
  alertStart_ = nowMs;
  enterStart_ = nowMs;
  transitioning_ = false;
}

// ------------------------------------------------------------------ RENDER ----

uint32_t WeatherUi::holdFor(uint8_t view) const {
  if (view == cfg::VIEW_FLIGHTS) return cfg::VIEW_HOLD_FLIGHTS_MS;
  if (view == cfg::VIEW_STATS) return cfg::VIEW_HOLD_STATS_MS;
  if (view == cfg::VIEW_RADAR) return cfg::VIEW_HOLD_RADAR_MS;
  return cfg::VIEW_HOLD_MS;
}

bool WeatherUi::needsFlights(uint32_t nowMs) const {
  if (view_ == cfg::VIEW_FLIGHTS) {
    return true;
  }
  // prefetch tuz przed przejsciem na ekran lotow
  const uint8_t prev =
      static_cast<uint8_t>((cfg::VIEW_FLIGHTS + cfg::VIEW_COUNT - 1) % cfg::VIEW_COUNT);
  if (view_ == prev && !transitioning_ && !alertActive_) {
    const uint32_t hold = holdFor(view_);
    const uint32_t el = nowMs - viewStart_;
    if (hold > cfg::FLIGHT_PREFETCH_MS && el >= hold - cfg::FLIGHT_PREFETCH_MS) {
      return true;
    }
  }
  return false;
}

void WeatherUi::drawView(TFT_eSPI& spr, uint8_t view, int ox, float t, const WeatherModel& w,
                         const PvModel& pv, const PvHistory& hist, const FlightModel& fl,
                         uint32_t nowMs, uint32_t heapNow) {
  // Golych "case 0:" tu juz nie ma. To bylo JEDYNE miejsce, gdzie mapowanie
  // numer widoku -> funkcja bylo zapisane literalami, podczas gdy drawProgress(),
  // render(), prevView() i holdFor() uzywaly cfg::VIEW_*. Skutek pominiecia tego
  // switcha przy dodawaniu ekranu: czarny obraz przez 9 s, bez bledu — default
  // nic nie rysuje, a pasek postepu pokazuje segment jakby wszystko gralo.
  static_assert(cfg::VIEW_STATS == cfg::VIEW_COUNT - 1,
                "ostatni widok musi byc VIEW_COUNT-1 — inaczej rotacja trafia w default");
  switch (view) {
    case cfg::VIEW_NOW:
      if (w.ready) drawViewNow(spr, ox, t, w);
      else drawNoData(spr, ox, "Pobieram prognozę...");
      break;
    case cfg::VIEW_HOURS:
      if (w.ready) drawViewHours(spr, ox, t, w);
      else drawNoData(spr, ox, "Pobieram prognozę...");
      break;
    case cfg::VIEW_DAYS:
      if (w.ready) drawViewDays(spr, ox, t, w);
      else drawNoData(spr, ox, "Pobieram prognozę...");
      break;
    case cfg::VIEW_RADAR:
      drawViewRadar(spr, ox, t, nowMs);
      break;
    case cfg::VIEW_HOME:
      drawViewHome(spr, ox, t, w);
      break;
    case cfg::VIEW_BOILER:
      drawViewBoiler(spr, ox, t);
      break;
    case cfg::VIEW_PV:
      drawViewPv(spr, ox, t, pv, hist);
      break;
    case cfg::VIEW_FLIGHTS:
      drawViewFlights(spr, ox, t, fl);
      break;
    case cfg::VIEW_STATS:
      drawViewStats(spr, ox, t, nowMs, heapNow);
      break;
    default:
      break;
  }
}

// Pełna klatka w GLOBALNYCH współrzędnych (y=0..205). Wywoływana raz na pas —
// wszystko liczy się deterministycznie z nowMs i stanu obiektu, więc oba pasy
// dostają identyczną treść i sklejają się w jeden obraz.
void WeatherUi::paintFrame(TFT_eSPI& spr, const WeatherModel& w, const PvModel& pv,
                           const PvHistory& hist, const FlightModel& fl, bool wifiOk,
                           uint32_t nowMs, uint32_t heapNow) {
  const float enterT = clampf(static_cast<float>(nowMs - enterStart_) / cfg::ENTER_ANIM_MS,
                              0.f, 1.f);
  drawContentBg(spr);

  if (alertActive_) {
    drawAlert(spr, clampf(static_cast<float>(nowMs - alertStart_) / 260.f, 0.f, 1.f));
  } else if (transitioning_) {
    const float p =
        easeOutCubic(static_cast<float>(nowMs - transStart_) / cfg::TRANSITION_MS);
    const int off = static_cast<int>(p * W);
    drawView(spr, prevView_, -off, 1.f, w, pv, hist, fl, nowMs, heapNow);
    drawView(spr, view_, W - off, enterT, w, pv, hist, fl, nowMs, heapNow);
  } else {
    drawView(spr, view_, 0, enterT, w, pv, hist, fl, nowMs, heapNow);
  }

  drawHeader(spr, w, wifiOk, nowMs);
  drawProgress(spr, nowMs);
}

bool WeatherUi::render(const WeatherModel& w, const PvModel& pv, const PvHistory& hist,
                       const FlightModel& fl, bool wifiOk, uint32_t nowMs) {
  if (!ready_) {
    return false;
  }
  // Po OTA bufor bywa zwolniony — bez tego rysowalibyśmy w nicość (czarny ekran).
  if (freed_ && !restoreBuffer()) {
    return false;
  }
  if (viewStart_ == 0) {
    viewStart_ = nowMs;
    enterStart_ = nowMs;
  }

  // --- animowane liczniki PV (łagodne dojście do wartości) ---
  bool animating = false;
  const float k = 0.16f;
  const float tgtAc = pv.online ? static_cast<float>(pv.data.powerAcW) : 0.f;
  const float tgtGrid = pv.online ? static_cast<float>(pv.data.gridPowerW) : 0.f;
  const float tgtLoad = pv.online ? static_cast<float>(pv.data.houseLoadW) : 0.f;

  auto approach = [&](float& cur, float tgt) {
    if (fabsf(tgt - cur) > 1.5f) {
      cur += (tgt - cur) * k;
      animating = true;
    } else {
      cur = tgt;
    }
  };
  approach(animAcW_, tgtAc);
  approach(animGridW_, tgtGrid);
  approach(animLoadW_, tgtLoad);

  // --- alert ---
  if (alertActive_ && nowMs - alertStart_ >= cfg::ALERT_SHOW_MS) {
    alertActive_ = false;
    viewStart_ = nowMs;
    enterStart_ = nowMs;
  }

  // --- rotacja widoków (wstrzymana, gdy ekran przypięty z panelu WWW) ---
  if (!alertActive_) {
    if (transitioning_) {
      if (nowMs - transStart_ >= cfg::TRANSITION_MS) {
        transitioning_ = false;
        viewStart_ = nowMs;
      }
    } else if (pinned_ >= 0) {
      // ekran zablokowany — nic nie robimy
    } else if (nowMs - viewStart_ >= holdFor(view_)) {
      prevView_ = view_;
      view_ = static_cast<uint8_t>((view_ + 1) % cfg::VIEW_COUNT);
      // Ekran "W DOMU" bez czujników byłby pustym kafelkiem co minutę — pomijamy go
      // w rotacji, dopóki nikt nie skonfiguruje choć jednego.
      if (view_ == cfg::VIEW_HOME && ble::count() == 0) {
        // Na KOLEJNY ekran (PIEC), nie na PV: skok na VIEW_PV przeskakiwał VIEW_BOILER,
        // wiec przy braku czujnikow BLE piec wypadal z rotacji, mimo ze byl
        // skonfigurowany. Warunek ponizej i tak pominie PIEC, gdy nieautoryzowany.
        view_ = static_cast<uint8_t>(cfg::VIEW_BOILER);
      }
      // Radar bez opadu = pusta mapa. Pokazywanie jej co minutę nie ma sensu —
      // pomijamy ekran, ale pasek postępu i tak go zaznaczy (innym kolorem),
      // żeby było widać, że taki ekran istnieje i po prostu nie ma co pokazywać.
      if (view_ == cfg::VIEW_RADAR && !radarmap::hasRain()) {
        view_ = static_cast<uint8_t>(cfg::VIEW_DAYS);
      }
      if (view_ == cfg::VIEW_BOILER && !settings().hasViessmann()) {
        view_ = static_cast<uint8_t>(cfg::VIEW_PV);
      }
      transitioning_ = true;
      transStart_ = nowMs;
      enterStart_ = nowMs;
    }
  }

  const float enterT = clampf(static_cast<float>(nowMs - enterStart_) / cfg::ENTER_ANIM_MS,
                              0.f, 1.f);

  // --- rysowanie: dwa pasy po 103 px ---
  // Każdy pas rysuje CAŁĄ klatkę (w globalnym układzie) i wypycha swój kawałek.
  // Sklejenie na y=103 wychodzi piksel w piksel, bo obie iteracje dostają to samo
  // nowMs i ten sam stan — elementy przecięte granicą (np. łuk PV, ikona pogody)
  // są rysowane w obu pasach, każdy zobaczy tylko swoją połowę.
  // Jeden odczyt sterty na klatkę, nie na pas — inaczej ekran statystyk pokazałby
  // w górnym pasie inną liczbę niż w dolnym (patrz komentarz przy paintFrame).
  const uint32_t heapNow = ESP.getFreeHeap();

  // Pomiar klatki idzie do diagnostyki, a nie na Serial — urzadzenie wisi na
  // scianie i portu szeregowego nikt nie zobaczy. Koszt: dwa micros() na klatke.
  uint32_t tPaint = 0, tPush = 0;
  for (int b = 0; b < BAND_N; ++b) {
    const int top = b * BAND_H;
    const uint32_t t0 = micros();
    setBand(spr_, top, VIEW_H);
    paintFrame(spr_, w, pv, hist, fl, wifiOk, nowMs, heapNow);
    const uint32_t t1 = micros();
    spr_.pushSprite(0, top);
    tPaint += t1 - t0;
    tPush += micros() - t1;
  }
  spr_.resetViewport();

  // srednia krocząca — pojedyncza klatka potrafi zlapac przerwanie WiFi
  diag().frameDrawUs = (diag().frameDrawUs * 7 + tPaint) / 8;
  diag().framePushUs = (diag().framePushUs * 7 + tPush) / 8;

  drawFooter(pv, wifiOk);   // poza buforem, wprost na TFT
  tickBacklight();

  if (cfg::PROFILE_FRAME) {
    static uint32_t lastLog = 0;
    if (nowMs - lastLog > 2000) {
      lastLog = nowMs;
      Serial.printf("KLATKA: rysowanie %lu us, wypchniecie %lu us, heap %lu (min %lu), blok %lu\n",
                    static_cast<unsigned long>(tPaint), static_cast<unsigned long>(tPush),
                    static_cast<unsigned long>(ESP.getFreeHeap()),
                    static_cast<unsigned long>(ESP.getMinFreeHeap()),
                    static_cast<unsigned long>(ESP.getMaxAllocHeap()));
    }
  }

  animating = animating || transitioning_ || alertActive_ || enterT < 1.f ||
              blCurrent_ != blTarget_;
  return animating;
}

// ------------------------------------------------------- WIDOK 5: SAMOLOTY ----

namespace {

// Rzutowanie w oknie gmapf. Z definicji gmapf::LON_MIN/LON_MAX daje to dokladnie
// (piksel w rastrze gmapw) - gmapf::X_OFF, czyli to samo, co rysuje ponizej petla ladu.
void mapProject(float lat, float lon, int ox, int& x, int& y) {
  x = ox + static_cast<int>((lon - gmapf::LON_MIN) / (gmapf::LON_MAX - gmapf::LON_MIN) *
                            gmapf::MAP_W);
  y = CY + static_cast<int>((gmapf::LAT_MAX - lat) / (gmapf::LAT_MAX - gmapf::LAT_MIN) *
                            gmapf::MAP_H);
}

uint16_t flightColor(const Flight& f) {
  if (flightToGdansk(f)) return col::FLY_ARRIVE;
  if (flightFromGdansk(f)) return col::FLY_DEPART;
  return col::FLY_OVER;
}

// maks. 4 znaki, zeby nie kolidowac z trasa w waskiej liscie
void fmtAlt(char* buf, size_t n, int32_t ft) {
  const float m = ft * 0.3048f;
  if (m < 950.f) {
    snprintf(buf, n, "%dm", static_cast<int>(lroundf(m / 10.f)) * 10);
  } else {
    snprintf(buf, n, "%dkm", static_cast<int>(lroundf(m / 1000.f)));
  }
}

}  // namespace

void WeatherUi::drawViewFlights(TFT_eSPI& spr, int ox, float t, const FlightModel& fl) {
  const float e = easeOutCubic(t);
  const int mx = ox;
  const int my = CY;

  // --- morze ---
  spr.fillRect(mx, my, gmapf::MAP_W, gmapf::MAP_H, col::MAP_SEA);

  // --- ląd (okno gmapf w rastrze gmapw: przesuniecie o X_OFF i przyciecie do 224 px) ---
  for (int row = 0; row < gmapf::MAP_H; ++row) {
    const uint16_t a = pgm_read_word(&gmapw::LAND_ROW_OFF[row]);
    const uint16_t b = pgm_read_word(&gmapw::LAND_ROW_OFF[row + 1]);
    for (uint16_t s = a; s < b; ++s) {
      int x0 = static_cast<int>(pgm_read_word(&gmapw::LAND_SPANS[s][0])) - gmapf::X_OFF;
      int x1 = static_cast<int>(pgm_read_word(&gmapw::LAND_SPANS[s][1])) - gmapf::X_OFF;
      if (x1 < 0 || x0 > gmapf::MAP_W - 1) continue;   // pas w calosci poza oknem
      if (x0 < 0) x0 = 0;
      if (x1 > gmapf::MAP_W - 1) x1 = gmapf::MAP_W - 1;
      spr.drawFastHLine(mx + x0, my + row, x1 - x0 + 1, col::MAP_LAND);
    }
  }

  // --- linia brzegowa (krawedzie pasow ladu, tak samo jak na radarze) ---
  // Dawna polilinia gmap::COAST_PTS znikla razem z MapData.h — gmapw nie ma konturu,
  // bo radar rysuje wybrzeze krawedziami pasow. Teraz oba ekrany robia to identycznie.
  for (int row = 0; row < gmapf::MAP_H; ++row) {
    const uint16_t a = pgm_read_word(&gmapw::LAND_ROW_OFF[row]);
    const uint16_t b = pgm_read_word(&gmapw::LAND_ROW_OFF[row + 1]);
    for (uint16_t s = a; s < b; ++s) {
      const uint16_t rx0 = pgm_read_word(&gmapw::LAND_SPANS[s][0]);
      const uint16_t rx1 = pgm_read_word(&gmapw::LAND_SPANS[s][1]);
      // Krawedz to wybrzeze tylko wtedy, gdy pas konczy sie wewnatrz RASTRA (320 px).
      // Test musi isc po gmapw, nie po oknie — inaczej brzeg okna udaje wybrzeze
      // i wzdluz mapy leci pionowa krecha (ten sam blad co kiedys na radarze).
      const int x0 = static_cast<int>(rx0) - gmapf::X_OFF;
      const int x1 = static_cast<int>(rx1) - gmapf::X_OFF;
      if (rx0 > 0 && x0 >= 0 && x0 < gmapf::MAP_W) {
        spr.drawPixel(mx + x0, my + row, col::MAP_COAST);
      }
      if (rx1 < gmapw::MAP_W - 1 && x1 >= 0 && x1 < gmapf::MAP_W) {
        spr.drawPixel(mx + x1, my + row, col::MAP_COAST);
      }
    }
  }

  // --- etykiety miejsc ---
  int lx, ly;
  mapProject(54.6053f, 18.8026f, ox, lx, ly);
  gl(spr, "HEL", lx + 5, ly - 3, col::MAP_LABEL);

  mapProject(54.3500f, 18.6600f, ox, lx, ly);
  gl(spr, "GDANSK", lx - 12, ly, col::MAP_LABEL);

  mapProject(54.7186f, 18.4092f, ox, lx, ly);
  glRight(spr, "PUCK", lx - 3, ly - 4, col::MAP_LABEL);

  // --- lotnisko EPGD ---
  mapProject(cfg::EPGD_LAT, cfg::EPGD_LON, ox, lx, ly);
  spr.drawRect(lx - 3, ly - 3, 7, 7, col::TEXT);
  spr.drawFastHLine(lx - 5, ly, 11, col::TEXT);
  gl(spr, "EPGD", lx + 8, ly - 3, col::TEXT_DIM);

  // --- dom (Częstochowska) ---
  mapProject(settings().lat, settings().lon, ox, lx, ly);
  spr.drawCircle(lx, ly, 6, col::ACCENT);
  spr.fillCircle(lx, ly, 3, col::ACCENT);
  gl(spr, "GDYNIA", lx - 40, ly - 4, col::ACCENT);

  // --- samoloty ---
  for (int i = 0; i < fl.count; ++i) {
    const Flight& f = fl.list[i];
    int x, y;
    mapProject(f.lat, f.lon, ox, x, y);
    const uint16_t c = flightColor(f);

    const float a = f.track * 0.0174533f;
    const float R = 9.f * e;
    const float r = 6.f * e;
    const int tx = x + static_cast<int>(sinf(a) * R);
    const int ty = y - static_cast<int>(cosf(a) * R);
    const int ax = x + static_cast<int>(sinf(a + 2.5f) * r);
    const int ay = y - static_cast<int>(cosf(a + 2.5f) * r);
    const int bx = x + static_cast<int>(sinf(a - 2.5f) * r);
    const int by = y - static_cast<int>(cosf(a - 2.5f) * r);
    spr.fillTriangle(tx, ty, ax, ay, bx, by, c);
    spr.drawTriangle(tx, ty, ax, ay, bx, by, col::BG);

    // numer wiążący z listą
    int nx = x + 12;
    int ny = y - 12;
    if (nx > mx + gmapf::MAP_W - 9) nx = x - 12;
    if (ny < my + 9) ny = y + 12;
    if (ny > my + gmapf::MAP_H - 9) ny = y - 12;
    spr.fillCircle(nx, ny, 7, c);
    spr.drawCircle(nx, ny, 7, col::BG);
    char nb[4];
    snprintf(nb, sizeof(nb), "%d", i + 1);
    glCenter(spr, nb, nx, ny - 3, col::BG);
  }

  // --- legenda ---
  const int ly0 = my + gmapf::MAP_H - 12;
  spr.fillRect(mx, ly0, gmapf::MAP_W, 12, col::BG);
  spr.fillCircle(mx + 7, ly0 + 6, 4, col::FLY_ARRIVE);
  gl(spr, "przylot GDN", mx + 14, ly0 + 2, col::TEXT_DIM);
  spr.fillCircle(mx + 100, ly0 + 6, 4, col::FLY_DEPART);
  gl(spr, "odlot GDN", mx + 107, ly0 + 2, col::TEXT_DIM);
  spr.fillCircle(mx + 180, ly0 + 6, 4, col::FLY_OVER);
  gl(spr, "przelot", mx + 187, ly0 + 2, col::TEXT_DIM);

  // ---------------------------------------------------------- panel listy ----
  const int lxp = ox + 228;
  spr.drawFastVLine(ox + 224, CY, CH, col::DIVIDER);

  plStr(spr, PLF14, "NA NIEBIE", lxp, 46, col::ACCENT);
  if (fl.ready) {
    char cb[8];
    snprintf(cb, sizeof(cb), "%d", fl.total);
    plRight(spr, PLF14, cb, ox + 318, 46, col::TEXT_DIM);
  }

  if (!fl.ready) {
    plStr(spr, PLF14, "Pobieram", lxp, 110, col::TEXT_MUTE);
    plStr(spr, PLF14, "dane...", lxp, 128, col::TEXT_MUTE);
    return;
  }
  if (fl.count == 0) {
    plStr(spr, PLF14, "Pusto", lxp, 110, col::TEXT_DIM);
    gl(spr, "brak samolotow", lxp, 118, col::TEXT_MUTE);
    gl(spr, "nad zatoka", lxp, 128, col::TEXT_MUTE);
    return;
  }

  for (int i = 0; i < fl.count; ++i) {
    const Flight& f = fl.list[i];
    const uint16_t c = flightColor(f);
    const int y0 = 52 + i * 25;
    const int grow = static_cast<int>(23 * clampf(e * 1.5f - i * 0.09f, 0.f, 1.f));
    if (grow < 6) continue;

    spr.fillRoundRect(lxp - 2, y0, 92, 23, 4, col::BG_CARD);
    spr.fillCircle(lxp + 8, y0 + 9, 8, c);
    char nb[4];
    snprintf(nb, sizeof(nb), "%d", i + 1);
    glCenter(spr, nb, lxp + 8, y0 + 6, col::BG);

    plStr(spr, PLF14, f.callsign, lxp + 20, y0 + 12, col::TEXT);
    gl(spr, f.routeKnown ? f.route : "lokalny", lxp + 20, y0 + 15, c);

    char ab[10];
    fmtAlt(ab, sizeof(ab), f.altFt);
    glRight(spr, ab, ox + 318, y0 + 15, col::TEXT_MUTE);
  }
}

// ------------------------------------------------ EKRAN KONFIGURACJI (AP) ----

void WeatherUi::drawSetup(const char* apSsid, const char* apPass, const char* apIp) {
  if (!ready_) return;
  const uint32_t ph = (millis() / 400) % 4;   // faza raz dla obu pasów

  pushBands([&](TFT_eSPI& spr) {
    spr.fillRect(0, 0, W, VIEW_H, col::BG);

    spr.fillRect(0, 0, W, cfg::HEADER_H, col::HEADER);
    spr.drawFastHLine(0, cfg::HEADER_H - 1, W, col::DIVIDER);
    plStr(spr, PLF14, "KONFIGURACJA", 12, 19, col::ACCENT);
    glRight(spr, "krok 1 z 2", W - 12, 10, col::TEXT_MUTE);

    plStr(spr, PLF14, "1. Połącz telefon z siecią:", 14, 56, col::TEXT_DIM);

    spr.fillRoundRect(14, 64, 292, 40, 8, col::BG_CARD);
    spr.fillRoundRect(14, 64, 4, 40, 2, col::ACCENT);
    plStr(spr, PLF18, apSsid, 26, 82, col::TEXT);
    plStr(spr, PLF14, "hasło:", 26, 99, col::TEXT_MUTE);
    plStr(spr, PLF14, apPass, 68, 99, col::ACCENT);

    plStr(spr, PLF14, "2. Otwórz w przeglądarce:", 14, 128, col::TEXT_DIM);

    spr.fillRoundRect(14, 136, 292, 38, 8, col::BG_CARD);
    spr.fillRoundRect(14, 136, 4, 38, 2, col::ACCENT_WARM);
    char url[32];
    snprintf(url, sizeof(url), "http://%s", apIp);
    plStr(spr, PLF18, url, 26, 161, col::TEXT);

    plStr(spr, PLF14, "Tam wybierzesz swoją sieć Wi-Fi,", 14, 186, col::TEXT_MUTE);
    plStr(spr, PLF14, "lokalizację i adres falownika.", 14, 202, col::TEXT_MUTE);

    // pulsujaca kropka aktywnosci
    for (uint32_t i = 0; i < 3; ++i) {
      spr.fillCircle(292 + i * 8 - 16, 196, 2, (i == ph) ? col::ACCENT : col::PV_TRACK);
    }
  });

  tft_.fillRect(0, VIEW_H, W, cfg::SCREEN_H - VIEW_H, col::BG);
  blTarget_ = cfg::BL_DAY;
  tickBacklight();
}

// ------------------------------------------------------ EKRAN AKTUALIZACJI ---

void WeatherUi::drawOta(int progress, const char* msg) {
  if (!ready_) return;
  const int p = progress < 0 ? 0 : (progress > 100 ? 100 : progress);

  pushBands([&](TFT_eSPI& spr) {
    spr.fillRect(0, 0, W, VIEW_H, col::BG);

    for (int y = 0; y < 70; ++y) {
      spr.drawFastHLine(0, y, W, lerp565(col::HEADER, col::BG, y / 70.f));
    }

    // strzalka w dol
    const int cx = W / 2;
    spr.fillRect(cx - 5, 52, 10, 26, col::ACCENT);
    spr.fillTriangle(cx, 92, cx - 16, 74, cx + 16, 74, col::ACCENT);
    spr.fillRoundRect(cx - 22, 100, 44, 5, 2, col::ACCENT);

    plCenter(spr, PLF18, "Aktualizacja", cx, 134, col::TEXT);
    plCenter(spr, PLF14, msg && msg[0] ? msg : "Pobieram...", cx, 158, col::TEXT_DIM);

    const int bx = 40, bw = W - 80, by = 176;
    spr.fillRoundRect(bx, by, bw, 10, 5, col::PV_TRACK);
    if (p > 0) {
      spr.fillRoundRect(bx, by, (bw * p) / 100, 10, 5, col::ACCENT);
    }
    char b[8];
    snprintf(b, sizeof(b), "%d%%", p);
    plCenter(spr, PLF14, b, cx, 200, col::ACCENT);
  });

  // Poniżej bufora (y>=206) rysujemy wprost na TFT.
  tft_.fillRect(0, VIEW_H, W, cfg::SCREEN_H - VIEW_H, col::BG);
  plCenter(tft_, PLF14, "Nie odłączaj zasilania", W / 2, 228, col::TEXT_MUTE);
  blTarget_ = cfg::BL_DAY;
  tickBacklight();
}

// ------------------------------------------- EKRAN: POŁĄCZONO / ADRES IP -----

void WeatherUi::drawNetInfo(const char* ssid, const char* ip, int rssi, int secsLeft,
                            int total) {
  if (!ready_) return;

  pushBands([&](TFT_eSPI& spr) {
    spr.fillRect(0, 0, W, VIEW_H, col::BG);

    // belka
    spr.fillRect(0, 0, W, cfg::HEADER_H, col::HEADER);
    spr.drawFastHLine(0, cfg::HEADER_H - 1, W, col::DIVIDER);
    spr.fillCircle(12, 14, 4, col::OK);
    plStr(spr, PLF14, "POŁĄCZONO Z SIECIĄ", 24, 19, col::OK);

    // Wersja, a w okresie probnym takze to, ze jest probna. Ten ekran jest PIERWSZYM,
    // co widac po restarcie z OTA — czyli dokladnie wtedy, gdy odpowiedz na pytanie
    // "ktora wersja wstala i czy juz sie obronila" jest najwiecej warta. Do v105
    // stalo tu samo "v106", identycznie jak przy wersji stabilnej, wiec ekran przemilczal
    // jedyna rzecz, ktora go w tym momencie odrozniala. Nazewnictwo i kolor jak w
    // drawViewStats — ten sam stan ma wygladac tak samo wszedzie.
    // Zmierzone: "v106 - próbna" = 89 px, tytul konczy sie na x=174, napis startuje
    // na x=221 -> 47 px odstepu, wiec nie ma prawa wejsc na tytul.
    char fw[24];
    const bool trial = otaTrialActive();
    if (trial) {
      snprintf(fw, sizeof(fw), "v%d - próbna", FW_VERSION);
    } else {
      snprintf(fw, sizeof(fw), "v%d", FW_VERSION);
    }
    plRight(spr, PLF14, fw, W - 10, 19, trial ? col::WARN : col::TEXT_MUTE);

    // Caly blok (ikona + siec + karta IP) stoi 14 px wyzej niz do v105. Powod jest
    // zmierzony, nie estetyczny: pod belka bylo 24 MARTWE wiersze (y=28..51), a dolne
    // 20 wierszy dzwigalo trzy elementy na raz — podpis, pasek i odliczanie zachodzily
    // na siebie. Ekran byl przeciazony u dolu i pusty u gory. Po przesunieciu pod belka
    // zostaje 10 px oddechu, a dol miesci wszystko z odstepami 5/7/5 px.

    // ikona WiFi — łuki o sile zależnej od RSSI
    const int wx = 42, wy = 82;
    const int bars = rssi >= -55 ? 3 : (rssi >= -70 ? 2 : (rssi >= -82 ? 1 : 0));
    for (int i = 0; i < 3; ++i) {
      const int r = 14 + i * 10;
      const uint16_t c = (i < bars) ? col::ACCENT : col::PV_TRACK;
      smoothArc(spr, wx, wy, r, r - 4, 225, 315, c, col::BG);
    }
    spr.fillCircle(wx, wy, 4, bars > 0 ? col::ACCENT : col::PV_TRACK);

    // sieć
    gl(spr, "SIEC", 92, 36, col::TEXT_MUTE);

    // SSID przycinamy POMIAREM, nie na oko. Pole ma 32 znaki (Settings.h), a miejsca
    // od x=92 do prawego marginesu jest 218 px. Zmierzone w PLF18: 32 x "M" = 512 px,
    // 32 x "A" = 448 px — czyli dluga nazwa wyjezdzala poza ekran (o 294 px w skrajnym
    // przypadku). Nikt tego nie zglosil tylko dlatego, ze tutejsza siec ma krotka nazwe.
    // Najpierw schodzimy na mniejszy font (wzorzec z drawViewStats), bo CALY SSID
    // mniejszym drukiem mowi wiecej niz polowa SSID duzym; dopiero potem tniemy.
    char sb[sizeof(Settings::ssid)];
    snprintf(sb, sizeof(sb), "%s", ssid);
    constexpr int kSsidMax = W - 10 - 92;   // 218 px
    const pltxt::FontSet sf = pltxt::stringWidth(PLF18, sb) <= kSsidMax ? PLF18 : PLF14;
    // Ciecie CALYMI ZNAKAMI, nie bajtami: SSID moze legalnie miec polskie litery, a te
    // zajmuja w UTF-8 dwa bajty. Urwanie samego ogona zostawiloby osierocony bajt
    // wiodacy, a wtedy pltxt::decodeUtf8 bierze NASTEPNY bajt jako kontynuacje — zjada
    // '\0' i czyta za buforem. Ten sam warunek co w drawViewStats: patrzymy na bajt,
    // NA KTORYM tniemy, i cofamy sie z bajtow kontynuacji (10xxxxxx) do wiodacego.
    while (sb[0] != '\0' && pltxt::stringWidth(sf, sb) > kSsidMax) {
      size_t n = strlen(sb) - 1;
      while (n > 0 && (static_cast<uint8_t>(sb[n]) & 0xC0) == 0x80) --n;
      sb[n] = '\0';
    }
    plStr(spr, sf, sb, 92, 62, col::TEXT);

    char sig[20];
    snprintf(sig, sizeof(sig), "%d dBm", rssi);
    gl(spr, sig, 92, 70, col::TEXT_DIM);

    // adres IP — duży, żeby dało się przepisać
    spr.fillRoundRect(14, 98, W - 28, 62, 10, col::BG_CARD);
    spr.fillRoundRect(14, 98, 4, 62, 2, col::ACCENT);
    gl(spr, "ADRES IP URZADZENIA", 30, 106, col::TEXT_MUTE);
    bigStr(spr, &FreeSansBold18pt7b, ip, 30, 148, col::ACCENT);

    // Podpowiedz, pasek i odliczanie stoja teraz JEDNO POD DRUGIM. Wczesniej dzielily
    // te same wiersze — i to jest zgloszony "tekst, ktory pojawia sie pod paskiem
    // odliczania". Zmierzone w starym ukladzie (plCenter kotwiczy LINIE BAZOWA):
    //   podpis   bl=196 -> y=185..199
    //   pasek    by=186 h=6 -> y=186..191   (rysowany PO tekscie, wiec go zamalowywal)
    //   odliczanie bl=204 -> y=193..205     (nachodzilo na dolne 7 wierszy podpisu)
    // Czyli kolizja byla POTROJNA, nie podwojna: podpis ginal pod paskiem I pod
    // odliczaniem naraz. Teraz, z pomiaru glifow (karta IP konczy sie na 159):
    //   podpis   bl=176 -> y=165..178       (5 px od karty)
    //   pasek    by=186 -> y=186..191       (7 px odstepu)
    //   odliczanie gl y=195 -> y=197..204   (5 px odstepu, dol tresci = 205)
    //
    // Podpis skrocony z "Panel konfiguracji dostępny pod tym adresem" (289 px, po 15 px
    // marginesu) na wersje bez "dostępny" (227 px, po 46 px) — to samo znaczenie, a
    // napis przestaje dotykac krawedzi.
    plCenter(spr, PLF14, "Panel konfiguracji pod tym adresem", W / 2, 176, col::TEXT_DIM);

    // odliczanie
    const int bx = 40, bw = W - 80, by = 186;
    spr.fillRoundRect(bx, by, bw, 6, 3, col::PV_TRACK);
    const float f = (total > 0) ? clampf(static_cast<float>(secsLeft) / total, 0.f, 1.f) : 0.f;
    if (f > 0.f) {
      spr.fillRoundRect(bx, by, static_cast<int>(bw * f), 6, 3, col::ACCENT);
    }
    char cd[24];
    snprintf(cd, sizeof(cd), "start za %d s", secsLeft);
    // glCenter, NIE plCenter: gl() kotwiczy GORE i sam dodaje PlFont10Ascent. Mniejszy
    // font, bo odliczanie to informacja drugorzedna — a przy PLF14 dolna krawedz
    // wypadala na 205, czyli dokladnie na ostatnim wierszu obszaru tresci.
    glCenter(spr, cd, W / 2, 195, col::TEXT_MUTE);
  });

  tft_.fillRect(0, VIEW_H, W, cfg::SCREEN_H - VIEW_H, col::BG);
  blTarget_ = cfg::BL_DAY;
  tickBacklight();
}

// ------------------------------- OTA: rysowanie bez bufora (oszczędza RAM) ---

void WeatherUi::releaseBuffer(bool clearScreen) {
  if (freed_ || !ready_) {
    return;
  }
  spr_.deleteSprite();
  freed_ = true;
  // Przy radarze NIE czyścimy ekranu — panel trzyma ostatnią klatkę, więc obraz
  // tylko zamiera na chwilę zamiast gasnąć.
  if (clearScreen) {
    tft_.fillScreen(col::BG);
  }
  Serial.printf("UI: zwolniono bufor, wolny heap=%u B\n",
                static_cast<unsigned>(ESP.getFreeHeap()));
}

bool WeatherUi::restoreBuffer() {
  if (!freed_) {
    return true;
  }
  spr_.setColorDepth(16);
  if (spr_.createSprite(cfg::SCREEN_W, BAND_H) == nullptr) {
    Serial.println("UI: nie udalo sie odtworzyc bufora!");
    return false;
  }
  footerInit_ = false;
  spr_.setSwapBytes(false);
  // createSprite ustawia viewport na fizyczny pas, więc fillRect(0,0,W,BAND_H) jest OK.
  spr_.fillRect(0, 0, W, BAND_H, col::BG);
  freed_ = false;
  Serial.printf("UI: odtworzono bufor, wolny heap=%u B\n",
                static_cast<unsigned>(ESP.getFreeHeap()));
  return true;
}

// Rysowane wprost na TFT — sprite'a już nie ma. Polskie znaki idą z PlFont,
// bo wbudowany font GLCD nie ma ą/ę/ł/ó.
void WeatherUi::drawOtaDirect(int progress, const char* msg) {
  if (!ready_) return;

  static int lastP = -1;
  static uint32_t frameAt = 0;
  const uint32_t now = millis();

  if (frameAt == 0 || now - frameAt > 30000) {
    frameAt = now;
    lastP = -1;
    tft_.fillScreen(col::BG);
    const int cx = W / 2;
    tft_.fillRect(cx - 5, 46, 10, 26, col::ACCENT);
    tft_.fillTriangle(cx, 86, cx - 16, 68, cx + 16, 68, col::ACCENT);
    tft_.fillRoundRect(cx - 22, 94, 44, 5, 2, col::ACCENT);

    plCenter(tft_, PLF18, "Aktualizacja", cx, 130, col::TEXT);
    plCenter(tft_, PLF14, "Nie odłączaj zasilania", cx, 228, col::TEXT_MUTE);
    tft_.drawRoundRect(40, 172, W - 80, 12, 6, col::PV_TRACK);
  }

  const int p = progress < 0 ? 0 : (progress > 100 ? 100 : progress);
  if (p != lastP) {
    lastP = p;
    tft_.fillRoundRect(42, 174, ((W - 84) * p) / 100, 8, 4, col::ACCENT);

    tft_.fillRect(60, 190, W - 120, 18, col::BG);
    char b[10];
    snprintf(b, sizeof(b), "%d%%", p);
    plCenter(tft_, PLF18, b, W / 2, 205, col::ACCENT);

    tft_.fillRect(20, 142, W - 40, 16, col::BG);
    plCenter(tft_, PLF14, msg && msg[0] ? msg : "Pobieram...", W / 2, 155, col::TEXT_DIM);
  }
}

// ------------------------------------------------- TEST DIODY RGB ------------

// drawLedTest() USUNIETY (v106). Byl ekranem autotestu diody: przez pierwsze 1,5 s po
// starcie zajmowal caly wyswietlacz napisem "Dioda powinna teraz swiecic na: CZERWONY".
// Sluzyl weryfikacji mapowania kanalow R/G/B — a to zostalo potwierdzone w v23
// (12.07.2026, komunikat commita "mapowanie R/G/B potwierdzone"). Od tamtej pory ekran
// pokazywal odpowiedz na pytanie, ktore juz nie bylo zadawane, i robil to kosztem
// jedynej informacji wartej wtedy pokazania: czy urzadzenie laczy sie z siecia.
// Sam autotest diody zostaje (Led.cpp, nieblokujacy) — zniknal tylko jego ekran.
// Gdyby kiedys trzeba bylo mapowanie sprawdzic ponownie: kolejnosc to R -> G -> B po
// 500 ms, widac ja na diodzie bez zadnego ekranu.

// ---------------------------------------------- WIDOK 6: STATYSTYKI ----------


// ------------------------------------------------ WIDOK 3: RADAR OPADOW -------
// Mapa Zatoki Gdanskiej z nalozonym RZECZYWISTYM obrazem opadu, animowana przez
// ostatnie 2 godziny. Nie mieszamy tego z ekranem samolotow — dwie warstwy danych
// na jednej mapie zrobilyby z niej kaszę.

void WeatherUi::drawViewRadar(TFT_eSPI& spr, int ox, float t, uint32_t nowMs) {
  const float e = easeOutCubic(t);

  const int n = radarmap::count();
  if (n == 0) {
    viewHeader(spr, ox, "pobieram...");
    drawNoData(spr, ox, "Pobieram mapę opadów", radarmap::lastError());
    return;
  }

  // Klatka animacji: cyklicznie, z krotka pauza na ostatniej (najnowszej) —
  // inaczej oko nie zdazy zauwazyc, gdzie pada TERAZ.
  const int steps = n + 2;   // 2 dodatkowe "przystanki" na koncu
  const int step = static_cast<int>((nowMs / cfg::RADAR_FRAME_MS) % steps);
  const int fi = step >= n ? n - 1 : step;

  const radarmap::Frame& fr = radarmap::frame(fi);
  diag().radarFrame = fi;
  diag().radarFrameMin = fr.offsetMin;

  // --- mapa na PELNA szerokosc (osobny raster gmapw, 320 px) ---
  // Ekran samolotow dzieli miejsce z lista lotow i musi miec 224 px. Radar nie ma
  // czego dzielic, wiec dostaje caly ekran — inaczej mapa byla przycieta z bokow.
  const int mx = ox;
  const int my = CY;

  spr.fillRect(mx, my, gmapw::MAP_W, gmapw::MAP_H, col::MAP_SEA);
  for (int row = 0; row < gmapw::MAP_H; ++row) {
    const uint16_t a = pgm_read_word(&gmapw::LAND_ROW_OFF[row]);
    const uint16_t b = pgm_read_word(&gmapw::LAND_ROW_OFF[row + 1]);
    for (uint16_t k = a; k < b; ++k) {
      const uint16_t x0 = pgm_read_word(&gmapw::LAND_SPANS[k][0]);
      const uint16_t x1 = pgm_read_word(&gmapw::LAND_SPANS[k][1]);
      spr.drawFastHLine(mx + x0, my + row, x1 - x0 + 1, col::MAP_LAND);
    }
  }

  // --- warstwa opadu ---
  // Rysujemy CIAGAMI, nie pikselami: przy 224x172 = 38 tys. pikseli pojedyncze
  // drawPixel zjadloby budzet klatki (21 ms) w calosci.
  static const uint16_t kPal[6] = {
      0,                       // 0 — brak opadu
      lerp565(col::MAP_SEA, col::RAIN, 0.55f),   // mzawka: ledwo widoczna
      col::RAIN,                                 // deszcz
      col::WARN,                                 // silny
      col::PV_IMPORT,                            // bardzo silny
      col::ERR,                                  // ulewa
  };

  const int rows = static_cast<int>(gmapw::MAP_H * e);   // animacja wejscia: opad "wchodzi"
  for (int y = 0; y < rows; ++y) {
    int x = 0;
    while (x < gmapw::MAP_W) {
      const uint8_t lv = radarmap::levelAt(fi, x, y);
      if (lv == 0) {
        ++x;
        continue;
      }
      int x2 = x + 1;
      while (x2 < gmapw::MAP_W && radarmap::levelAt(fi, x2, y) == lv) ++x2;
      spr.drawFastHLine(mx + x, my + y, x2 - x, kPal[lv > 5 ? 5 : lv]);
      x = x2;
    }
  }

  // --- KONTUR LADU NA WIERZCHU ---
  // Bez tego opad zamalowuje wybrzeze i mapa staje sie kolorowa plama, z ktorej
  // nie da sie odczytac, GDZIE pada. Rysujemy tylko krawedzie pasow ladu (lewa
  // i prawa), wiec kosztuje to dwa piksele na pas, a nie caly ląd.
  for (int row = 0; row < gmapw::MAP_H; ++row) {
    const uint16_t a = pgm_read_word(&gmapw::LAND_ROW_OFF[row]);
    const uint16_t b = pgm_read_word(&gmapw::LAND_ROW_OFF[row + 1]);
    for (uint16_t k = a; k < b; ++k) {
      const uint16_t x0 = pgm_read_word(&gmapw::LAND_SPANS[k][0]);
      const uint16_t x1 = pgm_read_word(&gmapw::LAND_SPANS[k][1]);
      // Piksel na KRAWEDZI KADRU to nie wybrzeze, tylko miejsce, w ktorym mapa sie
      // konczy. Rysowany dawal pionowa biala krechę wzdluz calego lewego brzegu,
      // bo lad (Pomorze) dochodzi tam do granicy obrazu.
      if (x0 > 0) spr.drawPixel(mx + x0, my + row, col::MAP_COAST_HI);
      if (x1 < gmapw::MAP_W - 1) spr.drawPixel(mx + x1, my + row, col::MAP_COAST_HI);
    }
  }

  // --- Gdynia ---
  int gx = 0, gy = 0;
  {
    const float lat = settings().lat, lon = settings().lon;
    gx = mx + static_cast<int>((lon - gmapw::LON_MIN) / (gmapw::LON_MAX - gmapw::LON_MIN) *
                               gmapw::MAP_W);
    gy = my + static_cast<int>((gmapw::LAT_MAX - lat) / (gmapw::LAT_MAX - gmapw::LAT_MIN) *
                               gmapw::MAP_H);
  }
  spr.drawCircle(gx, gy, 4, col::BG);
  spr.drawCircle(gx, gy, 3, col::ACCENT);
  spr.fillCircle(gx, gy, 1, col::ACCENT);
  gl(spr, "GDYNIA", gx + 7, gy - 4, col::ACCENT);

  // Naglowek DOPIERO TERAZ, na wlasnym pasku. Mapa zaczyna sie od CY (jak ekran
  // samolotow), wiec rysowana wczesniej zamalowywalaby tytul. Pasek zaslania
  // gorne 18 wierszy mapy — tam jest otwarte morze na polnoc od Helu.
  spr.fillRect(ox, CY, W, 18, col::BG);

  char hdr[24];
  if (fr.offsetMin >= -1) {
    snprintf(hdr, sizeof(hdr), "teraz");
  } else {
    snprintf(hdr, sizeof(hdr), "%ld min temu", static_cast<long>(-fr.offsetMin));
  }
  viewHeader(spr, ox, hdr);

  // --- os czasu (na dole, na mapie) ---
  const int ty = CY + gmapw::MAP_H - 20;
  spr.fillRect(ox, ty, W, 20, col::BG);

  const int ax0 = ox + 40, ax1 = ox + W - 46;
  spr.drawFastHLine(ax0, ty + 12, ax1 - ax0, col::GRID_HI);

  for (int i = 0; i < n; ++i) {
    const int x = ax0 + (i * (ax1 - ax0)) / (n - 1);
    const bool cur = (i == fi);
    spr.drawFastVLine(x, ty + 8, 8, cur ? col::ACCENT : col::TEXT_MUTE);
    if (cur) {
      spr.fillCircle(x, ty + 5, 3, col::ACCENT);
    }
  }

  gl(spr, "-2h", ox + 14, ty + 8, col::TEXT_MUTE);
  glRight(spr, "teraz", ox + W - 12, ty + 8, col::TEXT_MUTE);

  // Strzalka: czas plynie w prawo. Bez niej kierunek trzeba zgadywac z ruchu chmur,
  // a ten bywa mylacy (patrz: efekt kola wozu w symulacji).
  const int arrX = ax1 - 12;
  spr.drawFastHLine(arrX - 10, ty + 12, 10, col::ACCENT);
  spr.drawLine(arrX, ty + 12, arrX - 4, ty + 9, col::ACCENT);
  spr.drawLine(arrX, ty + 12, arrX - 4, ty + 15, col::ACCENT);

  // godzina biezacej klatki
  if (fr.epoch > 0) {
    const time_t tt = static_cast<time_t>(fr.epoch);
    struct tm tmv{};
    localtime_r(&tt, &tmv);
    char hm[8];
    snprintf(hm, sizeof(hm), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    const int x = ax0 + (fi * (ax1 - ax0)) / (n - 1);
    plCenter(spr, PLF14, hm, x, ty + 4, col::TEXT);
  }
}


// -------------------------------------------------- WIDOK 6: PIEC ------------
// Vitodens 050-W przez chmure ViCare. Piec jest slepy — nie ma czujnika
// zewnetrznego ani krzywej grzewczej — wiec pokazujemy to, co naprawde wie:
// wlasna wode, palnik i zuzycie gazu.

void WeatherUi::drawViewBoiler(TFT_eSPI& spr, int ox, float t) {
  const float e = easeOutCubic(t);
  static const vi::Model kEmptyBoiler{};
  const vi::Model& b = boiler_ ? *boiler_ : kEmptyBoiler;

  if (!b.valid) {
    viewHeader(spr, ox, "brak danych");
    drawNoData(spr, ox, "Piec nie odpowiada",
               diag().viErr[0] ? diag().viErr : "skonfiguruj w panelu");
    return;
  }

  // Od tego miejsca w dol obowiazuje jedna zasada: KAZDA liczba z pieca jest
  // rysowana tylko wtedy, gdy model potwierdza, ze ja dostal. Model oddaje flagi
  // has* wlasnie po to — bez ich czytania ekran pokazywal "0.0°C" jako swiezy
  // pomiar, gdy API nie przyslalo cechy. Zero jest tu nieodroznialne od prawdy,
  // wiec brak odczytu musi wygladac jak brak odczytu: "--".
  char hdr[24];
  if (!b.hasBurnerState) {
    snprintf(hdr, sizeof(hdr), "palnik: brak odczytu");
  } else if (b.burnerActive && b.hasModulation) {
    snprintf(hdr, sizeof(hdr), "palnik %d%%", b.modulationPct);
  } else if (b.burnerActive) {
    snprintf(hdr, sizeof(hdr), "palnik pracuje");
  } else {
    snprintf(hdr, sizeof(hdr), "palnik wyłączony");
  }
  const bool hot = b.hasBurnerState && b.burnerActive;
  viewHeader(spr, ox, hdr, hot ? col::PV_SOLAR : col::TEXT_MUTE,
             hot ? col::PV_SOLAR : col::TEXT_MUTE);

  // --- CIEPLA WODA: najwazniejsza liczba na tym ekranie ---
  gl(spr, "CIEPŁA WODA", ox + 13, 56, col::TEXT_MUTE);
  char v[12];
  snprintf(v, sizeof(v), b.hasDhwTemp ? "%.1f" : "--", b.dhwTempC);
  const int vw = bigStr(spr, &FreeSansBold24pt7b, v, ox + 12, 102,
                        !b.hasDhwTemp        ? col::TEXT_MUTE
                        : b.dhwTempC < 40.f  ? col::PV_HOUSE
                                             : col::PV_IMPORT);
  // Linia bazowa liczby (102), nie indeks gorny (76) — to samo, co przy mocy PV:
  // przy waskiej wartosci ("--" zamiast "53.4") jednostka wjezdzala w etykiete
  // "CIEPŁA WODA" nad soba.
  plStr(spr, PLF18, "°C", ox + 16 + vw, 102, col::TEXT_DIM);

  char sub[40];
  if (b.hasDhwTarget) {
    snprintf(sub, sizeof(sub), "zadana %.0f°C, %s", b.dhwTargetC,
             strcmp(b.dhwMode, "comfort") == 0   ? "komfort"
             : strcmp(b.dhwMode, "eco") == 0     ? "eko"
             : strcmp(b.dhwMode, "off") == 0     ? "wyłączona"
                                                 : b.dhwMode);
  } else {
    snprintf(sub, sizeof(sub), "zadana: brak odczytu");
  }
  plStr(spr, PLF14, sub, ox + 12, 120, col::TEXT_DIM);

  // --- obieg grzewczy (po prawej) ---
  const bool heating = strcmp(b.circuitMode, "heating") == 0;
  glCenter(spr, "OBIEG GRZEWCZY", ox + 250, 56, col::TEXT_MUTE);
  plCenter(spr, PLF18, heating ? "grzeje" : "standby", ox + 250, 82,
           heating ? col::PV_IMPORT : col::TEXT_MUTE);
  if (heating && b.hasCircuitTarget) {
    char cs[20];
    snprintf(cs, sizeof(cs), "nastawa %.0f°C", b.circuitTargetC);
    glCenter(spr, cs, ox + 250, 92, col::TEXT_MUTE);
  } else if (heating) {
    glCenter(spr, "nastawa: brak odczytu", ox + 250, 92, col::TEXT_MUTE);
  } else {
    glCenter(spr, "lato - nie grzeje", ox + 250, 92, col::TEXT_MUTE);
  }
  char sup[24];
  if (b.hasSupplyTemp) {
    snprintf(sup, sizeof(sup), "zasilanie %.1f°C", b.supplyTempC);
  } else {
    snprintf(sup, sizeof(sup), "zasilanie --");
  }
  glCenter(spr, sup, ox + 250, 108, col::TEXT_MUTE);

  // --- 4 kafelki: zuzycie dzis ---
  struct Card {
    const char* label;
    char value[12];
    const char* unit;
    uint16_t color;
  } cards[3];

  // Te trzy kafelki tez musza czytac flagi has*, inaczej jeden ekran mowi uczciwe
  // "--" przy CWU, a kafelek obok drukuje 0.0 m3 jako zmierzone zuzycie gazu.
  cards[0] = {"Gaz dziś", {0}, "m³", b.hasGas ? col::PV_SOLAR : col::TEXT_MUTE};
  snprintf(cards[0].value, sizeof(cards[0].value), b.hasGas ? "%.1f" : "--",
           b.gasDhwM3 + b.gasHeatM3);

  cards[1] = {"Ciepło", {0}, "kWh", b.hasHeat ? col::PV_IMPORT : col::TEXT_MUTE};
  snprintf(cards[1].value, sizeof(cards[1].value), b.hasHeat ? "%.0f" : "--",
           b.heatDhwKwh + b.heatHeatKwh);

  cards[2] = {"Prąd", {0}, "kWh", col::PV_HOUSE};
  snprintf(cards[2].value, sizeof(cards[2].value), "%.1f", b.powerKwh);

  // Licznik uruchomien i sila WiFi pieca — wyrzucone. Ekran ma pokazywac to, co
  // sprawdza sie codziennie, a nie wszystko, co API potrafi oddac.

  const int cy0 = 128, chh = 40;
  for (int i = 0; i < 3; ++i) {
    const int x = ox + 8 + i * 102;
    const int grow = static_cast<int>(chh * clampf(e * 1.3f - i * 0.08f, 0.f, 1.f));
    if (grow < 4) continue;
    spr.fillRoundRect(x, cy0 + (chh - grow), 98, grow, 6, col::BG_CARD);
    if (grow < chh - 2) continue;
    spr.fillRoundRect(x, cy0, 3, chh, 1, cards[i].color);
    plStr(spr, PLF14, cards[i].label, x + 9, cy0 + 15, col::TEXT_DIM);
    const int w2 = pltxt::drawString(spr, PLF18, cards[i].value, x + 9, cy0 + 36,
                                     cards[i].color, cards[i].color);
    gl(spr, cards[i].unit, x + 12 + w2, cy0 + 26, col::TEXT_MUTE);
  }

  drawGasChart(spr, ox, e);

  // Refresh token zyje 180 dni — bez ostrzezenia piec pewnego dnia po prostu
  // znika z ekranu i nikt nie wie dlaczego.
  const int dleft = vi::daysLeft();
  if (dleft >= 0 && dleft < 21) {
    char au[32];
    snprintf(au, sizeof(au), "autoryzacja: %d dni!", dleft);
    plRight(spr, PLF14, au, ox + W - 12, 196, col::ERR);
  }
}

// Profil doby: modulacja palnika. Uklad i skala jak sparkline na ekranie PV —
// ten sam jezyk wizualny dla "ile urzadzenie teraz pracuje".
void WeatherUi::drawGasChart(TFT_eSPI& spr, int ox, float e) {
  static const BurnerHistory kEmpty{};
  const BurnerHistory& h = burner_ ? *burner_ : kEmpty;

  gl(spr, "PRACA PALNIKA DZIŚ", ox + 12, 172, col::TEXT_MUTE);
  glRight(spr, "modulacja %", ox + W - 12, 172, col::TEXT_MUTE);

  const int sx = ox + 12, sy = 184, sw = W - 24, sh = 20;
  spr.fillRoundRect(sx, sy, sw, sh, 3, col::CHART_SPARK_BG);

  const int s0 = 0, s1 = BurnerHistory::SLOTS - 1;   // cala doba
  const int base = sy + sh - 1;

  for (int s = s0; s <= s1; ++s) {
    if (!h.filled[s] || h.mod[s] == 0) continue;
    const int x = sx + ((s - s0) * (sw - 2)) / (s1 - s0);
    int hh = static_cast<int>((h.mod[s] / 100.f) * (sh - 3) * e);
    if (hh < 1) hh = 1;
    // Ten sam zolty co "Gaz dziś" — kolor laczy liczbe z wykresem.
    spr.drawFastVLine(x, base - hh, hh, col::PV_SOLAR);
    spr.drawFastVLine(x + 1, base - hh, hh, col::PV_SOLAR_DK);
  }
  for (int hh = 6; hh <= 18; hh += 6) {
    const int x = sx + ((hh * 6 - s0) * (sw - 2)) / (s1 - s0);
    spr.drawFastVLine(x, sy + sh - 3, 3, col::GRID_HI);
  }
}

// ---------------------------------------------- WIDOK 7: W DOMU (czujniki BLE) --
// Sens tego ekranu nie polega na pokazaniu dwoch liczb — te sa w telefonie.
// Polega na ZESTAWIENIU ich z tym, co na zewnatrz: od razu widac, czy warto
// otworzyc okno, i gdzie robi sie duszno.

void WeatherUi::drawViewHome(TFT_eSPI& spr, int ox, float t, const WeatherModel& w) {
  const float e = easeOutCubic(t);
  static const RoomHistory kEmpty{};
  const RoomHistory& rh = rooms_ ? *rooms_ : kEmpty;
  const bool haveOut = w.current.valid;

  char hdr[28] = {};
  if (haveOut) {
    snprintf(hdr, sizeof(hdr), "na zewnątrz %d°C, %d%%",
             static_cast<int>(lroundf(w.current.tempC)), w.current.humidity);
  }
  viewHeader(spr, ox, hdr);

  // Kolor kafelka = kolor jego linii na wykresie. TO JEST CALA LEGENDA — dlatego
  // pod wykresem nie ma juz zadnego napisu. Kolejnosc musi zostac zgodna z
  // RoomHistory::ROOMS, bo indeksem jest slot z Settings.
  const uint16_t roomCol[RoomHistory::ROOMS] = {col::ACCENT,     col::PV_SOLAR,
                                                col::PV_EXPORT,  col::PV_IMPORT,
                                                col::STORM,      col::HUMID};

  struct Room {
    const char* name;
    float tempC;
    float hum;
    bool hasTemp;
    bool hasHum;
    int slot;         // indeks w Settings — po nim idzie historia i kolor
    uint32_t ageS;
    int rssiBest;     // sygnal LEPSZEGO ze zrodel, 0 = nikt go nie slyszy
    bool viaGw;       // true = to bramka slyszy go lepiej (litera S), false = ESP (E)
  } rooms[RoomHistory::ROOMS];
  int n = 0;

  for (int i = 0; i < ble::count() && n < RoomHistory::ROOMS; ++i) {
    const ble::Sensor s = ble::get(i);
    if (!s.valid) continue;
    const Settings::BleCfg* cfg = settings().bleFind(s.mac);
    if (cfg == nullptr) continue;

    // Sloty 6-7 z Settings nie maja ani koloru, ani miejsca w historii — pomijamy
    // je swiadomie zamiast wyjechac poza tablice.
    int slot = -1;
    for (int k = 0; k < RoomHistory::ROOMS; ++k) {
      if (&settings().ble[k] == cfg) slot = k;
    }
    if (slot < 0) continue;

    // Wybieramy LEPSZE ze zrodel, a nie ostatni zapis. Pole `s.rssi` niesie to,
    // co przyszlo ostatnie — dlatego Schody potrafily pokazywac -90 z wlasnego
    // radia, choc bramka slyszy je z -56. Odczyt (temperatura) jest identyczny
    // z obu zrodel, wiec wybor dotyczy wylacznie tego, ktora liczbe pokazac.
    // Zrodlo starsze niz 90 s nie liczy sie w ogole: lepszy slaby sygnal teraz
    // niz swietny sprzed pol godziny.
    const bool ownFresh = s.rssiOwn != 0 && s.ownAt != 0 && (millis() - s.ownAt) < 90000;
    const bool gwFresh = s.rssiGw != 0 && s.gwAt != 0 && (millis() - s.gwAt) < 90000;
    int best = 0;
    bool bestGw = false;
    if (ownFresh && gwFresh) {
      bestGw = s.rssiGw > s.rssiOwn;
      best = bestGw ? s.rssiGw : s.rssiOwn;
    } else if (ownFresh) {
      best = s.rssiOwn;
    } else if (gwFresh) {
      best = s.rssiGw;
      bestGw = true;
    }

    rooms[n] = {cfg->name[0] ? cfg->name : s.mac, s.tempC, s.humidity, s.hasTemp,
                s.hasHum, slot, s.seenAt ? (millis() - s.seenAt) / 1000 : 9999,
                best, bestGw};
    ++n;
  }

  if (n == 0) {
    drawNoData(spr, ox, "Brak czujników", "dodaj je w panelu urządzenia");
    return;
  }

  // ------------------------------------------------------------- kafelki ------
  // Uklad zalezy od liczby czujnikow. Zmierzone: przy 4 kafelkach w jednym rzedzie
  // kazdy ma 69 px, a sama liczba "23.8" w duzej czcionce potrzebuje ~90 px.
  // Wiec: 1-2 czujniki -> rzad z wielka liczba, 3-4 -> siatka 2x2 mniejsza czcionka.
  // 5-6 czujnikow -> trzy kolumny (`tight`). Kafelek chudnie ze 146 do 94 px i wtedy
  // KOMPLET czterech elementow po prostu nie wchodzi - zmierzone na tablicach glifow:
  // sygnal musialby stanac na linii nazwy, a "100E" nawet w PlFont10 startuje tam na
  // x+59, podczas gdy sama "Lazienka" (PLF14) konczy sie na x+67. To 8 px jedno na
  // drugim przy KROTKIEJ nazwie; "Pokoj dziecka" konczy sie na x+97, czyli 38 px za
  // duzo. Nie ratuje tego ani zdjecie minusa, ani zmniejszenie nazwy do PlFont10.
  // Dlatego przy `tight` kafelek ma TRZY elementy: nazwa, temperatura i wilgotnosc
  // (PlFont10, zapas 2 px w najgorszym przypadku "-19.8" + "100%"), a sygnal znika.
  // Znika sygnal, a nie wilgotnosc, bo wilgotnosc to tresc ekranu, a sygnal
  // diagnostyka. Znika tez jednostka - patrz nizej. Sciezka jest dzis MARTWA
  // (4 czujniki -> 2 kolumny, cw=146) i zapali sie dopiero przy piatym czujniku.
  const bool grid = n > 2;
  const int cols = grid ? (n > 4 ? 3 : 2) : n;
  const int gap = 8;
  const int cw = (W - 20 - (cols - 1) * gap) / cols;
  const int ch = grid ? 38 : 80;
  const int cy = 54;
  const bool tight = cols == 3;

  for (int i = 0; i < n; ++i) {
    const Room& r = rooms[i];
    const int cxi = i % cols, cyi = i / cols;
    const int x = ox + 10 + cxi * (cw + gap);
    const int y = cy + cyi * (ch + 6);
    const uint16_t rc = roomCol[r.slot];

    const int grow = static_cast<int>(ch * clampf(e * 1.6f - i * 0.14f, 0.f, 1.f));
    if (grow < 6) continue;
    spr.fillRoundRect(x, y + (ch - grow), cw, grow, 8, col::BG_CARD);
    if (grow < ch - 2) continue;

    const bool stale = r.ageS > 900;   // 15 minut ciszy = dane nieaktualne
    spr.fillRoundRect(x, y, 3, ch, 1, stale ? col::TEXT_MUTE : rc);
    plStr(spr, PLF14, r.name, x + 10, y + 15, stale ? col::TEXT_MUTE : col::TEXT);

    // PRAWA KOLUMNA KAFELKA: na gorze SYGNAL (maly), pod nim WILGOTNOSC.
    // Wilgotnosc jest trescia - po nia sie na ten ekran patrzy. Sygnal jest
    // diagnostyka: mowi, czy czujnik zaraz zamilknie, a litera - ktore z dwoch uszu
    // go slyszy. E = wlasne radio ESP, S = bramka Shelly. To NIE jest to samo
    // miejsce: lustro w lazience tlumi radio ESP tak, ze Schody slychac z -90,
    // a z Shelly z -56. Bez tej litery nie wiadomo, czyj to pomiar ani co poprawilo
    // przestawienie bramki. Dlatego sygnal zostaje, ale schodzi na drugi plan.
    //
    // PlFont10 jest wymuszony przez NOWA pozycje, a nie ratuje starej kolizji.
    // Uwaga na pokuse dopisania tu, ze "naprawia blad": w poprzedniej wersji sygnal
    // stal na wlasnym wierszu (baseline y+32, czyli piksele y+18..y+31), a nazwa na
    // y+4..y+17: nachodzily sie w X o 13 px, ale w Y wcale, wiec NIC sie nie stykalo.
    // Dopiero przeniesienie sygnalu NA wiersz nazwy tworzy ryzyko: w PLF18 "-100E"
    // ma 52 px i startowalby na x+84, wjezdzajac w kazda nazwe dluzsza niz 74 px.
    // W PlFont10 ma 29 px, startuje na x+107 i zostawia 10 px zapasu nawet dla
    // "Pokoj dziecka" (87 px). Minus zostaje w kazdym ukladzie - nie ma po co go scinac.
    const int rs = r.rssiBest;
    char sg[12];
    if (rs == 0) {
      snprintf(sg, sizeof(sg), "--");
    } else {
      snprintf(sg, sizeof(sg), "%d%c", rs, r.viaGw ? 'S' : 'E');
    }
    // Progi jak w narzedziu do rozstawiania czujnikow: > -80 dobrze, > -88 slabo,
    // nizej na granicy.
    const uint16_t sc = rs == 0    ? col::TEXT_MUTE
                        : rs > -80 ? col::OK
                        : rs > -88 ? col::WARN
                                   : col::ERR;
    if (!tight) {
      plRight(spr, pltxt::font10(), sg, x + cw - 10, y + (grid ? 15 : 10), sc);
    }

    // Wilgotnosc - o stopien mniejsza od temperatury: PLF14 pod PLF18 w siatce.
    //
    // Baseline zalezy od ukladu i to nie jest kosmetyka:
    // * siatka (ch=38): y+32, czyli linia bazowa temperatury. Sygnal siedzi wyzej,
    //   na linii nazwy (y+15). Najciasniej jest przy "-19.8" + "100%": blok
    //   temperatury z jednostka konczy sie na x+69, wilgotnosc startuje na x+100,
    //   czyli 31 px zapasu.
    // * kafelek 80 px (ch=80): NIE ma linii temperatury do dzielenia. Wielka
    //   czcionka zjada caly wiersz - samo "-19.8" to 106 px ze 126 px tresci, wiec
    //   cokolwiek po prawej wjechaloby na cyfry (zmierzone: -36 px). Zamiast tego
    //   wilgotnosc siada pod sygnalem, w wolnym pasie: sygnal zajmuje wiersze
    //   y+2..y+10, wilgotnosc y+13..y+23, a wielkie cyfry zaczynaja sie dopiero na
    //   y+25. Waskim gardlem jest wtedy nazwa: "Pokoj dziecka" konczy sie na x+97,
    //   "100%" startuje na x+100, czyli 3 px. Nazwa dluzsza niz 90 px (np.
    //   "Lazienka Gora" = 93 px) wjedzie na wilgotnosc; nazw nikt nigdzie nie
    //   przycina.
    char hs[10];
    snprintf(hs, sizeof(hs), r.hasHum ? "%.0f%%" : "--", r.hum);
    const uint16_t hc = !r.hasHum                        ? col::TEXT_MUTE
                        : (r.hum > 65.f || r.hum < 30.f) ? col::WARN
                                                         : col::PV_HOUSE;
    plRight(spr, tight ? pltxt::font10() : PLF14, hs, x + cw - 10, y + (grid ? 32 : 23),
            hc);

    char v[12];
    snprintf(v, sizeof(v), r.hasTemp ? "%.1f" : "--", r.tempC);
    const uint16_t tc = r.hasTemp && !stale ? tempColor(r.tempC) : col::TEXT_MUTE;

    if (grid) {
      // ciasno — temperatura w PLF18 zamiast wielkiej czcionki
      const int vw = pltxt::drawString(spr, PLF18, v, x + 10, y + 32, tc, tc);
      // Przy trzech kolumnach znika jednostka. To tu najtanszy piksel do oddania:
      // ekran nazywa sie "W DOMU", a wszystko na nim to temperatury pokoi w stopniach
      // Celsjusza. Jednostka powtarzalaby tytul ekranu szesc razy pod rzad, a kosztuje
      // 15 px: z nia blok temperatury konczy sie na x+69 (3 px przerwy + 12 px znaku
      // stopnia w PlFont10) i wjezdza na wilgotnosc, ktora startuje na x+56; bez niej
      // temperatura konczy sie na x+54 i zostaja 2 px zapasu. Przy dwoch kolumnach
      // (cw=146) miejsca jest dosc i jednostka zostaje.
      if (!tight) {
        gl(spr, "°C", x + 13 + vw, y + 22, col::TEXT_DIM);
      }
    } else {
      const int vw = bigStr(spr, &FreeSansBold24pt7b, v, x + 10, y + 58, tc);
      plStr(spr, PLF14, "°C", x + 14 + vw, y + 40, col::TEXT_DIM);
      if (stale) {
        plStr(spr, PLF14, "brak łączności", x + 10, y + 74, col::TEXT_MUTE);
      } else if (haveOut && r.hasTemp) {
        const float d = r.tempC - w.current.tempC;
        char ds[28];
        snprintf(ds, sizeof(ds), d >= 0 ? "cieplej o %.1f°" : "chłodniej o %.1f°",
                 d >= 0 ? d : -d);
        plStr(spr, PLF14, ds, x + 10, y + 74, d > 0 ? col::PV_IMPORT : col::PV_HOUSE);
      }
    }
  }

  // ------------------------------------------------- wykres z ostatnich 24 h ---
  // SAMA TEMPERATURA. Wilgotnosc byla tu druga linia na kazdy pokoj — przy czterech
  // czujnikach osiem linii na 26 px wysokosci. Zeby to w ogole rozroznic, trzeba bylo
  // legendy "GRUBA = TEMPERATURA / CIENKA = WILGOTNOSC", ktora nie miala gdzie stanac
  // i wlazila na ramke wykresu. Usuniecie serii rozwiazuje jedno i drugie: legenda
  // jest zbedna (kolor paska kafelka = kolor linii), a wykres urosl z 26 do 42 px
  // i odzyskal 22 px szerokosci po osi procentow.
  const int gx0 = ox + 32, gx1 = ox + W - 12;
  const int gy0 = 148, gy1 = 190;

  float tMin = 1e9f, tMax = -1e9f;
  bool any = false;
  for (int i = 0; i < n; ++i) {
    for (int k = 0; k < RoomHistory::SLOTS; ++k) {
      const int16_t v = rh.t10[rooms[i].slot][rh.idx(k)];
      if (v == RoomHistory::NO_T) continue;
      const float f = v / 10.f;
      if (f < tMin) tMin = f;
      if (f > tMax) tMax = f;
      any = true;
    }
  }

  spr.fillRoundRect(gx0 - 2, gy0 - 4, gx1 - gx0 + 4, gy1 - gy0 + 8, 3, col::CHART_SPARK_BG);

  if (!any) {
    glCenter(spr, "ZBIERAM HISTORIE...", (gx0 + gx1) / 2, gy0 + 20, col::TEXT_MUTE);
    return;
  }
  if (tMax - tMin < 2.f) {  // plaska historia — nie rozciagamy szumu na caly wykres
    const float mid = (tMin + tMax) / 2.f;
    tMin = mid - 1.f;
    tMax = mid + 1.f;
  }

  auto yT = [&](float v) {
    return gy1 - static_cast<int>(clampf((v - tMin) / (tMax - tMin), 0.f, 1.f) *
                                  (gy1 - gy0));
  };
  auto xAt = [&](int k) {
    return gx0 + (k * (gx1 - gx0)) / (RoomHistory::SLOTS - 1);
  };

  // siatka co 6 h
  for (int hh = 6; hh <= 18; hh += 6) {
    const int x = xAt(RoomHistory::SLOTS - 1 - hh * 6);
    spr.drawFastVLine(x, gy0, gy1 - gy0, col::GRID);
  }
  // ...i linia w polowie skali. Przy czterech liniach w zakresie ~3°C samo min/max
  // nie wystarcza, zeby ocenic o ile pokoje sie roznia.
  const int yMid = (gy0 + gy1) / 2;
  spr.drawFastHLine(gx0, yMid, gx1 - gx0, col::GRID);

  // --- linie ---
  for (int i = 0; i < n; ++i) {
    const int r = rooms[i].slot;
    const uint16_t rc = roomCol[r];

    int px = -1, pyT = 0;
    int lastX = -1, lastY = 0;
    for (int k = 0; k < RoomHistory::SLOTS; ++k) {
      const int16_t tv = rh.t10[r][rh.idx(k)];
      if (tv == RoomHistory::NO_T) {
        px = -1;  // dziura w danych (urzadzenie nie dzialalo) — nie laczymy przez nia
        continue;
      }
      const int x = xAt(k);
      const int cyT = yT(tv / 10.f);
      if (px >= 0) {
        spr.drawLine(px, pyT, x, cyT, rc);  // 1 px - przy 2 px linie zlewaly sie w plame
      }
      px = x;
      pyT = cyT;
      lastX = x;
      lastY = cyT;
    }
    // Kropka na koncu linii — kotwiczy "ta barwa = ten kafelek" w miejscu, w ktorym
    // linie sa najgestsze, czyli przy prawej krawedzi. Promien 1, nie 2: przy linii
    // 2 px kropka o promieniu 2 miala 5 px srednicy, czyli 2,5x linii. Przy linii
    // 1 px ta sama kropka bylaby 5x szersza od niej i z kotwicy zrobilaby sie plama
    // - a przy czterech pokojach kropki spotykaja sie tu obok siebie. Promien 1 daje
    // 3 px: nadal wyraznie grubsza od linii, wiec kotwica trzyma.
    if (lastX >= 0) spr.fillCircle(lastX, lastY, 1, rc);
  }

  // --- os temperatury ---
  // PlFont, nie GLCD — znak stopnia. CZWARTY raz ta sama pulapka; komentarz zostaje
  // jako ostrzezenie: cokolwiek ma "°", "ą", "ł" — NIE moze isc przez gl()/glRight().
  // "%.0f", nie "%.1f": na os zostaje 27 px (do gx0-5), a "24.5°" w PLF14 ma ~38 px
  // i wyjechaloby poza lewa krawedz. Wykres sluzy do POROWNANIA pokoi, dokladne
  // wartosci sa na kafelkach wyzej — grubsza podzialka niczego tu nie kosztuje.
  char ax[10];
  snprintf(ax, sizeof(ax), "%.0f°", tMax);
  plRight(spr, PLF14, ax, gx0 - 5, gy0 + 4, col::TEXT_MUTE);
  snprintf(ax, sizeof(ax), "%.0f°", (tMin + tMax) / 2.f);
  plRight(spr, PLF14, ax, gx0 - 5, yMid + 5, col::TEXT_MUTE);
  snprintf(ax, sizeof(ax), "%.0f°", tMin);
  plRight(spr, PLF14, ax, gx0 - 5, gy1, col::TEXT_MUTE);

  gl(spr, "-24h", gx0 - 2, gy1 + 5, col::TEXT_MUTE);
  glCenter(spr, "-12h", xAt(RoomHistory::SLOTS - 1 - 72), gy1 + 5, col::TEXT_MUTE);
  glRight(spr, "teraz", gx1 + 2, gy1 + 5, col::TEXT_MUTE);
}

void WeatherUi::drawViewStats(TFT_eSPI& spr, int ox, float t, uint32_t nowMs,
                              uint32_t heapNow) {
  const float e = easeOutCubic(t);
  const Diag& d = diag();
  // NIE millis()/getFreeHeap() — te wartości muszą być identyczne we wszystkich
  // pasach i paskach tej samej klatki, inaczej wiersz przecięty granicą pasa
  // (np. "Samoloty" na y=103, "MQTT" na granicy paska zrzutu) rwie się w pół.
  const uint32_t now = nowMs;

  char b[48];
  if (otaTrialActive()) {
    // Okres próbny: wersja jeszcze nie potwierdziła, że działa. Jeśli w ciągu
    // 3 minut nie udowodni (WiFi + dane + sterta), urządzenie samo się cofnie.
    snprintf(b, sizeof(b), "v%d - próbna", FW_VERSION);
    viewHeader(spr, ox, b, col::WARN);
  } else if (d.otaRemote > FW_VERSION) {
    snprintf(b, sizeof(b), "v%d -> v%d", FW_VERSION, d.otaRemote);
    viewHeader(spr, ox, b, col::WARN);
  } else {
    snprintf(b, sizeof(b), "v%d", FW_VERSION);
    viewHeader(spr, ox, b, col::OK);
  }

  // --- źródła danych: zielona kropka = działa, czerwona = błąd, szara = wyłączone ---
  // "off" = stan neutralny, NIE awaria: falownik śpi po zachodzie (Modbus wyłączony),
  // MQTT jest świadomie wyłączony przez użytkownika. W obu razach kropka szara.
  // Bramki: JEDEN wiersz na cala liste. Na trzy osobne nie ma miejsca — src[8] to
  // dokladnie 4 wiersze x 2 kolumny konczace sie na y=120, a karty startuja na 128.
  // Liczba zywych idzie w STATUS, nie w nazwe: przy nazwie "Bramka" zostaje
  // zmierzone 76 px, a "Bramki 2/3" zjadloby ~28 px i przycieloby "nie odpowiada".
  char gwSt[16] = "";
  const int gwCfg = blegw::configured();
  const int gwOn = blegw::online();
  if (gwCfg > 1 && gwOn < gwCfg) {
    snprintf(gwSt, sizeof(gwSt), "%d z %d żyje", gwOn, gwCfg);
  } else if (gwCfg > 0) {
    snprintf(gwSt, sizeof(gwSt), "%s", blegw::lastError());
  }

  struct Src {
    const char* name;
    uint32_t okAt;
    const char* err;
    bool off;
    const char* offMsg;
  } src[8] = {
      // Kolejnosc jest wierszami: {lewa, prawa}, {lewa, prawa}...
      {"Pogoda", d.weatherOkAt, d.weatherErr, false, ""},
      {"Radar", d.radarOkAt, d.radarErr, false, ""},

      {"Falownik", d.pvOkAt, d.pvErr, d.pvAsleep, "uśpiony"},
      // Piec byl mierzony od poczatku (viOkAt/viErr) i wystawiany w /api/state,
      // ale nikt go tu nie rysowal.
      {"Piec", d.viOkAt, d.viErr, !settings().hasViessmann(), "wyłączony"},

      {"Samoloty", d.flightOkAt, d.flightErr, false, ""},
      {"MQTT", d.mqttOkAt, d.mqttErr, !settings().hasMqtt(), "wyłączony"},

      // configured(), nie "bleGwHost[0] == 0" — poprawne takze wtedy, gdyby lista
      // kiedys przestala byc zageszczana.
      {gwCfg > 1 ? "Bramki" : "Bramka", blegw::lastOkAt(), gwSt, gwCfg == 0, "wyłączona"},
      // OTA: err zawsze puste. otaMsg NIE nadaje sie na blad — tym samym kanalem
      // leci postep ("Pobieram nową wersję"), wiec czerwona kropka zapalalaby sie
      // w trakcie poprawnej aktualizacji. Tu wystarczy wiek ostatniego sprawdzenia.
      {"OTA", d.otaOkAt, "", false, ""},
  };

  // Dwie kolumny po 148 px zamiast jednej na 300 — w jednym rzedzie "Pogoda" i
  // "OK 8 min temu" zostawialy w srodku ~150 px pustki. 4 wiersze po 17 px koncza
  // sie na y=120, karty zaczynaja sie na 128.
  const int cellW = (W - 24) / 2;
  for (int i = 0; i < static_cast<int>(sizeof(src) / sizeof(src[0])); ++i) {
    const int cx = ox + 8 + (i % 2) * (cellW + 8);
    const int y0 = 52 + (i / 2) * 17;
    if (e < (i + 1) * 0.07f) continue;

    const bool off = src[i].off;
    const bool bad = !off && src[i].err[0] != '\0';
    const bool never = !off && src[i].okAt == 0;
    const uint16_t dot = off ? col::TEXT_MUTE : (bad ? col::ERR : (never ? col::WARN : col::OK));

    spr.fillCircle(cx + 5, y0 + 6, 4, dot);
    const int nameW =
        pltxt::drawString(spr, PLF14, src[i].name, cx + 15, y0 + 11,
                          off ? col::TEXT_MUTE : col::TEXT,
                          off ? col::TEXT_MUTE : col::TEXT);

    // Ile miejsca zostalo na status PO nazwie. Bez tego dluzszy blad ("brak
    // polaczenia") wchodzil w nazwe — na 148 px nie ma marginesu na zgadywanie.
    const int avail = cellW - 19 - nameW - 4;

    const char* txt = nullptr;
    uint16_t tc = col::TEXT_DIM;
    if (off) {
      txt = src[i].offMsg;
      tc = col::TEXT_MUTE;
    } else if (bad) {
      txt = src[i].err;
      tc = col::ERR;
    } else if (never) {
      txt = "czekam...";
      tc = col::TEXT_MUTE;
    } else {
      // Bez "OK" na przedzie. Zmierzone na tablicach glifow: "OK 8 min temu" = 72 px,
      // a przy "Falowniku" zostaje 69, przy "Samolotach" 66 — wiec kazdy taki wiersz
      // bylby przyciety. Zielona kropka obok i tak juz mowi "OK"; wartosc niesie wiek.
      // Powyzej 90 minut przechodzimy na godziny, inaczej "120 min temu" (65 px)
      // ociera sie o limit 66 px.
      const uint32_t ago = (now - src[i].okAt) / 1000;
      if (ago < 90) {
        snprintf(b, sizeof(b), "%lus temu", static_cast<unsigned long>(ago));
      } else if (ago < 5400) {
        snprintf(b, sizeof(b), "%lu min temu", static_cast<unsigned long>(ago / 60));
      } else {
        snprintf(b, sizeof(b), "%lu h temu", static_cast<unsigned long>(ago / 3600));
      }
      txt = b;
    }

    // Status idzie przez gl() = PlFont10. Od v81 ten font ZNA polskie znaki, wiec
    // "wyłączony" i "uśpiony" nie potrzebuja juz wiekszego PLF14.
    // Przycinanie to siatka bezpieczenstwa dla bledow z sieci — te potrafia miec
    // dowolna dlugosc ("HTTP 500 Internal Server Error"). Znacznik urwania to "..",
    // a NIE "…" — PlFont10 nie ma glifu U+2026, wiec wielokropek zniknalby po cichu
    // (glyphIndex zwraca -1, znak jest pomijany) i nie bylo by widac, ze tekst urwano.
    char cut[52];
    snprintf(cut, sizeof(cut), "%s", txt);
    if (pltxt::stringWidth(pltxt::font10(), cut) > avail) {
      size_t len = strlen(cut);
      while (len > 1) {
        cut[--len] = '\0';
        char probe[56];
        snprintf(probe, sizeof(probe), "%s..", cut);
        if (pltxt::stringWidth(pltxt::font10(), probe) <= avail) {
          snprintf(cut, sizeof(cut), "%s", probe);
          break;
        }
      }
    }
    glRight(spr, cut, cx + cellW - 4, y0 + 3, tc);
  }

  // --- zdrowie: temperatura / RAM / czas pracy ---
  const uint32_t up = now / 1000;
  const uint32_t heap = heapNow;   // złapany raz na klatkę u wołającego
  const uint32_t minHeap = d.minHeap == 0xFFFFFFFF ? heap : d.minHeap;

  struct Card {
    const char* label;
    char value[14];
    char sub[20];
    uint16_t color;
  } cards[3];

  cards[0] = {"TEMPERATURA", {0}, {0},
              cpuTempC_ >= cfg::CPU_T_WARN
                  ? col::ERR
                  : (cpuTempC_ >= cfg::CPU_T_OK ? col::WARN : col::OK)};
  snprintf(cards[0].value, sizeof(cards[0].value), "%.0f°C", cpuTempC_);
  // Bez "ń" — podpisy kart rysuje GLCD, który nie zna polskich znaków.
  snprintf(cards[0].sub, sizeof(cards[0].sub), "procesor");

  // Kolor OPISUJE TE LICZBE, KTORA KARTA POKAZUJE, a karta pokazuje `heap`, czyli
  // sterte biezaca. Liczenie koloru z minHeap dawalo czerwona karte z zielona
  // wartoscia "150 kB" i bylo nie do obronienia odkad minHeap to dolek DOZYWOTNI
  // (ESP.getMinFreeHeap(), zmierzone na urzadzeniu 22044 B < HEAP_DANGER = 25000):
  // karta swiecilaby na czerwono do konca swiata, niezaleznie od tego, co pokazuje.
  // Progi HEAP_* opisuja sterte biezaca, patrz systemHealth(). Dolek zostaje tam,
  // gdzie jest uczciwy i niczego nie przefarbowuje: biala kreska na wskazniku obok.
  cards[1] = {"WOLNY SRAM", {0}, {0},
              heap < cfg::HEAP_DANGER
                  ? col::ERR
                  : (heap < cfg::HEAP_WARN ? col::WARN : col::OK)};
  snprintf(cards[1].value, sizeof(cards[1].value), "%lu kB",
           static_cast<unsigned long>(heap / 1024));
  // Pod spodem PSRAM — od v50 to on dzwiga bufor ekranu i dekoder radaru.
  if (ESP.getPsramSize() > 0) {
    // "PSRAM 1893 kB" wchodzilo pod wskaznik — skracamy do megabajtow.
    snprintf(cards[1].sub, sizeof(cards[1].sub), "PSRAM %.1f MB",
             ESP.getFreePsram() / 1048576.f);
  } else {
    snprintf(cards[1].sub, sizeof(cards[1].sub), "min %lu kB",
             static_cast<unsigned long>(minHeap / 1024));
  }

  // Powód ostatniego resetu wprost na ekranie — dotąd nie było wiadomo, czy
  // urządzenie się wywala, czy po prostu ktoś wyjął wtyczkę.
  cards[2] = {"CZAS PRACY", {0}, {0}, resetWasCrash() ? col::ERR : col::TEXT};
  if (up < 3600) {
    snprintf(cards[2].value, sizeof(cards[2].value), "%lu min",
             static_cast<unsigned long>(up / 60));
  } else if (up < 86400) {
    snprintf(cards[2].value, sizeof(cards[2].value), "%lu godz",
             static_cast<unsigned long>(up / 3600));
  } else {
    snprintf(cards[2].value, sizeof(cards[2].value), "%lu dni",
             static_cast<unsigned long>(up / 86400));
  }
  snprintf(cards[2].sub, sizeof(cards[2].sub), "po: %s",
           resetReasonShort(d.resetReason));

  // Karty: 100 px szerokosci z 4 px przerwy ZLEWALY sie w jeden pas — na ciemnym tle
  // 4 px to mniej niz promien zaokraglenia rogu, wiec granicy po prostu nie bylo
  // widac. Teraz 96 px z przerwa 10 px, i wyzsze o 6 px po odzyskaniu linii z dolu.
  const int cw = 96, cgap = 10;
  const int cy0 = 128, chh = 50;
  for (int i = 0; i < 3; ++i) {
    const int x = ox + 7 + i * (cw + cgap);
    const int grow = static_cast<int>(chh * clampf(e * 1.8f - 0.3f - i * 0.15f, 0.f, 1.f));
    if (grow < 5) continue;
    spr.fillRoundRect(x, cy0 + (chh - grow), cw, grow, 6, col::BG_CARD);
    if (grow < chh - 2) continue;
    spr.fillRoundRect(x, cy0, 3, chh, 1, cards[i].color);
    gl(spr, cards[i].label, x + 9, cy0 + 6, col::TEXT_MUTE);
    plStr(spr, PLF18, cards[i].value, x + 9, cy0 + 34, cards[i].color);
    gl(spr, cards[i].sub, x + 9, cy0 + 38, col::TEXT_MUTE);

    // wskaźnik po prawej stronie kafelka (temperatura / sterta)
    const int gx = x + 78, gy = cy0 + 8, gw = 11, gh = 34;
    if (i == 0) {
      // Strefy wg noty katalogowej ESP32-S3. Biała kreska = 85 °C, czyli granica
      // zalecanej pracy — koniec skali to Tj max 125 °C, a nie "czerwono".
      const GaugeZone z[3] = {
          {cfg::CPU_T_OK, col::OK}, {cfg::CPU_T_WARN, col::WARN}, {cfg::CPU_T_MAX, col::ERR}};
      zoneGauge(spr, gx, gy, gw, gh, cpuTempC_, cfg::CPU_T_MIN, cfg::CPU_T_MAX, z, 3,
                cfg::CPU_T_SPEC, true);
    } else if (i == 1) {
      // Tu odwrotnie niż przy temperaturze: mało = źle. Czerwony pas na DOLE to
      // strefa, w której radar nie zdekoduje PNG, a TLS zaczyna się dławić.
      // Biała kreska = minimum, jakie sterta osiągnęła od startu (najgorszy moment).
      const GaugeZone z[3] = {{static_cast<float>(cfg::HEAP_DANGER), col::ERR},
                              {static_cast<float>(cfg::HEAP_WARN), col::WARN},
                              {static_cast<float>(cfg::HEAP_FULL), col::OK}};
      zoneGauge(spr, gx, gy, gw, gh, static_cast<float>(heap), 0.f,
                static_cast<float>(cfg::HEAP_FULL), z, 3, static_cast<float>(minHeap), true);
    }
  }

  // --- siec: JEDNA linia zamiast dwoch ---
  // Bylo: paski + SSID na y=190, adres IP osobno na y=205, a napis "panel" wciskany
  // miedzy nie na y=197 — trzy rzeczy w dwoch liniach, ktore na siebie nachodzily.
  // Teraz wszystko na jednej linii bazowej: paski i SSID po lewej, panel i IP po
  // prawej. Zwolniona linia poszla do kart wyzej (44 -> 50 px).
  const int rssi = WiFi.RSSI();
  const int bars = rssi >= -55 ? 4 : (rssi >= -65 ? 3 : (rssi >= -75 ? 2 : (rssi >= -85 ? 1 : 0)));
  const int netY = 201;   // wspolna linia bazowa
  for (int i = 0; i < 4; ++i) {
    const int bh = 4 + i * 3;
    spr.fillRect(ox + 10 + i * 6, netY - 1 - bh, 4, bh, i < bars ? col::ACCENT : col::PV_TRACK);
  }

  // Adres BEZ "http://" — to 44 px, ktore odbieraly miejsce nazwie sieci, a nikt
  // nie wpisuje schematu z ekranu. Podpis "panel" mowi, czym ten adres jest.
  char ip[20];
  snprintf(ip, sizeof(ip), "%s", WiFi.localIP().toString().c_str());
  const int ipW = pltxt::stringWidth(PLF14, ip);
  plRight(spr, PLF14, ip, ox + W - 10, netY, col::ACCENT);
  // netY-10, NIE netY-2: glRight() kotwiczy GORE i sam dodaje PlFont10Ascent = 10,
  // wiec argument y=netY-2 dawal linie bazowa 209. Glify "panel" siegaja od -8 do +2
  // wzgledem bazowej (litera "p" schodzi pod linie), czyli realnie y = 201..211,
  // a obszar tresci konczy sie na 205. Widocznych bylo 5 z 11 px. Teraz bazowa
  // wypada na 201, glify na 193..203, i podpis faktycznie stoi na tej samej linii
  // bazowej co adres IP obok: tak jak od poczatku twierdzil komentarz nizej.
  glRight(spr, "panel", ox + W - 14 - ipW, netY - 10, col::TEXT_MUTE);

  // SSID dostaje to, co zostalo — i jest przycinany, a nie nachodzi na adres.
  // Nazwa sieci moze byc dowolnie dluga, wiec bez pomiaru to tylko kwestia czasu.
  snprintf(b, sizeof(b), "%s  %d dBm", WiFi.SSID().c_str(), rssi);
  const int ssidMax = (ox + W - 14 - ipW - pltxt::stringWidth(pltxt::font10(), "panel") - 8) -
                      (ox + 40);
  // Ciecie CALYMI ZNAKAMI, nie bajtami. SSID moze legalnie miec polskie znaki, a te
  // zajmuja w UTF-8 po dwa bajty. Urwanie samego ogona zostawialo osierocony bajt
  // wiodacy (z dwubajtowej sekwencji C5 82 zostawalo samo C5), a wtedy
  // pltxt::decodeUtf8 bierze NASTEPNY bajt jako kontynuacje: zjada '\0', przeskakuje
  // terminator i czyta pamiec za buforem, az trafi na przypadkowe zero.
  // Warunek patrzy na b[n], czyli na bajt, NA KTORYM tniemy: dopoki jest to bajt
  // kontynuacji (10xxxxxx), cofamy sie do bajtu wiodacego i dopiero tam stawiamy '\0'.
  // (Sprawdzanie b[n-1] cofaloby o jeden za duzo: zjadaloby poprawny znak i mimo to
  // zostawialo osierocony bajt wiodacy, czyli dokladnie ten blad, ktory naprawiamy.)
  while (b[0] != '\0' && pltxt::stringWidth(PLF14, b) > ssidMax) {
    size_t n = strlen(b) - 1;
    while (n > 0 && (static_cast<uint8_t>(b[n]) & 0xC0) == 0x80) --n;
    b[n] = '\0';
  }
  plStr(spr, PLF14, b, ox + 40, netY, col::TEXT_DIM);
}

// ------------------------------------- ZRZUT EKRANU DO PRZEGLĄDARKI ----------
// BMP 320x240 24-bit, wysyłany wiersz po wierszu — w RAM-ie trzymamy tylko
// jedną linię (960 B), a nie cały obraz (230 kB).
//
// Po przejściu na dwa pasy bufor wyświetlacza trzyma tylko połowę obrazu naraz,
// więc nie da się już go po prostu odczytać. Zamiast tego zrzut RYSUJE ekran od nowa
// do własnego, wąskiego sprite'a (320x24 = 15 kB) — pasek po pasku, od dołu, tak jak
// idzie BMP. Dzięki temu:
//   - nie dotykamy bufora wyświetlacza, więc obraz na TFT dalej płynie (zrzut leci
//     z zadania web na rdzeniu 0, rysowanie z loop() na rdzeniu 1),
//   - stopka PV (y=206..239), która nigdy nie była w buforze, po prostu wpada w
//     ostatnie paski.
// Koszt: 10 przebiegów rysowania na jeden zrzut, ale każdy z nich jest w większości
// przycinany "za darmo" przez viewport.

// Podglad w przegladarce: wymuszenie konkretnego ekranu. idx < 0 wraca do rotacji.
void WeatherUi::prevView() {
  int v = view_;
  for (int i = 0; i < cfg::VIEW_COUNT; ++i) {
    v = (v - 1 + cfg::VIEW_COUNT) % cfg::VIEW_COUNT;
    const bool skipped = (v == cfg::VIEW_RADAR && !radarmap::hasRain()) ||
                         (v == cfg::VIEW_HOME && ble::count() == 0) ||
                         (v == cfg::VIEW_BOILER && !settings().hasViessmann());
    if (!skipped) break;
  }
  if (v == view_) {
    viewStart_ = millis();
    return;
  }
  prevView_ = view_;
  view_ = static_cast<uint8_t>(v);
  transitioning_ = true;
  transStart_ = millis();
  enterStart_ = transStart_;
  viewStart_ = transStart_;
  pinned_ = -1;
  alertActive_ = false;
}

void WeatherUi::pinView(int idx) {
  if (idx < 0) {
    pinned_ = -1;
    viewStart_ = millis();  // pelny czas na biezacym ekranie, potem rusza dalej
    return;
  }
  if (idx >= cfg::VIEW_COUNT) return;
  pinned_ = static_cast<int8_t>(idx);
  if (idx == view_ && !transitioning_) return;  // juz na nim jestesmy

  prevView_ = view_;
  view_ = static_cast<uint8_t>(idx);
  transitioning_ = true;
  transStart_ = millis();
  enterStart_ = transStart_;
  alertActive_ = false;
}

void WeatherUi::streamScreenshot(WiFiClient& client, const WeatherModel& w, const PvModel& pv,
                                 const PvHistory& hist, const FlightModel& fl, bool wifiOk) {
  if (!ready_ || freed_) {
    return;   // trwa OTA — sterty i tak nie ma na nic
  }

  constexpr int WD = cfg::SCREEN_W;
  constexpr int HT = cfg::SCREEN_H;
  const uint32_t rowSize = WD * 3;            // 320*3 = 960, podzielne przez 4
  const uint32_t dataSize = rowSize * HT;
  const uint32_t fileSize = 54 + dataSize;

  uint8_t hdr[54] = {0};
  hdr[0] = 'B'; hdr[1] = 'M';
  hdr[2] = fileSize; hdr[3] = fileSize >> 8; hdr[4] = fileSize >> 16; hdr[5] = fileSize >> 24;
  hdr[10] = 54;
  hdr[14] = 40;
  hdr[18] = WD; hdr[19] = WD >> 8;
  hdr[22] = HT; hdr[23] = HT >> 8;            // dodatnie = obraz od dołu
  hdr[26] = 1;
  hdr[28] = 24;
  hdr[34] = dataSize; hdr[35] = dataSize >> 8;
  hdr[36] = dataSize >> 16; hdr[37] = dataSize >> 24;

  // Własny pasek roboczy — NIE bufor wyświetlacza. 320x24x16bpp = 15,4 kB.
  TFT_eSprite shot(&tft_);
  shot.setColorDepth(16);
  if (shot.createSprite(WD, SHOT_H) == nullptr) {
    return;   // brak pamięci — lepiej nic nie wysłać niż zabrać ją radarowi
  }
  shot.setSwapBytes(false);

  client.print("HTTP/1.1 200 OK\r\nContent-Type: image/bmp\r\n");
  client.printf("Content-Length: %lu\r\n", static_cast<unsigned long>(fileSize));
  client.print("Cache-Control: no-store\r\nConnection: close\r\n\r\n");
  client.write(hdr, sizeof(hdr));

  // Jedna klatka = jeden moment w czasie. Gdyby każdy pasek brał świeże millis()
  // albo świeży odczyt sterty, rozjechałyby się nie tylko animacje: między paskami
  // leci transmisja BMP (setki ms), więc napisy "OK 12s temu" i "WOLNY RAM" na ekranie
  // statystyk pokazałyby w kolejnych paskach RÓŻNE wartości i litery rwałyby się w pół.
  const uint32_t nowMs = millis();
  const uint32_t heapNow = ESP.getFreeHeap();

  static uint8_t line[WD * 3];
  for (int top = HT - SHOT_H; top >= 0; top -= SHOT_H) {   // BMP idzie od dołu
    setBand(shot, top, HT);          // układ globalny 0..239 (ze stopką)
    // paintFrame czyści 0..205, drawFooterTo maluje 206..239 — razem cały ekran,
    // więc świeżo wyzerowany sprite nie prześwituje nigdzie na czarno.
    paintFrame(shot, w, pv, hist, fl, wifiOk, nowMs, heapNow);
    drawFooterTo(shot, pv, wifiOk);  // sama sprawdzi, czy wpada w ten pasek

    for (int y = top + SHOT_H - 1; y >= top; --y) {
      for (int x = 0; x < WD; ++x) {
        const uint16_t c = shot.readPixel(x, y);   // współrzędne globalne
        line[x * 3 + 0] = static_cast<uint8_t>((c & 0x1F) << 3);          // B
        line[x * 3 + 1] = static_cast<uint8_t>(((c >> 5) & 0x3F) << 2);   // G
        line[x * 3 + 2] = static_cast<uint8_t>(((c >> 11) & 0x1F) << 3);  // R
      }
      client.write(line, sizeof(line));
    }
  }

  shot.deleteSprite();
  client.flush();
}
