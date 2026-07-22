#include "WeatherUi.h"
#include "RoomHistory.h"

#include <Arduino.h>
#include <cmath>
#include <cstdio>
#include <ctime>

#include "AirClient.h"
#include "BleGateway.h"
#include "Colors.h"
#include "Config.h"
#include "Moon.h"
#include "GasMeter.h"
#include "Viessmann.h"
#include "MapDataWide.h"
#include "MapDataRadar.h"
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
#include "RetroFont.h"
#include "RetroSprites.h"
#include "ThemeV2.h"

// v111: widok PAMIEC czyta te trzy wprost z ESP-IDF (heap_caps_*/partycje/OTA) —
// juz i tak zlinkowane (ESP.getFreeHeap(), Ota.cpp, OtaGuard.cpp korzystaja z tego
// samego), wiec to nie sa nowe zaleznosci, tylko nowe wywolania istniejacego kodu.
#include <esp_heap_caps.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>

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

// Kolor indeksu jakosci powietrza (1..6, tabela ARMAAG — patrz AirClient.cpp).
// DYSKRETNY, nie interpolowany jak tempColor() wyzej: to sa oddzielne KLASY oceny
// ("dobre" kontra "zle"), nie ciagla skala fizyczna, wiec plynne przejscie miedzy
// kolorami sugerowaloby stany posrednie, ktorych tabela nie zna.
uint16_t airIndexColor(int index) {
  switch (index) {
    case 1: return col::AIR_GOOD;
    case 2: return col::AIR_FAIR;
    case 3: return col::AIR_MODERATE;
    case 4: return col::AIR_POOR;
    case 5: return col::AIR_BAD;
    case 6: return col::AIR_SEVERE;
    default: return col::TEXT_MUTE;   // 0 = nie da sie policzyc
  }
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

// Bajty -> tekst, jednostka dobrana automatycznie: <1 MB w kB (bez ulamka),
// >=1 MB w MB (2 miejsca po przecinku). Ten sam pomysl co fmtPower (W/kW) wyzej —
// spojne jednostki na ekranie PAMIEC (v111), od 20 kB (NVS) po 4 MB (flash).
void fmtBytes(char* buf, size_t n, uint32_t bytes) {
  if (bytes >= 1024UL * 1024UL) {
    snprintf(buf, n, "%.2f MB", bytes / 1048576.f);
  } else {
    snprintf(buf, n, "%lu kB", static_cast<unsigned long>(bytes / 1024));
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
  tft_.init();

  // LEDC PODPINAMY DOPIERO TU — PO tft_.init(). Kolejnosc jest istotna, a nie
  // kosmetyczna: init() potrafi wolac digitalWrite() na pinie podswietlenia, co w
  // rdzeniu esp32 3.x odpina od niego kanal LEDC i zostawia pin na sztywno HIGH.
  // Wtedy sterowanie jasnoscia przestaje dzialac CICHO — ledcWrite() dalej zwraca
  // sukces. Podpiecie po init() sprawia, ze ostatnie slowo ma PWM.
  pinMode(cfg::PIN_TFT_BL, OUTPUT);
  ledcAttach(cfg::PIN_TFT_BL, cfg::BL_PWM_FREQ, cfg::BL_PWM_BITS);
  ledcWrite(cfg::PIN_TFT_BL, 0);   // ciemno tylko na czas czyszczenia ekranu ponizej

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

  // Podswietlenie zapalamy OD RAZU na pelna moc, bez rampy od zera.
  // Powod: rampa (krok 6) rusza wylacznie z tickBacklight(), a to jest wolane tylko
  // ze sciezek RYSOWANIA. Podczas setup() — WiFi ~3.6 s, Modbus ~3.2 s, BLE, radar —
  // rysowania praktycznie nie ma, wiec ekran wisialby ciemny przez kilkanascie sekund
  // i wygladaloby to jak zawieszony start. Wczesniej tego nie bylo widac, bo TFT_eSPI
  // trzymal pin na sztywno HIGH (patrz User_Setup.h) i fade-in nie mial jak zadzialac.
  // Po naprawie sterowania fade-in nagle stal sie widoczny — stad ta zmiana.
  // Automat z LDR sciagnie jasnosc w dol po pierwszym odczycie swiatla.
  blCurrent_ = cfg::BL_DAY;
  blTarget_ = cfg::BL_DAY;
  ledcWrite(cfg::PIN_TFT_BL, blCurrent_);
  ready_ = true;
  return true;
}

void WeatherUi::startBacklightSweep(uint32_t ms) {
  const uint32_t now = millis();
  blSweepStart_ = now;
  blSweepUntil_ = now + ms;
  // Wymuszenie z LDR ma byc wylaczone na czas testu — inaczej petla loop() co klatke
  // nadpisywalaby jasnosc wyliczona z rampy i nic by nie pulsowalo.
  blForceUntil_ = blSweepUntil_;
}

// Ekran testu: DUZA liczba PWM + pasek + rampa. Sens jest jeden — pozwolic porownac
// to, co firmware TWIERDZI, ze wystawia, z tym, co oko WIDZI. Rysowany prosto na
// buforze, bez HUD-u i stopki: to nie jest widok z rotacji, tylko narzedzie.
void WeatherUi::drawBacklightSweep(TFT_eSPI& spr, uint32_t nowMs) {
  spr.fillRect(0, 0, W, VIEW_H, col::BG);
  plCenter(spr, PLF18, "TEST PODSWIETLENIA", W / 2, 30, col::ACCENT);
  gl(spr, "PIN 14 — jasnosc ma pulsowac", 14, 44, col::TEXT_DIM);

  // Wielka liczba: to, co realnie idzie na PWM.
  char v[8];
  snprintf(v, sizeof(v), "%u", static_cast<unsigned>(blCurrent_));
  const int vw = bigStr(spr, &FreeSansBold24pt7b, v, 0, 0, col::BG);  // pomiar szerokosci
  bigStr(spr, &FreeSansBold24pt7b, v, (W - vw) / 2, 112, col::TEXT);
  glCenter(spr, "z 255", W / 2, 120, col::TEXT_MUTE);

  // Pasek proporcjonalny do wartosci — druga, niezalezna reprezentacja tej samej
  // liczby (latwiej zlapac wzrokiem ruch niz zmiane cyfr).
  const int bx = 30, bw = W - 60, by = 140, bh = 18;
  spr.drawRect(bx - 2, by - 2, bw + 4, bh + 4, col::DIVIDER);
  const int fill = (bw * blCurrent_) / 255;
  if (fill > 0) spr.fillRect(bx, by, fill, bh, col::ACCENT);

  // Ile testu zostalo — zeby bylo widac, ze sam sie skonczy.
  const int32_t left = static_cast<int32_t>(blSweepUntil_ - nowMs);
  char rest[28];
  snprintf(rest, sizeof(rest), "koniec za %ld s", static_cast<long>(left > 0 ? left / 1000 : 0));
  glCenter(spr, rest, W / 2, 176, col::TEXT_MUTE);
  glCenter(spr, "jesli jasnosc STOI — pin nie jest sterowany", W / 2, 190, col::WARN);
}

void WeatherUi::tickBacklight() {
  // Rampa testowa: trojkat 255 -> 20 -> 255 w cyklu 12 s. Trojkat, nie sinus —
  // liniowa zmiana latwiej pozwala ocenic okiem, czy jasnosc idzie ROWNO, czy
  // skacze. Ustawiamy blCurrent_ WPROST (z pominieciem zwyklej rampy krokiem 6),
  // bo tutaj to wlasnie faza ma byc plynna, nie dojscie do celu.
  const uint32_t nowMsBl = millis();
  if (blSweepUntil_ != 0 && static_cast<int32_t>(nowMsBl - blSweepUntil_) < 0) {
    const uint32_t phase = (nowMsBl - blSweepStart_) % 12000;
    const uint32_t half = phase < 6000 ? phase : (12000 - phase);   // 0..6000..0
    const int val = 20 + static_cast<int>((235UL * half) / 6000);   // 20..255
    blCurrent_ = static_cast<uint8_t>(val);
    blTarget_ = blCurrent_;
    ledcWrite(cfg::PIN_TFT_BL, blCurrent_);
    return;
  }

  // Koniec wymuszenia z panelu — oddajemy sterowanie automatowi z LDR. Sprawdzane
  // TU, bo tickBacklight() jest wolane z kazdej sciezki rysowania, wiec test wygasnie
  // nawet gdyby petla glowna akurat stala na ekranie startowym albo w portalu.
  if (blForceUntil_ != 0 && static_cast<int32_t>(millis() - blForceUntil_) >= 0) {
    blForceUntil_ = 0;
  }
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
    "RETRO", "TERAZ", "GODZINY", "RADAR", "5 DNI", "W DOMU", "PIEC", "FOTOWOLTAIKA",
    "SAMOLOTY", "POWIETRZE", "PAMIĘĆ", "RUCH", "STATYSTYKI"};

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

// Definicja zyje tu (obok drawProgress, jej jedynego dotychczasowego uzytkownika),
// ale jest publiczna i statyczna (patrz deklaracja w WeatherUi.h) — themev2::
// hudSegments w ThemeV2.cpp wola ja bez wlasnej instancji WeatherUi. Warunki SA
// dokladnie tym, co do v118 stalo wprost w drawProgress(); wydzielenie nie zmienia
// zadnego z nich, tylko daje im jedno miejsce zamiast dwoch.
bool WeatherUi::viewSkipped(int i, const AirModel* air) {
  // V3 "Pasmowy" nie ma ekranu RETRO (Mario) ani osobnego GODZINY — prognoza
  // godzinowa jest wchlonieta w pasek opadu ekranu glownego (patrz makieta 01).
  // Pomijamy je w rotacji, zeby kolejnosc byla: GLOWNY→RADAR→5 DNI→PRAD→POKOJE→
  // OGRZEWANIE→POWIETRZE→SAMOLOTY, zgodnie ze specyfikacja projektu.
  if (settings().theme == 3 && (i == cfg::VIEW_RETRO || i == cfg::VIEW_HOURS)) {
    return true;
  }
  return (i == cfg::VIEW_RADAR && !radarmap::hasRain()) ||
         (i == cfg::VIEW_HOME && ble::count() == 0) ||
         (i == cfg::VIEW_BOILER && !settings().hasViessmann()) ||
         (i == cfg::VIEW_AIR && (!air || !air->ready));
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
    // Warunki sa w viewSkipped() (patrz komentarz przy deklaracji w WeatherUi.h) —
    // to samo pyta drugi pasek, w stylu V2 (themev2::hudSegments).
    const bool skipped = viewSkipped(i, air_);
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

// -------------------------------------------------------------- WIDOK 0: RETRO --
// Ekran ozdobny w stylu gry platformowej z przelomu lat 80/90 (Mario), pierwszy w
// rotacji (cfg::VIEW_RETRO == 0). Rysuje WLASNY HUD gorny i dolny w stylu 8-bit —
// patrz komentarze w WeatherUi.h przy deklaracji i w paintFrame()/render() przy
// miejscach, gdzie z tego powodu omija wspolna belke/pasek postepu/stopke PV.
// Grafika (font, sprite Maria) jest w RetroFont.h / RetroSprites.h — tu tylko
// ja rysujemy i doklejamy tlo (niebo, slonce, chmury, miasto, platforma, blok "?").

namespace {

// Paleta z zaakceptowanego mockupu (RGB888 -> RGB565 przez C565() z Colors.h).
// Osobna od namespace col:: — to inny jezyk wizualny (gra 8-bitowa), mieszanie
// jej z paleta panelu danych zacieraloby granice miedzy dwoma stylami ekranow.
namespace rcol {
// SKY_T/SKY_M/SKY_B (z mockupu) NIE zyja tu jako gotowe RGB565 — gradient nieba
// (patrz drawViewRetro) kwantyzuje kazdy kanal PRZED spakowaniem do 565, wiec
// potrzebuje surowych skladowych 0-255, nie tego zapakowanego koloru. Zeby nie
// trzymac tych samych trzech liczb w dwoch miejscach (i nie ryzykowac rozjazdu),
// stale RGB888 sa inline w petli gradientu, a tu zostaje tylko komentarz z ich
// nazwami dla latwego dopasowania do mockupu: SKY_T=(74,58,107), SKY_M=(122,106,155),
// SKY_B=(150,184,216). SKY_M ponizej to jedyna z trzech, ktora jest tez uzywana
// jako gotowy kolor (blaknięcie napisu "+1" w kolor nieba).
constexpr uint16_t SKY_M   = C565(122, 106, 155);
constexpr uint16_t SUN_A   = C565(255, 208, 112);
constexpr uint16_t SUN_B   = C565(255, 144, 80);
constexpr uint16_t CLOUD   = C565(232, 240, 250);
constexpr uint16_t CLOUD_E = C565(255, 232, 160);
constexpr uint16_t BRICK   = C565(200, 96, 88);
constexpr uint16_t BRICK_D = C565(160, 72, 64);
constexpr uint16_t BRICK_L = C565(232, 136, 120);
constexpr uint16_t MOSS    = C565(136, 192, 64);
constexpr uint16_t CITY_1  = C565(106, 122, 155);
constexpr uint16_t CITY_2  = C565(88, 100, 132);
constexpr uint16_t RED     = C565(224, 64, 64);
constexpr uint16_t WHITE   = C565(248, 248, 248);
constexpr uint16_t YEL     = C565(248, 208, 32);
constexpr uint16_t CYAN    = C565(120, 224, 240);
constexpr uint16_t HUD     = C565(28, 24, 44);
constexpr uint16_t HUD_LN  = C565(70, 60, 100);
constexpr uint16_t BLACK   = C565(0, 0, 0);
}  // namespace rcol

// ---- tekst RetroFontu ---------------------------------------------------------

// Jeden znak jako siatka kwadratow scale x scale. Sasiadujace w poziomie zapalone
// bity sklejamy w JEDEN szerszy fillRect zamiast osobnego wywolania na kazda
// kolumne — przy s=6 (wielka temperatura) i przy dwoch wlasnych HUD-ach na tym
// ekranie to roznica miedzy setkami a tysiacami wywolan na klatke (patrz ograniczenie
// fps w zadaniu: nigdy pojedynczy prymityw na piksel po calym ekranie).
void retroChar(TFT_eSPI& s, char c, int x, int y, int scale, uint16_t color) {
  const int idx = retrofont::index(c);
  if (idx < 0) return;   // znak spoza zestawu (patrz retroAscii) — po prostu pomijamy
  for (int row = 0; row < 8; ++row) {
    const uint8_t bits = pgm_read_byte(&retrofont::GLYPHS[idx][row]);
    if (bits == 0) continue;
    int col = 0;
    while (col < 8) {
      if (!(bits & (0x80 >> col))) { ++col; continue; }
      int end = col;
      while (end < 8 && (bits & (0x80 >> end))) ++end;
      s.fillRect(x + col * scale, y + row * scale, (end - col) * scale, scale, color);
      col = end;
    }
  }
}

// Napis: krok miedzy znakami to 9*scale (8 px znaku + 1 px odstepu). GLYPHS same w
// sobie nie rezerwuja marginesu — niektore (np. '0') rysuja az do kolumny 7 — wiec
// bez tego dodatkowego odstepu litery by sie stykaly.
int retroStr(TFT_eSPI& s, const char* t, int x, int y, int scale, uint16_t color) {
  int cx = x;
  for (const char* p = t; *p; ++p) {
    retroChar(s, *p, cx, y, scale, color);
    cx += 9 * scale;
  }
  return cx - x;
}

// Kazdy napis na tym ekranie dostaje czarny cien, przesuniety o `scale` w prawo
// i w dol, rysowany PRZED wlasciwym tekstem — bez tego jasne litery gina na tle
// nieba/chmur (niebo tego ekranu jest zamierzenie ciemne u gory, patrz kwantyzacja
// nizej).
int retroStrShadowed(TFT_eSPI& s, const char* t, int x, int y, int scale, uint16_t color) {
  retroStr(s, t, x + scale, y + scale, scale, rcol::BLACK);
  return retroStr(s, t, x, y, scale, color);
}

// UTF-8 -> WIELKIE ASCII zrozumiale dla RetroFontu (ktory nie ma malych liter ani
// polskich znakow — patrz naglowek RetroFont.h). Polskie litery to w UTF-8 zawsze
// sekwencje DWUBAJTOWE (0xC3/0xC4/0xC5 + drugi bajt) — trzeba je rozpoznac jawnie:
// bez tego kazdy z dwoch bajtow lecialby do fontu osobno, a to albo znika (indeks
// -1), albo przypadkiem trafia w zupelnie inny, przypadkowy znak z tablicy glifow.
// Uzyte i dla nazwy miasta (z ustawien — uzytkownik moze wpisac cokolwiek), i dla
// opisu pogody (patrz drawViewRetro).
void retroAscii(char* dst, size_t dstSize, const char* src) {
  size_t o = 0;
  auto p = reinterpret_cast<const unsigned char*>(src);
  while (*p != 0 && o + 1 < dstSize) {
    char rep = 0;
    if (p[0] == 0xC4 && p[1] != 0) {
      switch (p[1]) {
        case 0x84: case 0x85: rep = 'A'; break;  // Ą ą
        case 0x86: case 0x87: rep = 'C'; break;  // Ć ć
        case 0x98: case 0x99: rep = 'E'; break;  // Ę ę
      }
      if (rep) dst[o++] = rep;
      p += 2;
      continue;
    }
    if (p[0] == 0xC5 && p[1] != 0) {
      switch (p[1]) {
        case 0x81: case 0x82: rep = 'L'; break;  // Ł ł
        case 0x83: case 0x84: rep = 'N'; break;  // Ń ń
        case 0x9A: case 0x9B: rep = 'S'; break;  // Ś ś
        case 0xB9: case 0xBA: rep = 'Z'; break;  // Ź ź
        case 0xBB: case 0xBC: rep = 'Z'; break;  // Ż ż
      }
      if (rep) dst[o++] = rep;
      p += 2;
      continue;
    }
    if (p[0] == 0xC3 && p[1] != 0) {
      switch (p[1]) {
        case 0x93: case 0xB3: rep = 'O'; break;  // Ó ó
      }
      if (rep) dst[o++] = rep;
      p += 2;
      continue;
    }
    char c = static_cast<char>(*p);
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    dst[o++] = c;
    ++p;
  }
  dst[o] = '\0';
}

// ---- deterministyczny "szum" ---------------------------------------------------

// Hash liczby -> liczba: to samo wejscie ZAWSZE daje to samo wyjscie. Sylwetka
// miasta i kepki mchu potrzebuja czegos, co WYGLADA losowo, ale rand()/millis()
// jako zrodlo dawaloby przy KAZDYM przerysowaniu (20 razy na sekunde) inny uklad —
// tlo migotaloby zamiast stac w miejscu.
uint32_t hash32(uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352dU;
  x ^= x >> 15; x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}

// Jedna warstwa dachow: bloki 12-24 px szerokie, 14-54 px wysokie, z kilkoma
// jasniejszymi "oknami". `seed` rozroznia warstwy (dalsza/blizsza), inaczej
// bylyby identyczne, tylko przesuniete o 6 px w pionie.
void drawCityLayer(TFT_eSPI& s, int ox, int baseY, uint16_t color, uint32_t seed) {
  constexpr uint16_t kWin = C565(255, 224, 160);   // cieple, zapalone okno
  int x = 0, i = 0;
  while (x < W) {
    const uint32_t h1 = hash32(seed + static_cast<uint32_t>(i) * 131u);
    const uint32_t h2 = hash32(seed + static_cast<uint32_t>(i) * 131u + 17u);
    const int bw = 12 + static_cast<int>(h1 % 13u);   // 12..24
    const int bh = 14 + static_cast<int>(h2 % 41u);   // 14..54
    const int by = baseY - bh;
    s.fillRect(ox + x, by, bw, bh, color);
    for (int wy = by + 3; wy + 2 <= baseY - 3; wy += 6) {
      for (int wx = x + 3; wx + 2 <= x + bw - 3; wx += 6) {
        // Nie kazde okno swieci — ktore, decyduje hash (zawsze ten sam wynik).
        const uint32_t hw = hash32(seed + static_cast<uint32_t>(wx) * 977u +
                                    static_cast<uint32_t>(wy) * 131u);
        if ((hw & 3u) == 0u) {
          s.fillRect(ox + wx, wy, 2, 2, kWin);
        }
      }
    }
    x += bw + 2;
    ++i;
  }
}

// ---- Mario ----------------------------------------------------------------------

// Sprite 16x16 (nibble/piksel — patrz RetroSprites.h), powiekszony do scale x scale.
// Indeks 0 w palecie = przezroczysty (pomijamy). Serie tego samego koloru w
// poziomie sklejamy w jeden fillRect (jak w retroChar) — bez tego kazda klatka
// biegu to 256 wywolan, x4 klatki, 20 razy na sekunde.
void drawMario(TFT_eSPI& s, int x, int y, int frame, int scale) {
  if (x + mariospr::W * scale < 0 || x > W) return;   // cala klatka poza ekranem
  const uint8_t* d = mariospr::DATA[frame];
  for (int row = 0; row < mariospr::H; ++row) {
    int col = 0;
    while (col < mariospr::W) {
      const uint8_t byte = pgm_read_byte(&d[row * (mariospr::W / 2) + col / 2]);
      const uint8_t nib = (col % 2 == 0) ? (byte >> 4) : (byte & 0x0F);
      if (nib == 0) { ++col; continue; }
      int end = col + 1;
      while (end < mariospr::W) {
        const uint8_t b2 = pgm_read_byte(&d[row * (mariospr::W / 2) + end / 2]);
        const uint8_t n2 = (end % 2 == 0) ? (b2 >> 4) : (b2 & 0x0F);
        if (n2 != nib) break;
        ++end;
      }
      const uint16_t color = pgm_read_word(&mariospr::PALETTE[nib]);
      s.fillRect(x + col * scale, y + row * scale, (end - col) * scale, scale, color);
      col = end;
    }
  }
}

// Grzybek premii: noga jasnobezowa, czerwony kapelusz z bialymi kropkami. Rysowany
// proceduralnie (nie sprite'em z RetroSprites.h) — to jedyne miejsce na tym
// ekranie, ktore go potrzebuje, wiec nie oplaca sie trzymac dla niego osobnej
// bitmapy w plikach z grafika.
void drawMushroom(TFT_eSPI& s, int x, int groundY) {
  constexpr int w = 14, capH = 6, stemH = 6;
  constexpr uint16_t stem = C565(240, 232, 208);
  constexpr uint16_t cap = C565(216, 48, 48);
  constexpr uint16_t capShade = C565(150, 32, 32);
  constexpr uint16_t white = C565(248, 248, 248);
  const int y0 = groundY - capH - stemH;
  // KOLEJNOSC MA ZNACZENIE: kapelusz (pelna wysokosc capH*2, zeby wyszedl okragly
  // dol, nie plaski) rysujemy PRZED noga. Gdyby noga posla pierwsza, kapelusz —
  // wyzszy i rysowany na calej szerokosci az do y0+capH*2 — zamalowalby ja
  // calkowicie (oba prostokaty konczylyby sie na tym samym dolnym brzegu). Noga
  // na wierzchu wystaje spod zaokraglonego brzegu kapelusza, tak jak w oryginale.
  s.fillRoundRect(x, y0, w, capH * 2, capH, cap);
  s.fillRect(x + 3, y0 + capH, w - 6, stemH, stem);
  s.drawFastHLine(x + 1, y0 + capH, w - 2, capShade);
  s.fillRect(x + 2, y0 + 1, 2, 2, white);
  s.fillRect(x + w - 4, y0 + 1, 2, 2, white);
  s.fillRect(x + w / 2 - 1, y0 + 3, 2, 2, white);
}

// Znak zapytania na bloku premii. NIE idzie przez RetroFont — ten w ogole nie
// deklaruje '?' w FIRST_CHARS (RetroFont.h), a to jednorazowa ikona gry, nie
// tekst, wiec nie ma powodu poszerzac wspolnego fontu dla jednego miejsca.
void drawQMark(TFT_eSPI& s, int x, int y, int scale, uint16_t color) {
  static const uint8_t kBits[8] = {0x3C, 0x66, 0x06, 0x0C, 0x18, 0x00, 0x18, 0x00};
  for (int row = 0; row < 8; ++row) {
    const uint8_t bits = kBits[row];
    for (int col = 0; col < 8; ++col) {
      if (bits & (0x80 >> col)) {
        s.fillRect(x + col * scale, y + row * scale, scale, scale, color);
      }
    }
  }
}

}  // namespace

void WeatherUi::drawViewRetro(TFT_eSPI& spr, int ox, float t, const WeatherModel& w,
                               uint32_t nowMs) {
  // Brak wjazdu/wyjazdu osobno od reszty tresci — cala klatka (HUD wlacznie)
  // slizga sie razem pod `ox`, tak jak w kazdym innym drawView*.
  (void)t;
  const WeatherSnapshot& c = w.current;

  // ================================================================= HUD gorny --
  spr.fillRect(ox, 0, W, 25, rcol::HUD);
  spr.drawFastHLine(ox, 25, W, rcol::HUD_LN);

  {
    // Nazwa miasta jest z ustawien (uzytkownik moze ja zmienic w panelu WWW) —
    // retroAscii() zabezpiecza przed polskimi znakami, ktorych ten font nie ma.
    char cityBuf[40];
    retroAscii(cityBuf, sizeof(cityBuf), settings().city);
    retroStrShadowed(spr, cityBuf, ox + 6, 4, 2, rcol::WHITE);
  }
  {
    // Zegar scienny czytamy swiezo (jak drawHeader/drawViewMotion), NIE z nowMs —
    // to kalendarz, nie animacja. Rozjazd o pojedyncza sekunde miedzy paskami
    // zrzutu (albo miedzy dwiema polowkami przejscia) nikomu nie zaszkodzi — ten
    // sam kompromis co w drawHeader.
    const time_t now = time(nullptr);
    char dateBuf[8], timeBuf[8];
    if (now < 1700000000) {
      snprintf(dateBuf, sizeof(dateBuf), "--.--");
      snprintf(timeBuf, sizeof(timeBuf), "--:--");
    } else {
      struct tm tmv{};
      localtime_r(&now, &tmv);
      snprintf(dateBuf, sizeof(dateBuf), "%02d-%02d", tmv.tm_mday, tmv.tm_mon + 1);
      snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    }
    retroStrShadowed(spr, dateBuf, ox + 118, 4, 2, rcol::YEL);
    retroStrShadowed(spr, timeBuf, ox + 214, 4, 2, rcol::CYAN);
  }

  // ===================================================================== niebo --
  // Kwantyzacja (v & 0xF0, 16 poziomow na kanal zamiast 256) to CELOWY "retro
  // banding" z gry 8-bitowej — nie oszczednosc i nie przypadek zaokraglenia.
  // Gladki gradient (bez tej linijki) wygladalby na tym ekranie jak z INNEGO,
  // wspolczesnego widoku pogodowego — ktos "poprawiajac" to na czysty lerp565
  // zepsulby caly efekt.
  constexpr int skyTop = 26, skyBot = 186;
  for (int y = skyTop; y < skyBot; ++y) {
    const float f = static_cast<float>(y - skyTop) / static_cast<float>(skyBot - skyTop - 1);
    uint8_t r, g, b;
    if (f < 0.5f) {
      const float ff = f / 0.5f;
      r = static_cast<uint8_t>(74.f + (122.f - 74.f) * ff);
      g = static_cast<uint8_t>(58.f + (106.f - 58.f) * ff);
      b = static_cast<uint8_t>(107.f + (155.f - 107.f) * ff);
    } else {
      const float ff = (f - 0.5f) / 0.5f;
      r = static_cast<uint8_t>(122.f + (150.f - 122.f) * ff);
      g = static_cast<uint8_t>(106.f + (184.f - 106.f) * ff);
      b = static_cast<uint8_t>(155.f + (216.f - 155.f) * ff);
    }
    spr.drawFastHLine(ox, y, W, C565(r & 0xF0, g & 0xF0, b & 0xF0));
  }

  // ===================================================================== slonce --
  {
    constexpr int cx = 250, cy = 58, r = 26, cell = 4;
    for (int by = cy - r; by < cy + r; by += cell) {
      for (int bx = cx - r; bx < cx + r; bx += cell) {
        const int dx = (bx + cell / 2) - cx;
        const int dy = (by + cell / 2) - cy;
        if (dx * dx + dy * dy > r * r) continue;   // poza okregiem — pomijamy kwadrat
        // Kwadraty 4x4, NIE okragle piksele: to slonce z gry 8-bitowej (Mario), a
        // reszta apki rysuje pogode oblymi ikonami (WeatherIcons.h) — okrag by tu
        // zaprzeczyl calej stylistyce tego jednego ekranu.
        spr.fillRect(ox + bx, by, cell, cell, dy < -4 ? rcol::SUN_A : rcol::SUN_B);
      }
    }
  }

  // ===================================================================== chmury --
  {
    auto cloud = [&](int ccx, int cyTop, int scale) {
      static const int kUnits[4] = {6, 14, 18, 14};
      // JEDNOSTKA = skala, a NIE 4*skala. Pierwsza wersja mnozyla przez 4 i chmura
      // przy scale=3 miala 18*12 = 216 px szerokosci — pol ekranu bieli, ktora
      // zaslaniala temperature (zweryfikowane zrzutem z urzadzenia). Tu jednostki
      // 6/14/18/14 sa w PIKSELACH skali: przy scale=3 daje to 54 px, czyli chmurke.
      const int unit = scale;
      const int band = 2 * scale;   // wysokosc jednego pasa chmury
      int y = cyTop, lastW = kUnits[3] * unit;
      for (int i = 0; i < 4; ++i) {
        const int wpx = kUnits[i] * unit;
        spr.fillRect(ox + ccx - wpx / 2, y, wpx, band, rcol::CLOUD);
        y += band;
        lastW = wpx;
      }
      // Podswietlony spod chmury — jakby slonce przebijalo od dolu; ten sam trik
      // co w tlach klasycznych platformowek z tamtej epoki.
      spr.fillRect(ox + ccx - lastW / 2, y, lastW, unit, rcol::CLOUD_E);
    };
    cloud(196, 40, 3);
    cloud(152, 72, 2);   // wyzej niz 96: przy y=96 nachodzila na "ODCZUW nn" (y=104)
  }

  // ===================================================================== miasto --
  // Deterministyczny generator (hash32 z indeksu budynku, ZERO stanu/seeda od
  // czasu) — inaczej sylwetka migotalaby przy kazdym z 20 przerysowan na sekunde.
  drawCityLayer(spr, ox, 176, rcol::CITY_2, 1000u);   // dalsza warstwa — rysowana pierwsza
  drawCityLayer(spr, ox, 182, rcol::CITY_1, 7000u);   // blizsza warstwa — na wierzchu

  // =============================================================== blok "?" -----
  {
    constexpr int qx = 268, qy = 112, qs = 24;
    spr.fillRect(ox + qx, qy, qs, qs, rcol::RED);
    spr.drawFastHLine(ox + qx, qy, qs, lerp565(rcol::RED, rcol::WHITE, 0.35f));
    spr.drawFastVLine(ox + qx, qy, qs, lerp565(rcol::RED, rcol::WHITE, 0.20f));
    spr.drawFastHLine(ox + qx, qy + qs - 1, qs, lerp565(rcol::RED, rcol::BLACK, 0.35f));
    spr.drawFastVLine(ox + qx + qs - 1, qy, qs, lerp565(rcol::RED, rcol::BLACK, 0.20f));
    drawQMark(spr, ox + qx, qy, 3, rcol::YEL);
  }

  // ============================================================= platforma ------
  {
    constexpr int py0 = 186, py1 = 196;
    spr.fillRect(ox, py0, W, py1 - py0, rcol::BRICK_D);   // spoiny jako tlo
    for (int by = py0; by < py1; by += 8) {
      // "Running bond": co drugi rzad przesuniety o pol cegly, inaczej spoiny
      // ulozylyby sie w rowna siatke i wygladaloby to jak plytki, nie mur.
      const int rowIdx = (by - py0) / 8;
      const int offset = (rowIdx % 2) * 4;
      const int remain = py1 - by;
      const int bh = remain < 7 ? remain : 7;
      if (bh <= 0) continue;
      for (int bx = -offset; bx < W; bx += 8) {
        spr.fillRect(ox + bx, by, 7, bh, rcol::BRICK);
      }
    }
    spr.drawFastHLine(ox, py0, W, rcol::BRICK_L);   // gorna krawedz jasniejsza
    // Mech na wierzchu: 2-3 px, NIEREGULARNY (hash32 na x) — rowny pasek od razu
    // zdradzalby, ze jest generowany, a nie "porosnietymi cegly".
    for (int x = 0; x < W; x += 3) {
      const int tuft = 2 + static_cast<int>(hash32(static_cast<uint32_t>(x) + 5000u) % 2u);
      spr.fillRect(ox + x, py0 - tuft, 3, tuft, rcol::MOSS);
    }
  }

  // ================================================ rabek dolnego HUD-u ---------
  // Bufor rysowania siega tylko do y=VIEW_H-1=205 — reszta pasa HUD dolnego
  // (206..239) to terytorium stopki PV, POZA buforem (patrz drawViewRetroFooter
  // i miejsce jej wywolania w render()/streamScreenshot()). Tu malujemy TYLKO ten
  // sam kolor co tamta funkcja, zeby zszycie na y=206 bylo niewidoczne — bez
  // tekstu: polowa znaku wpadlaby w bufor, polowa w stopke, i rwalaby sie w pol.
  spr.fillRect(ox, 196, W, VIEW_H - 196, rcol::HUD);
  spr.drawFastHLine(ox, 196, W, rcol::HUD_LN);

  // =============================================================== Mario --------
  // Pozycja, klatka, grzybek i "+1" to CZYSTA funkcja nowMs — zero wewnetrznego
  // stanu w klasie. Nie przypadek: /api/screen renderuje klatke w paskach
  // (paintFrame wolane wielokrotnie z TYM SAMYM nowMs) i przejscia rysuja ten sam
  // widok dwa razy w jednej klatce (raz jako prevView_, raz jako view_) — gdyby
  // pozycja Maria zalezala od millis() czytanego NA MIEJSCU, kazde z tych wywolan
  // zobaczyloby inny czas i klatki rozjechalyby sie w miejscu zszycia (dokladnie
  // ten problem rozwiazano juz w drawViewStats/drawViewMotion — patrz ich komentarze
  // o "jedna klatka = jeden moment").
  constexpr uint32_t kCycleMs = 13000;      // pelny przebieg ~13 s (mieści się w 12-15 s)
  constexpr float kXStart = -40.f, kXEnd = 340.f;
  constexpr int kMushroomTriggerX = 188;    // mario_x+16>=204  <=>  mario_x>=188
  constexpr uint32_t kPopMs = 1200;         // czas unoszenia/gasniecia napisu "+1"
  constexpr int kMushroomX = 214;
  constexpr int kGroundY = 186;             // GY z opisu — stopy Maria i grzybek stoja tu

  const uint32_t cyclePos = nowMs % kCycleMs;
  const float frac = static_cast<float>(cyclePos) / static_cast<float>(kCycleMs);
  const int marioX = static_cast<int>(kXStart + frac * (kXEnd - kXStart));

  // Ten sam wzor co marioX (nie osobna stala), zeby "moment podniesienia grzybka"
  // nigdy nie rozjechal sie z faktyczna pozycja Maria, np. po zmianie kCycleMs.
  const float triggerFrac = (kMushroomTriggerX - kXStart) / (kXEnd - kXStart);
  const uint32_t triggerMs = static_cast<uint32_t>(triggerFrac * kCycleMs);

  if (cyclePos < triggerMs) {
    drawMushroom(spr, ox + kMushroomX, kGroundY);
  } else if (cyclePos - triggerMs < kPopMs) {
    const float popT = static_cast<float>(cyclePos - triggerMs) / static_cast<float>(kPopMs);
    const int popY = (kGroundY - 18) - static_cast<int>(popT * 16);
    // "Gasniecie" bez prawdziwej alfy (fillRect jej nie ma) — przyblizone
    // przenikaniem koloru w kolor nieba w tej okolicy ekranu.
    const uint16_t faded = lerp565(rcol::YEL, rcol::SKY_M, popT);
    retroStrShadowed(spr, "+1", ox + kMushroomX, popY, 2, faded);
  }
  // W pozostalym oknie cyklu (grzybek juz zniknal, "+1" juz zgaslo) nie rysujemy
  // nic — nowy cykl (cyclePos < triggerMs po zawinieciu) przywraca grzybka.

  {
    constexpr int marioScale = 2;
    const int frame = static_cast<int>((nowMs / 110) % 4);   // 0,1,2,3 = stoi,bieg1,bieg2,bieg1
    drawMario(spr, ox + marioX, kGroundY - mariospr::H * marioScale, frame, marioScale);
  }

  // ========================================================== dane pogodowe -----
  char tempBuf[8];
  snprintf(tempBuf, sizeof(tempBuf), "%d", static_cast<int>(lroundf(c.tempC)));
  retroStrShadowed(spr, tempBuf, ox + 12, 44, 6, rcol::WHITE);
  retroStrShadowed(spr, "*C", ox + 114, 48, 3, rcol::YEL);

  char feelsBuf[16];
  snprintf(feelsBuf, sizeof(feelsBuf), "ODCZUW %d", static_cast<int>(lroundf(c.feelsC)));
  retroStrShadowed(spr, feelsBuf, ox + 12, 104, 2, rcol::WHITE);

  {
    // labelForCode(), NIE descForCode(): descForCode ma 28 wariantow ("Częściowe
    // zachmurzenie", "Silny marznący deszcz"...) — za dlugie na s=2 przy x=12.
    // labelForCode() to te same krotkie 8 kategorii, co ikona pogody gdzie indziej
    // w apce ("Słonecznie", "Burza"...) — stad przyklad z zadania "SŁONECZNIE".
    char descBuf[16];
    retroAscii(descBuf, sizeof(descBuf), wxico::labelForCode(c.weatherCode));
    retroStrShadowed(spr, descBuf, ox + 12, 126, 2, rcol::YEL);
  }
}

// HUD dolny RETRO. To DOKLADNIE ten sam pas (y=VIEW_H..SCREEN_H-1 = 206..239) co
// stopka PV (drawFooterTo) — wywolujacy (render()/streamScreenshot()) wybiera
// jedno z dwoch, nigdy oba na raz. `dst` generyczny z tego samego powodu co w
// drawFooterTo: dziala i na zywym TFT, i na pasku zrzutu ekranu.
void WeatherUi::drawViewRetroFooter(TFT_eSPI& dst, const WeatherModel& w) {
  const int y = VIEW_H;   // 206
  if (!dst.checkViewport(0, y, W, cfg::SCREEN_H - VIEW_H)) {
    return;
  }
  dst.fillRect(0, y, W, cfg::SCREEN_H - VIEW_H, rcol::HUD);
  // Brak danych (jeszcze przed pierwszym pobraniem): pusty pasek zamiast zer,
  // ktore wygladalyby jak realny (zerowy) odczyt wilgotnosci/wiatru/cisnienia.
  if (!w.ready) {
    return;
  }

  const int labelY = y + 1, valueY = y + 18;
  char buf[16];

  snprintf(buf, sizeof(buf), "%d%%", w.current.humidity);
  retroStrShadowed(dst, "WILGOC", 8, labelY, 2, rcol::YEL);
  retroStrShadowed(dst, buf, 8, valueY, 2, rcol::WHITE);

  // Kolumny 8 / 116 / 236 i wiatr BEZ spacji przed jednostka. Pierwsza wersja miala
  // 8/112/240 oraz "%d KM/H": przy dwucyfrowym wietrze wartosc konczyla sie na 224 px,
  // a cisnienie zaczynalo na 240 — 16 px przerwy przy znakach szerokich na 16 px
  // czytalo sie jak jedno slowo ("13 KM/H1012" na zrzucie z urzadzenia). Teraz
  // najszerszy przypadek to "13KM/H" = 96 px (116..212), czyli 24 px do nastepnej
  // kolumny. Trzycyfrowy wiatr (huragan) siegnie 228 px i nadal sie nie sklei.
  snprintf(buf, sizeof(buf), "%dKM/H", static_cast<int>(lroundf(w.current.windKmh)));
  retroStrShadowed(dst, "WIATR", 124, labelY, 2, rcol::YEL);
  retroStrShadowed(dst, buf, 124, valueY, 2, rcol::CYAN);

  snprintf(buf, sizeof(buf), "%d", static_cast<int>(lroundf(w.current.pressureHpa)));
  retroStrShadowed(dst, "HPA", 236, labelY, 2, rcol::YEL);
  retroStrShadowed(dst, buf, 236, valueY, 2, rcol::WHITE);
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
  // v111: geste ekrany eksploracyjne — wiecej czasu na przeczytanie (patrz Config.h).
  if (view == cfg::VIEW_MEM) return cfg::VIEW_HOLD_MEM_MS;
  if (view == cfg::VIEW_MOTION) return cfg::VIEW_HOLD_MOTION_MS;
  // Baza rotacji edytowalna z panelu (settings().dwellS, w sekundach). Dotyczy
  // ekranow BEZ wlasnej stalej powyzej — FLIGHTS/RADAR/MEM/MOTION/STATS maja swoj
  // czas i CELOWO ich nie ruszamy. dwellS jest juz clampniete w Settings (3..60 s),
  // wiec tu bez dodatkowej obrony; cfg::VIEW_HOLD_MS zostaje domyslna tej wartosci.
  return static_cast<uint32_t>(settings().dwellS) * 1000UL;
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
  // VIEW_RETRO (case ponizej) NIE sprawdza `v2` ani razu — jest identyczny w
  // obu wygladach z definicji (patrz komentarz przy Settings::theme w
  // Settings.h i przy WeatherUi::drawViewRetro w WeatherUi.h).
  // Motyw V3 "Pasmowy" przejmuje CALE rysowanie ekranu wlasnym ukladem (dwie
  // kolumny, jasne tlo, glify prymitywami) — nie miesza sie z galezia v1/v2 ani
  // jednym case. VIEW_RETRO celowo TU nie wpada: retro (Mario) zostaje wspolne dla
  // V1/V2 i w V3 nie istnieje — w rotacji V3 slot 0 pokazuje ekran glowny.
  if (settings().theme == 3) {
    drawV3(spr, view, ox, t, w, pv, hist, fl, nowMs, heapNow);
    return;
  }
  const bool v2 = settings().theme == 2;
  switch (view) {
    case cfg::VIEW_RETRO:
      if (w.ready) drawViewRetro(spr, ox, t, w, nowMs);
      else drawNoData(spr, ox, "Pobieram prognozę...");
      break;
    case cfg::VIEW_NOW:
      if (!w.ready) { drawNoData(spr, ox, "Pobieram prognozę..."); break; }
      if (v2) drawViewNowV2(spr, ox, t, w); else drawViewNow(spr, ox, t, w);
      break;
    case cfg::VIEW_HOURS:
      if (!w.ready) { drawNoData(spr, ox, "Pobieram prognozę..."); break; }
      if (v2) drawViewHoursV2(spr, ox, t, w); else drawViewHours(spr, ox, t, w);
      break;
    case cfg::VIEW_DAYS:
      if (!w.ready) { drawNoData(spr, ox, "Pobieram prognozę..."); break; }
      if (v2) drawViewDaysV2(spr, ox, t, w); else drawViewDays(spr, ox, t, w);
      break;
    case cfg::VIEW_RADAR:
      if (v2) drawViewRadarV2(spr, ox, t, w, nowMs); else drawViewRadar(spr, ox, t, w, nowMs);
      break;
    case cfg::VIEW_HOME:
      if (v2) drawViewHomeV2(spr, ox, t, w); else drawViewHome(spr, ox, t, w);
      break;
    case cfg::VIEW_BOILER:
      if (v2) drawViewBoilerV2(spr, ox, t); else drawViewBoiler(spr, ox, t);
      break;
    case cfg::VIEW_PV:
      if (v2) drawViewPvV2(spr, ox, t, pv, hist); else drawViewPv(spr, ox, t, pv, hist);
      break;
    case cfg::VIEW_FLIGHTS:
      if (v2) drawViewFlightsV2(spr, ox, t, fl); else drawViewFlights(spr, ox, t, fl);
      break;
    case cfg::VIEW_AIR:
      // Bez "if (w.ready)" jak przy NOW/HOURS/DAYS: ten widok zalezy od WLASNEGO
      // modelu (air_), nie od WeatherModel — gotowosc sprawdza on sam w srodku
      // (tak samo jak PV/BOILER/HOME czytaja swoje wlasne modele).
      if (v2) drawViewAirV2(spr, ox, t, w); else drawViewAir(spr, ox, t, w);
      break;
    case cfg::VIEW_MEM:
      if (v2) drawViewMemV2(spr, ox, t, heapNow); else drawViewMem(spr, ox, t, heapNow);
      break;
    case cfg::VIEW_MOTION:
      if (v2) drawViewMotionV2(spr, ox, t, nowMs); else drawViewMotion(spr, ox, t, nowMs);
      break;
    case cfg::VIEW_STATS:
      if (v2) drawViewStatsV2(spr, ox, t, nowMs, heapNow); else drawViewStats(spr, ox, t, nowMs, heapNow);
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

  // V3 "Pasmowy": kazdy ekran ma WLASNE, rozne tlo (jasne dwukolumnowe / pelne jasne
  // / ciemny radar), wiec dzielony slajd przejscia (rysowanie dwoch widokow na jednym
  // tle) nie ma sensu — drugi zamalowalby pierwszy. V3 robi ciecie: rysuje wprost
  // aktywny widok. Podczas alertu rysujemy plansze zdarzenia w stylu V3 (burza/mroz/
  // awaria — makiety 13/18/19), nie zwykly ekran; postep liczony jak V1 (260 ms).
  if (settings().theme == 3) {
    if (alertActive_) {
      drawV3Alert(spr, clampf(static_cast<float>(nowMs - alertStart_) / 260.f, 0.f, 1.f));
    } else {
      drawV3(spr, view_, 0, enterT, w, pv, hist, fl, nowMs, heapNow);
    }
  } else if (alertActive_) {
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

  // RETRO rysuje WLASNY HUD gorny (nazwa miasta/data/godzina w stylu 8-bit) na
  // y=0..25 i wlasny pasek na dole (patrz drawViewRetro/drawViewRetroFooter) —
  // wspolna belka (zegar/kropka zdrowia/tytul) i segmentowany pasek postepu
  // wygladalyby na nim jak inny motyw nabity na sile z gory. Chowamy je, gdy RETRO
  // jest ktorakolwiek ze stron aktywnego przejscia (nie tylko view_), zeby podczas
  // 340 ms slidu obie strony byly spojne — nie "chrome miga na pol ekranu".
  // V3 "Pasmowy" rysuje wlasny kontekst (ciemna kolumna z zegarem/data/temperatura)
  // i nie ma wspolnej belki ani segmentowego paska postepu — chowamy chrome tak samo
  // jak dla RETRO, na obu stronach ewentualnego przejscia.
  const bool hideChrome =
      settings().theme == 3 ||
      view_ == cfg::VIEW_RETRO || (transitioning_ && prevView_ == cfg::VIEW_RETRO);
  if (!hideChrome) {
    // V2 dostaje WLASNY HUD (miasto/data/godzina) i pasek segmentow zamiast
    // belki+paska V1 — patrz ThemeV2.h. `&& !alertActive_`: drawAlert() rysuje
    // na wspolrzednych V1 (CY=34), nie na V2_TITLE_Y=36..51 — przepisanie
    // popupu alertu na uklad V2 nie bylo w zakresie tego zadania (alerty sa
    // rzadkie i krotkie, 6,5 s), wiec na czas alertu CALA belka (w obu
    // wygladach) wraca do V1, zamiast ryzykowac nachodzenie dwoch ukladow.
    // Chrome (jak dzis) NIGDY nie slizga sie z `ox` — tylko tytul/tresc
    // widoku (drawView*V2 dostaje wlasne `ox`) jedzie podczas przejscia.
    if (settings().theme == 2 && !alertActive_) {
      themev2::hudTop(spr, 0, settings().city);
      const float frac = transitioning_
                             ? 0.f
                             : clampf(static_cast<float>(nowMs - viewStart_) / holdFor(view_), 0.f, 1.f);
      themev2::hudSegments(spr, 0, view_, cfg::VIEW_COUNT, frac, air_);
    } else {
      drawHeader(spr, w, wifiOk, nowMs);
      drawProgress(spr, nowMs);
    }
  }
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
    } else if (settings().theme == 3) {
      // V3 "Pasmowy" (spec 7a): BRAK auto-rotacji. Nawigacja recznie dotykiem
      // (touchTapV3/touchDoubleV3). Zamiast rotacji: po 60 s BEZ dotyku kazdy widok
      // wraca do GLOWNEGO (VIEW_NOW). Liczymy od ostatniego STUKNIECIA (lastTouchMs_)
      // — panel-pin bez dotyku (lastTouchMs_==0) NIE wraca, zeby pin z /api/view dalej
      // dzialal (twarde ograniczenie 4). GLOWNY sam z siebie nic nie robi.
      if (view_ != cfg::VIEW_NOW && lastTouchMs_ != 0 &&
          nowMs - lastTouchMs_ >= 60000UL) {
        prevView_ = view_;
        view_ = static_cast<uint8_t>(cfg::VIEW_NOW);
        viewStart_ = nowMs;
        enterStart_ = nowMs;
        v3Sig_ = 0xFFFFFFFFu;
        lastTouchMs_ = 0;   // juz na GLOWNYM — nie odliczaj w kolko
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
      // POWIETRZE bez danych z ZADNEJ stacji (GA17 i GA24) to pusty ekran co minute —
      // pomijamy go tak samo jak RADAR bez opadu i W DOMU bez czujnikow. Skacze na
      // NASTEPNY widok (PAMIEC), bo AIR juz jest ostatnim "zwyklym" ekranem przed
      // trojka serwisowa, ktora nigdy nie jest pomijana.
      if (view_ == cfg::VIEW_AIR && (!air_ || !air_->ready)) {
        view_ = static_cast<uint8_t>(cfg::VIEW_MEM);
      }
      transitioning_ = true;
      transStart_ = nowMs;
      enterStart_ = nowMs;
    }
  }

  const float enterT = clampf(static_cast<float>(nowMs - enterStart_) / cfg::ENTER_ANIM_MS,
                              0.f, 1.f);

  // --- V3: pomijanie przerysowania, gdy nic widocznego sie nie zmienilo -----------
  // loop() wola render() co ~50 ms i BEZWARUNKOWO wypycha bufor na TFT. Na ciemnym
  // tle (V1/V2) przepisanie tych samych pikseli jest niewidoczne; na JASNYM ukladzie
  // V3, na fizycznym ST7789, kazde wypchniecie widac jako blysk odswiezenia — ekran
  // "mrucze" bez przerwy (zgloszone przez wlasciciela: na V1 to samo bylo widac tylko
  // przy animacji Mario). V3 czyta SUROWE modele, stale miedzy pobraniami z sieci, wiec
  // tresc realnie zmienia sie rzadko. Liczymy sygnature tego, co widac; gdy bez zmian —
  // nie rysujemy i nie wypychamy. Radar (dryf chmur), przejscia i alerty rysuja sie zawsze.
  if (settings().theme == 3 && view_ != cfg::VIEW_RADAR && !transitioning_ &&
      !alertActive_ && (nowMs - enterStart_) >= cfg::ENTER_ANIM_MS) {
    uint32_t sig = 2166136261u;
    auto mix = [&](uint32_t x) { sig = (sig ^ x) * 16777619u; };
    mix(view_);
    const time_t nt = time(nullptr);
    mix(nt > 1700000000 ? static_cast<uint32_t>(nt / 60) : 0u);   // minuta (zegar)
    mix(blTarget_);                                               // dzien / polmrok / noc
    mix(static_cast<uint32_t>(pinned_ + 2));
    // Kropka feedbacku dotyku: jej stan (zapalona/zgaszona) wchodzi w sygnature, wiec
    // render NARYSUJE ja przy zapaleniu i SKASUJE przy zgasnieciu (~600 ms) — inaczej
    // pominiecie przerysowania zostawiloby ja na ekranie. Rysuje ja drawV3 (WeatherUiV3).
    mix(rawTouchMs_ != 0 && nowMs - rawTouchMs_ < 600u ? 0xD07u : 0u);
    // Ekrany diagnostyczne pokazuja zywe liczby (heap/temp/fps) — odswiezaj co 2 s,
    // zeby sie aktualizowaly, ale nie 20x/s.
    if (view_ == cfg::VIEW_MEM || view_ == cfg::VIEW_MOTION || view_ == cfg::VIEW_STATS)
      mix(nowMs / 2000);
    mix(static_cast<uint32_t>(static_cast<int>(w.current.tempC * 10)) ^
        (static_cast<uint32_t>(w.current.weatherCode) << 16) ^ (w.current.isDay ? 1u : 0u));
    mix(static_cast<uint32_t>(static_cast<int>(w.current.feelsC * 10)) ^
        (static_cast<uint32_t>(w.current.precipProb) << 8) ^ static_cast<uint32_t>(w.current.humidity));
    mix(static_cast<uint32_t>(pv.data.powerAcW) ^ (static_cast<uint32_t>(pv.data.gridPowerW) << 1) ^
        static_cast<uint32_t>(pv.data.energyTodayKwh * 100) ^ (pv.online ? 0x40000000u : 0u));
    if (air_) mix(air_->sampleEpoch ^ (static_cast<uint32_t>(air_->index) << 24) ^
                  static_cast<uint32_t>(air_->pm25 * 10));
    if (roomModel_) {
      mix(static_cast<uint32_t>(roomModel_->count) | (static_cast<uint32_t>(roomModel_->sensorCount) << 8));
      for (int i = 0; i < roomModel_->count && i < 6; ++i)
        mix(static_cast<uint32_t>(static_cast<int>(roomModel_->rows[i].tempC * 10)) ^
            ((roomModel_->rows[i].ageS / 60) << 16));
    }
    if (boiler_) mix(static_cast<uint32_t>(static_cast<int>(boiler_->dhwTempC * 10)) ^
                     (boiler_->burnerActive ? 1u : 0u) ^
                     (static_cast<uint32_t>(boiler_->modulationPct) << 8) ^ boiler_->okAt);
    mix(static_cast<uint32_t>(fl.count) | (static_cast<uint32_t>(fl.total) << 8) | (fl.ready ? 0x10000u : 0u));
    if (fl.count > 0) mix(static_cast<uint32_t>(fl.list[0].altFt) ^ (static_cast<uint32_t>(fl.list[0].gs) << 16));
    if (sig == v3Sig_) {
      tickBacklight();
      return blCurrent_ != blTarget_;   // dalej tylko po to, by dokonczyc rampe jasnosci
    }
    v3Sig_ = sig;
  } else if (settings().theme == 3) {
    v3Sig_ = 0xFFFFFFFFu;   // po radarze/przejsciu/alercie wymus przerysowanie nastepnej stabilnej klatki
  }

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
    // Test podswietlenia przejmuje CALY ekran — to narzedzie diagnostyczne, a nie
    // widok w rotacji: pokazuje wylacznie liczbe PWM, pasek i odliczanie do konca.
    // Sam wygasa (blSweepUntil_), wiec nie ma jak zostac na stale.
    if (backlightSweepActive(nowMs)) {
      drawBacklightSweep(spr_, nowMs);
    } else {
      paintFrame(spr_, w, pv, hist, fl, wifiOk, nowMs, heapNow);
    }
    const uint32_t t1 = micros();
    spr_.pushSprite(0, top);
    tPaint += t1 - t0;
    tPush += micros() - t1;
  }
  spr_.resetViewport();

  // srednia krocząca — pojedyncza klatka potrafi zlapac przerwanie WiFi
  diag().frameDrawUs = (diag().frameDrawUs * 7 + tPaint) / 8;
  diag().framePushUs = (diag().framePushUs * 7 + tPush) / 8;

  // RETRO zajmuje caly dolny pasek (y=206..239) wlasnym HUD-em (WILGOC/WIATR/HPA) —
  // patrz komentarz przy drawViewRetroFooter. Poza tym widokiem dokladnie stara
  // sciezka: drawFooter(), bez zadnej zmiany.
  if (settings().theme == 3) {
    // V3 rysuje dolny pas (206..239) sam, wprost na TFT — uklad V3 siega pelnej
    // wysokosci (POWIETRZE na glownym, osie wykresow), a tego nie da sie zmiescic w
    // sprite 206 px. To ten sam mechanizm co footer RETRO.
    drawV3Bottom(tft_, view_, w, pv, fl, nowMs, heapNow);
    footerInit_ = false;   // stopka PV moze byc nieaktualna po powrocie na V1/V2 — patrz nizej
  } else if (view_ == cfg::VIEW_RETRO) {
    drawViewRetroFooter(tft_, w);
    // Cache w drawFooter() (lastAc_/lastGrid_/...) nic nie wie o tym, ze fizyczne
    // piksele pod nim wlasnie pokazywaly zupelnie inny widok. Bez tego resetu, gdy
    // po powrocie z RETRO wartosci PV akurat sie nie zmienily, drawFooter()
    // uznaloby "nic nowego" i NIE przemalowalby stopki — zostalyby na ekranie
    // piksele HUD-u RETRO pod (juz innym) aktualnym widokiem.
    footerInit_ = false;
  } else {
    drawFooter(pv, wifiOk);   // poza buforem, wprost na TFT
  }
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

void WeatherUi::drawViewRadar(TFT_eSPI& spr, int ox, float t, const WeatherModel& w,
                               uint32_t nowMs) {
  // `w` i `nowMs` sa tu od v126 NIEUZYWANE — i to jest cala miara tej zmiany.
  // Wybor klatki animacji (z nowMs), jej wiek i wektor przesuniecia chmur (z wiatru
  // w `w.current`) licza sie teraz w warstwie danych: buildRadarModel() w
  // pogoda-gdynia.ino. Tam tez wedrowal ZAPIS do diagnostyki radaru — funkcja
  // rysujaca nie ma prawa mutowac globalnego stanu, tym bardziej ze ten sam kod
  // rysuje takze zrzut BMP z webTask, czyli z DRUGIEGO rdzenia.
  //
  // Parametry zostaja w sygnaturze, bo drawView() woła wszystkie widoki jednakowo
  // (i tak samo woła wariant V2, ktory doklada tylko wiersz tytulu) — wycinanie ich
  // z JEDNEGO widoku rozjechaloby ten uklad, a niczego nie kupuje.
  (void)w;
  (void)nowMs;

  const float e = easeOutCubic(t);

  static const RadarViewModel kEmpty{};
  const RadarViewModel& rm = radarModel_ ? *radarModel_ : kEmpty;

  const int n = rm.frames;
  if (n == 0) {
    viewHeader(spr, ox, "pobieram...");
    drawNoData(spr, ox, "Pobieram mapę opadów", rm.error);
    return;
  }

  // Klatka, ktora idzie na ekran, i przesuniecie PROBKOWANIA jej rastra. Oba sa
  // STALE w calej tej klatce — tak samo jak wtedy, gdy liczyly sie tutaj — wiec
  // petla opadu nizej dalej moze isc CIAGAMI jednakowego poziomu zamiast pikselami.
  const int fi = rm.frameIdx;
  const int sx = rm.shiftX;
  const int sy = rm.shiftY;

  // --- mapa na PELNA szerokosc (v110: gmapr, 300 km, nie gmapw/111 km — gmapw
  // zostaje wylacznie dla ekranu samolotow, patrz MapDataWide.h i drawViewFlights) ---
  // Ekran samolotow dzieli miejsce z lista lotow i musi miec 224 px. Radar nie ma
  // czego dzielic, wiec dostaje caly ekran — inaczej mapa byla przycieta z bokow.
  const int mx = ox;
  const int my = CY;

  spr.fillRect(mx, my, gmapr::MAP_W, gmapr::MAP_H, col::MAP_SEA);
  for (int row = 0; row < gmapr::MAP_H; ++row) {
    const uint16_t a = pgm_read_word(&gmapr::LAND_ROW_OFF[row]);
    const uint16_t b = pgm_read_word(&gmapr::LAND_ROW_OFF[row + 1]);
    for (uint16_t k = a; k < b; ++k) {
      const uint16_t x0 = pgm_read_word(&gmapr::LAND_SPANS[k][0]);
      const uint16_t x1 = pgm_read_word(&gmapr::LAND_SPANS[k][1]);
      spr.drawFastHLine(mx + x0, my + row, x1 - x0 + 1, col::MAP_LAND);
    }
  }

  // --- warstwa opadu ---
  // Rysujemy CIAGAMI, nie pikselami: przy 320x172 = 55 tys. pikseli pojedyncze
  // drawPixel zjadloby budzet klatki (21 ms) w calosci. (Tu bylo kiedys "224x172 =
  // 38 tys." — 224 to szerokosc mapy SPRZED przejscia na szeroka gmapw; od v110
  // radar rysuje na gmapr, ktora ma te sama wielkosc rastra co gmapw, 320x172.)
  // Przesuniecie (sx,sy) jest STALE w calej klatce (przychodzi gotowe z modelu),
  // wiec rm.levelAt(x-sx, y-sy) dalej daje ciagi jednakowego poziomu — to
  // przesuniecie PROBKOWANIA zrodla, a nie zmiana kosztu per piksel. Odczyt zwraca
  // 0 poza rastrem, wiec ujemne x-sx/y-sy sa bezpieczne z definicji.
  //
  // v126: to jest teraz indeksowanie tablicy WEWNATRZ modelu (RadarData.h), a nie
  // wywolanie radarmap::levelAt() do innej jednostki kompilacji przy KAZDYM z ~110
  // tys. odczytow na klatke. Rachunek jest identyczny, kosztu przybyc nie moglo.
  static const uint16_t kPal[6] = {
      0,                       // 0 — brak opadu
      lerp565(col::MAP_SEA, col::RAIN, 0.55f),   // mzawka: ledwo widoczna
      col::RAIN,                                 // deszcz
      col::WARN,                                 // silny
      col::PV_IMPORT,                            // bardzo silny
      col::ERR,                                  // ulewa
  };

  const int rows = static_cast<int>(gmapr::MAP_H * e);   // animacja wejscia: opad "wchodzi"
  for (int y = 0; y < rows; ++y) {
    int x = 0;
    while (x < gmapr::MAP_W) {
      const uint8_t lv = rm.levelAt(x - sx, y - sy);
      if (lv == 0) {
        ++x;
        continue;
      }
      int x2 = x + 1;
      while (x2 < gmapr::MAP_W && rm.levelAt(x2 - sx, y - sy) == lv) ++x2;
      spr.drawFastHLine(mx + x, my + y, x2 - x, kPal[lv > 5 ? 5 : lv]);
      x = x2;
    }
  }

  // --- KONTUR LADU NA WIERZCHU ---
  // Bez tego opad zamalowuje wybrzeze i mapa staje sie kolorowa plama, z ktorej
  // nie da sie odczytac, GDZIE pada. Rysujemy tylko krawedzie pasow ladu (lewa
  // i prawa), wiec kosztuje to dwa piksele na pas, a nie caly ląd.
  for (int row = 0; row < gmapr::MAP_H; ++row) {
    const uint16_t a = pgm_read_word(&gmapr::LAND_ROW_OFF[row]);
    const uint16_t b = pgm_read_word(&gmapr::LAND_ROW_OFF[row + 1]);
    for (uint16_t k = a; k < b; ++k) {
      const uint16_t x0 = pgm_read_word(&gmapr::LAND_SPANS[k][0]);
      const uint16_t x1 = pgm_read_word(&gmapr::LAND_SPANS[k][1]);
      // Piksel na KRAWEDZI KADRU to nie wybrzeze, tylko miejsce, w ktorym mapa sie
      // konczy. Rysowany dawal pionowa biala krechę wzdluz calego lewego brzegu,
      // bo lad (Pomorze) dochodzi tam do granicy obrazu.
      if (x0 > 0) spr.drawPixel(mx + x0, my + row, col::MAP_COAST_HI);
      if (x1 < gmapr::MAP_W - 1) spr.drawPixel(mx + x1, my + row, col::MAP_COAST_HI);
    }
  }

  // --- Gdynia ---
  int gx = 0, gy = 0;
  {
    const float lat = settings().lat, lon = settings().lon;
    gx = mx + static_cast<int>((lon - gmapr::LON_MIN) / (gmapr::LON_MAX - gmapr::LON_MIN) *
                               gmapr::MAP_W);
    gy = my + static_cast<int>((gmapr::LAT_MAX - lat) / (gmapr::LAT_MAX - gmapr::LAT_MIN) *
                               gmapr::MAP_H);
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
  if (rm.frameMin >= -1) {
    snprintf(hdr, sizeof(hdr), "teraz");
  } else {
    snprintf(hdr, sizeof(hdr), "%ld min temu", static_cast<long>(-rm.frameMin));
  }
  viewHeader(spr, ox, hdr);

  // --- os czasu (na dole, na mapie) ---
  const int ty = CY + gmapr::MAP_H - 20;
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
  if (rm.frameEpoch > 0) {
    const time_t tt = static_cast<time_t>(rm.frameEpoch);
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

  // Wiersze przychodza GOTOWE z warstwy danych (RoomModel, patrz RoomData.h):
  // nazwa jest juz rozwiazana (wpis z ustawien po MAC), RSSI jest juz WYBRANY
  // lepszy z dwoch zrodel, a wiek probki policzony raz, z tego samego nowMs, co
  // cala klatka. Do v125 stala tutaj kopia tego wszystkiego — razem z progiem
  // swiezosci 90 s i dwoma wolaniami millis() w petli zbierajacej kafelki.
  //
  // Bierzemy WSKAZNIKI do wierszy, nie ich kopie: model zyje dluzej niz ta funkcja
  // (odswieza go loop()), wiec kopiowanie 6 x 24 B na kazda klatke nie kupiloby nic.
  static const RoomModel kNoRooms{};
  const RoomModel& rmod = roomModel_ ? *roomModel_ : kNoRooms;

  // V1 pokazuje TYLKO czujniki, ktore maja slot w Settings — a wiec kolor kafelka
  // i wiersz w historii. Czujnik bez wpisu, albo wpisany w slot 6-7 (poza
  // RoomHistory::ROOMS), jest tu pomijany dokladnie tak, jak dotad; wariant V2
  // rysuje go z MAC-iem zamiast nazwy. Ten filtr ZOSTAJE w rysowaniu, bo to jest
  // decyzja o ukladzie (nie ma dla niego ani koloru, ani miejsca na wykresie),
  // a nie o danych.
  const RoomRow* rooms[RoomHistory::ROOMS];
  int n = 0;
  for (int i = 0; i < rmod.count && n < RoomHistory::ROOMS; ++i) {
    if (rmod.rows[i].slot < 0) continue;
    rooms[n++] = &rmod.rows[i];
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
    const RoomRow& r = *rooms[i];
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
    const int rs = r.rssi;
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
    snprintf(hs, sizeof(hs), r.hasHum ? "%.0f%%" : "--", r.humidity);
    const uint16_t hc = !r.hasHum ? col::TEXT_MUTE
                        : (r.humidity > 65.f || r.humidity < 30.f)
                            ? col::WARN
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
      const int16_t v = rh.t10[rooms[i]->slot][rh.idx(k)];
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
    const int r = rooms[i]->slot;
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

// ------------------------------------------------------- WIDOK: POWIETRZE (v117) --
// Miejska siec czujnikow Gdyni (ARMAAG/sensorbox.pl) — GA17 (Sandomierska 3, Maly
// Kack) to stacja GLOWNA wlasciciela, GA24 (Halicka 8) automatyczny ZAPAS. Pobieranie,
// fallback GA17->GA24 i indeks licza sie w AirClient.cpp/AirData.h — ten ekran TYLKO
// rysuje gotowy AirModel, dokladnie jak PV/loty czytaja gotowy PvModel/FlightModel.
//
// STACJA MUSI BYC WIDOCZNA — to nie jest kosmetyka. Gdyby ekran cicho pokazywal
// liczby z GA24 (Halicka), wlasciciel patrzylby na pomiar spod innego adresu, myslac,
// ze to czujnik "pod nosem" (Sandomierska). Ta sama zasada, co litera E/S przy
// czujnikach BLE (patrz WeatherUi::drawViewHome) — KTO naprawde dostarczyl pomiar
// musi byc czytelne na pierwszy rzut oka, nigdy domyslne.
void WeatherUi::drawViewAir(TFT_eSPI& spr, int ox, float t, const WeatherModel& w) {
  const float e = easeOutCubic(t);
  static const AirModel kEmptyAir{};
  const AirModel& am = air_ ? *air_ : kEmptyAir;

  if (!am.ready) {
    // render() i drawProgress() JUZ pomijaja ten widok w rotacji, gdy !ready (patrz
    // warunek `skipped` w obu miejscach, tak samo jak przy RADAR bez opadu) — ta
    // galaz odpala sie wiec tylko po recznym przypieciu z panelu WWW (pinView),
    // gdzie pomijanie automatyczne nie obowiazuje.
    drawNoData(spr, ox, "Brak danych: GA17 i GA24",
               am.errorMsg[0] ? am.errorMsg : "sprawdzam ponownie za chwilę");
    return;
  }

  // --- wiek najswiezszej probki PM ("X min temu") — liczony z EPOCH (czas pomiaru
  // na stacji), NIE z millis() naszego ostatniego fetch'a (ten drugi jest osobno w
  // /api/diag jako air.ok_ago_s — patrz Portal.cpp). To rozne pytania: "jak dawno MY
  // sie polaczylismy" kontra "jak stary jest POMIAR, na ktory patrzysz". ---
  const time_t nowT = time(nullptr);
  char ageStr[24];
  snprintf(ageStr, sizeof(ageStr), "wiek nieznany");
  bool stale = false;
  if (am.sampleEpoch > 0 && nowT > 1700000000 &&
      nowT >= static_cast<time_t>(am.sampleEpoch)) {
    const uint32_t ageS = static_cast<uint32_t>(nowT - static_cast<time_t>(am.sampleEpoch));
    stale = ageS > AIR_STALE_S;   // ta sama granica, co przy decyzji GA17->GA24
    if (ageS < 120) {
      snprintf(ageStr, sizeof(ageStr), "%lus temu", static_cast<unsigned long>(ageS));
    } else if (ageS < 3600) {
      snprintf(ageStr, sizeof(ageStr), "%lu min temu", static_cast<unsigned long>(ageS / 60));
    } else {
      snprintf(ageStr, sizeof(ageStr), "%.1f h temu", ageS / 3600.f);
    }
  }
  viewHeader(spr, ox, ageStr, stale ? col::WARN : col::TEXT_MUTE);

  // --- stacja: od razu pod belka, pelnym PLF14 (nie malym gl()) — ma byc NIEMOZLIWE
  // do przeoczenia, patrz uzasadnienie nad funkcja. Kolor WARN przy zapasie: nie tylko
  // slowo "(ZAPAS)", ale i barwa ma krzyczec "to NIE jest Twoja stacja". ---
  char stLbl[32];
  snprintf(stLbl, sizeof(stLbl), am.usingFallback ? "%s (ZAPAS)" : "%s", am.stationName);
  plStr(spr, PLF14, stLbl, ox + 10, 58, am.usingFallback ? col::WARN : col::TEXT);

  // --- indeks ogolny: duzy napis slowny, kolor = klasa ARMAAG (patrz airIndexColor) ---
  plCenter(spr, PLF18, airIndexName(am.index), ox + W / 2, 94, airIndexColor(am.index));

  // --- dwie karty PM10 / PM2.5 — kolor KAZDEJ to jej WLASNY czastkowy indeks (nie
  // indeks ogolny!), zeby bylo widac, KTORY skladnik naprawde ustala wynik ogolny
  // (zawsze ten sam, GORSZY z dwoch — patrz uzasadnienie "maksimum, nie srednia" przy
  // koncu AirClient::parsePayload). Wzorzec kafelkow jak w drawViewPv/drawViewBoiler.
  struct Card {
    const char* label;
    bool has;
    float v;
    int idx;
  };
  const Card cards[2] = {
      {"PM10", am.hasPm10, am.pm10, am.indexPm10},
      {"PM2.5", am.hasPm25, am.pm25, am.indexPm25},
  };

  const int cy0 = 108, chh = 56;
  const int cw = (W - 20 - 10) / 2;   // 2 karty na szerokosc, 10 px marginesu, 10 px odstepu
  for (int i = 0; i < 2; ++i) {
    const int x = ox + 10 + i * (cw + 10);
    const uint16_t cc = cards[i].has ? airIndexColor(cards[i].idx) : col::TEXT_MUTE;
    const int grow = static_cast<int>(chh * clampf(e * 1.4f - i * 0.12f, 0.f, 1.f));
    if (grow < 4) continue;
    spr.fillRoundRect(x, cy0 + (chh - grow), cw, grow, 8, col::BG_CARD);
    if (grow < chh - 2) continue;
    spr.fillRoundRect(x, cy0, 3, chh, 1, cc);
    plStr(spr, PLF14, cards[i].label, x + 10, cy0 + 18, col::TEXT_DIM);
    if (cards[i].has) {
      char val[10];
      snprintf(val, sizeof(val), "%.1f", cards[i].v);
      const int vw = pltxt::drawString(spr, PLF18, val, x + 10, cy0 + 44, cc, cc);
      // ASCII "ug/m3", NIE "µg/m³": PlFont10/14/18 nie maja glifu U+00B5 (mikro), a
      // PLF14/18 nie maja tez U+00B3 (superskrypt 3) — sprawdzone wprost w tablicach
      // kodow (PlFont*Codepoints). Brakujacy glif nie wywala programu (drawString po
      // prostu go pomija, patrz PlText.h), ale cichy brak "µ" wygladalby na literowke,
      // nie na swiadoma decyzje. ASCII dziala wszedzie i nie wymaga dorabiania
      // bitmapowych glifow do trzech czcionek dla jednej etykiety jednostki.
      plStr(spr, PLF14, "ug/m3", x + 15 + vw, cy0 + 44, col::TEXT_MUTE);
    } else {
      plStr(spr, PLF14, "brak danych", x + 10, cy0 + 44, col::TEXT_MUTE);
    }
  }

  // --- porownanie z nasza prognoza (opcjonalne, patrz zadanie) — tylko gdy stacja
  // daje TA/RH/PA, czyli TYLKO na GA17 (patrz AirData.h::hasWeather: GA24 tych
  // zmiennych w ogole nie odpytujemy). ---
  if (am.hasWeather) {
    char cmp[64];
    snprintf(cmp, sizeof(cmp), "stacja: %.0f°C  %.0f%%  %.0f hPa", am.tempC, am.rh,
             am.pressureHpa);
    gl(spr, cmp, ox + 10, 178, col::TEXT_DIM);
    if (w.current.valid) {
      char ours[24];
      snprintf(ours, sizeof(ours), "u nas: %.0f°C", w.current.tempC);
      glRight(spr, ours, ox + W - 10, 178, col::TEXT_MUTE);
    }
  }
}

// ------------------------------------------------------- WIDOK: PAMIĘĆ (v111) --
// Ekran eksploracyjny: właściciel wprost poprosił o WSZYSTKIE rodzaje pamięci na
// jednym ekranie, żeby ocenić wizualnie, co z tego zostawić na stałe. Stąd dziewięć
// wierszy, nie trzy — to celowe rozrośnięcie, nie przypadkowe.
//
// KAŻDA liczba tu woła prawdziwe API ESP-IDF/Arduino W MIEJSCU RYSOWANIA — żadna
// nie jest zgadywana ani przepisana z noty katalogowej. Jedyny wyjątek to
// `heapNow`: ten JEDEN parametr przychodzi ze stosu wołającego (paintFrame),
// dokładnie jak w drawViewStats — bo o to samo chodzi w komentarzu przy
// paintFrame ("JEDNA klatka = JEDEN moment"): wiersz "SRAM" ma pokazywać TĘ SAMĄ
// liczbę, co karta "WOLNY SRAM" na ekranie STATYSTYKI, nawet gdy oba paski BMP
// zrzutu ekranu dzieli od siebie transmisja HTTP. Reszta (fragmentacja sterty,
// PSRAM, flash, tabela partycji, RTC) zmienia się dużo wolniej albo wcale (tabela
// partycji jest stała od startu urządzenia) — czytamy ją na żywo, tak samo jak
// drawViewStats na żywo czyta WiFi.RSSI() czy pola Diag.
void WeatherUi::drawViewMem(TFT_eSPI& spr, int ox, float t, uint32_t heapNow) {
  const float e = easeOutCubic(t);
  const Diag& d = diag();

  viewHeader(spr, ox, "WSZYSTKIE RODZAJE");

  int y = 52;
  constexpr int ROW_H = 17;   // ten sam odstęp, co lista źródeł na ekranie STATYSTYKI

  // Wiersz: nazwa po lewej (gl, przygaszona), liczby po prawej (glRight), opcjonalny
  // pasek zapełnienia pod spodem. Pasek WSZĘDZIE pokazuje ułamek WOLNEGO miejsca —
  // pełny = dużo zapasu. Jedyny wyjątek to APP (OTA) niżej: tam liczy się zajętość
  // partycji (bo pytanie brzmi "ile zjada firmware", nie "ile zostało"), ale i tak
  // przekazuje się tu `frac` jako "wolne", a odwrócenie robi się w miejscu wywołania.
  auto row = [&](const char* name, const char* text, bool hasBar, float frac,
                uint16_t barColor) {
    gl(spr, name, ox + 10, y, col::TEXT_MUTE);
    glRight(spr, text, ox + W - 10, y, col::TEXT);
    if (hasBar) {
      const int bx = ox + 10, by = y + 12, bw = W - 20, bh = 3;
      spr.fillRoundRect(bx, by, bw, bh, 1, col::PV_TRACK);
      const int fw = static_cast<int>(bw * clampf(frac, 0.f, 1.f) * e);
      if (fw > 0) spr.fillRoundRect(bx, by, fw, bh, 1, barColor);
    }
    y += ROW_H;
  };

  // --- 1. SRAM wewnętrzny (DRAM) ---
  {
    // heap_caps_get_largest_free_block: NAJWIĘKSZY pojedynczy kawałek, jaki da się
    // jeszcze zaalokować JEDNYM wywołaniem — to jest FRAGMENTACJA. Gdy jest sporo
    // mniejszy niż "wolne", sterta jest podziurawiona: sumarycznie miejsca nie
    // brakuje, ale duża alokacja (bufor TLS, dekoder PNG radaru) może się nie zmieścić.
    const uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    // heap_caps_get_minimum_free_size: dołek OD STARTU urządzenia (nie od teraz) —
    // ten sam sens, co biała kreska na wskaźniku "WOLNY SRAM" na ekranie STATYSTYKI.
    const uint32_t minEver = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    char val[56];
    snprintf(val, sizeof(val), "wolne %lu  blok %lu  dołek %lu kB",
             static_cast<unsigned long>(heapNow / 1024),
             static_cast<unsigned long>(largest / 1024),
             static_cast<unsigned long>(minEver / 1024));
    const uint16_t c = heapNow < cfg::HEAP_DANGER  ? col::ERR
                       : heapNow < cfg::HEAP_WARN  ? col::WARN
                                                    : col::OK;
    // Skala paska to HEAP_FULL — TA SAMA "pełna skala wskaźnika", której używa
    // zoneGauge() na ekranie STATYSTYKI. Nie nowa liczba, ten sam punkt odniesienia.
    row("SRAM", val, true, static_cast<float>(heapNow) / cfg::HEAP_FULL, c);
  }

  // --- 2. PSRAM (2 MB, dźwiga bufor ekranu i dekoder PNG radaru od v50) ---
  {
    const uint32_t total = ESP.getPsramSize();
    const uint32_t freeP = ESP.getFreePsram();
    const uint32_t minEver = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    char lbl[24], fS[16], mS[16];
    fmtBytes(fS, sizeof(fS), freeP);
    fmtBytes(mS, sizeof(mS), minEver);
    char tS[16];
    fmtBytes(tS, sizeof(tS), total);
    snprintf(lbl, sizeof(lbl), "PSRAM %s", tS);
    char val[56];
    snprintf(val, sizeof(val), "wolne %s  dołek %s", fS, mS);
    const float frac = total ? static_cast<float>(freeP) / total : 0.f;
    // Brak kalibrowanych progów dla PSRAM (Config.h ma je tylko dla sterty
    // wewnętrznej) — kolor jest neutralny, nie stanowy.
    row(lbl, val, total > 0, frac, col::ACCENT);
  }

  // --- 3. Flash — cały układ scalony (4 MB w tym płycie) ---
  {
    const uint32_t chipSz = diag().flashBytes;
    // Rozmiar firmware'u czytamy z diag() (policzony raz w setup()), NIE wołamy tu
    // ESP.getSketchSize(): ta funkcja skanuje flash i w loop() podczas rysowania (SPI
    // zajety przez TFT) zwracala 0 — ekran pokazywal "firmware 0 kB". Patrz Diag w Log.h.
    const uint32_t sketchSz = diag().sketchBytes;
    char fS[16], cS[16];
    fmtBytes(fS, sizeof(fS), sketchSz);
    fmtBytes(cS, sizeof(cS), chipSz);
    char val[56];
    snprintf(val, sizeof(val), "firmware %s  chip %s", fS, cS);
    const float frac = chipSz ? static_cast<float>(chipSz - sketchSz) / chipSz : 0.f;
    row("FLASH", val, chipSz > 0, frac, col::ACCENT);
  }

  // --- 4. Partycja APP aktywna TERAZ — ile z WŁASNEGO slotu OTA (~1,9 MB) zajmuje ---
  {
    const esp_partition_t* run = esp_ota_get_running_partition();
    const uint32_t sketchSz = diag().sketchBytes;   // z setup(), nie skan w rysowaniu
    const uint32_t runSz = run ? run->size : 0;
    const int pct = runSz ? static_cast<int>((static_cast<uint64_t>(sketchSz) * 100) / runSz) : 0;
    char val[56];
    snprintf(val, sizeof(val), "%s: %lu/%lu kB (%d%%)", run ? run->label : "?",
             static_cast<unsigned long>(sketchSz / 1024),
             static_cast<unsigned long>(runSz / 1024), pct);
    // Progi 75%/90% SĄ ARBITRALNE — Config.h nie definiuje granicy zapełnienia
    // partycji OTA (to inny rodzaj zasobu niż sterta). Tylko orientacyjne
    // podświetlenie rosnącego zapełnienia, NIE zmierzony limit jak HEAP_*/CPU_T_*.
    const uint16_t c = pct >= 90 ? col::ERR : pct >= 75 ? col::WARN : col::OK;
    const float freeFrac = runSz ? 1.f - static_cast<float>(sketchSz) / runSz : 0.f;
    row("APP (OTA)", val, runSz > 0, freeFrac, c);
  }

  // --- 5. Obie połówki OTA — dlatego nowy firmware zawsze mieści się na drugiej ---
  {
    const esp_partition_t* app0 = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
    const esp_partition_t* app1 = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
    char val[56];
    snprintf(val, sizeof(val), "app0 %lu  app1 %lu kB (2x OTA)",
             static_cast<unsigned long>(app0 ? app0->size / 1024 : 0),
             static_cast<unsigned long>(app1 ? app1->size / 1024 : 0));
    row("PARTYCJE", val, false, 0.f, col::TEXT);
  }

  // --- 6. Partycje danych: NVS (ustawienia), SPIFFS (istnieje, ale NIEUŻYWANY —
  // pokazujemy rozmiar właśnie po to, żeby było widać rezerwację), coredump
  // (zrzut awaryjny — szczegóły w /api/coredump) ---
  {
    const esp_partition_t* nvs = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, nullptr);
    const esp_partition_t* spiffs = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
    const esp_partition_t* core = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
    char val[56];
    snprintf(val, sizeof(val), "nvs %lu  spiffs %lu(nieuż.)  core %lu kB",
             static_cast<unsigned long>(nvs ? nvs->size / 1024 : 0),
             static_cast<unsigned long>(spiffs ? spiffs->size / 1024 : 0),
             static_cast<unsigned long>(core ? core->size / 1024 : 0));
    row("DANE", val, false, 0.f, col::TEXT);
  }

  // --- 7. RTC SLOW — pamięć, która PRZEŻYWA OTA (gPir/gLdr, patrz Log.h) ---
  {
    // 7680 B to realny rozmiar sekcji .rtc_noinit, w której siedzą gPir/gLdr —
    // 8192 B to fizyczny rozmiar RTC SLOW (stała sprzętowa ESP32-S3); 512 B niżej
    // zajmuje coś innego (potwierdzone przez nm/symbole linkera — patrz uzasadnienie
    // w Log.h przy PirRtc, nie jest to zgadywane).
    constexpr uint32_t kRtcSlowUsable = 7680;
    constexpr uint32_t kRtcFastPhysical = 8192;  // NIEUŻYWANE w tym projekcie — patrz Log.h
    const uint32_t used = sizeof(PirRtc) + sizeof(LdrRtc);   // gPir + gLdr
    char val[56];
    snprintf(val, sizeof(val), "%lu/%lu B  FAST %lu B nieuż.",
             static_cast<unsigned long>(used),
             static_cast<unsigned long>(kRtcSlowUsable),
             static_cast<unsigned long>(kRtcFastPhysical));
    row("RTC SLOW", val, true, 1.f - static_cast<float>(used) / kRtcSlowUsable, col::OK);
  }

  // --- 8. ROM (bootrom) — stała sprzętowa ESP32-S3, tylko do odczytu, kodu
  // własnego projektu tu NIE MA. Nie da się zmierzyć "wolnego miejsca" (bo nic
  // tu nie alokujemy) — pokazane jako ciekawostka, nie jako zasób do zarządzania.
  row("ROM", "384 kB, tylko odczyt (bootrom)", false, 0.f, col::TEXT);

  // --- 9. Stos zadań — to samo źródło, co /api/diag mem.stack_*_spare ---
  {
    // 16384 B to rozmiar zadany w obu xTaskCreatePinnedToCore(...,16384,...,&gWebTask,0)
    // / (...,16384,...,&gNetTask,0) w pogoda-gdynia.ino. Nie ma na to wspólnej stałej
    // w Config.h — jeśli te wywołania kiedyś zmienią rozmiar, tę liczbę (i próg
    // niżej) trzeba poprawić ręcznie.
    constexpr uint32_t kTaskStackBytes = 16384;
    char val[56];
    snprintf(val, sizeof(val), "net %lu  web %lu B (z %lu)",
             static_cast<unsigned long>(d.stackNet), static_cast<unsigned long>(d.stackWeb),
             static_cast<unsigned long>(kTaskStackBytes));
    const uint32_t worst = d.stackNet < d.stackWeb ? d.stackNet : d.stackWeb;
    const float worstFrac = worst / static_cast<float>(kTaskStackBytes);
    // Progi orientacyjne (nie ma zmierzonego limitu w Config.h): poniżej 15%
    // zapasu robi się niepokojąco blisko przepełnienia stosu, poniżej 30% — warto
    // obserwować.
    const uint16_t c = worstFrac < 0.15f ? col::ERR : worstFrac < 0.3f ? col::WARN : col::OK;
    row("STOS", val, true, worstFrac, c);
  }
}

// ------------------------------------------------------- WIDOK: RUCH (v111) ----
// PIR (rytm doby — kiedy chodzimy do łazienki) + LDR (jasność, zdarzenia
// "zostawione światło") + wydajność rysowania (fps) — trzy różne pomiary na
// jednym ekranie eksploracyjnym.
//
// Dane PIR/LDR JUŻ ISTNIEJĄ (gPir/gLdr, RTC SLOW, zbierane od v107/v108) — tu NIC
// nie liczymy od nowa, tylko czytamy to, co już zbiera loop()/pirIsr(). gPir i
// gLdr są pisane na RDZENIU 1 (loop() + ISR), a to rysowanie też leci na
// rdzeniu 1 (WeatherUi::render() woła się z loop()) — TEN SAM rdzeń, więc odczyt
// tutaj jest bezpieczny bez żadnej blokady. To dokładnie to samo założenie, co
// przy odczycie tych samych pól do /api/diag na rdzeniu 0 (webTask) — tam rozjazd
// o jedną próbkę jest już zaakceptowany (patrz komentarze przy gPir w Log.h).
void WeatherUi::drawViewMotion(TFT_eSPI& spr, int ox, float t, uint32_t nowMs) {
  const float e = easeOutCubic(t);
  const Diag& d = diag();

  char hdr[32];
  snprintf(hdr, sizeof(hdr), "%s  %u mV", d.pirState ? "ruch teraz" : "bez ruchu",
           static_cast<unsigned>(d.ldrMv));
  viewHeader(spr, ox, hdr, d.pirState ? col::OK : col::TEXT_MUTE);

  // ==================================================== PIR: rytm doby (24 h) --
  gl(spr, "PIR: RYTM DOBY (24 H)", ox + 10, 50, col::TEXT_MUTE);
  {
    char ago[24];
    if (d.pirLastAt == 0) {
      snprintf(ago, sizeof(ago), "brak od startu");
    } else {
      // nowMs ze stosu (jak w drawViewStats), NIE świeży millis() — inaczej ten
      // napis mógłby pokazać inną wartość w kolejnym pasku zrzutu BMP (patrz
      // komentarz przy paintFrame o "jedna klatka = jeden moment").
      const uint32_t agoS = (nowMs - d.pirLastAt) / 1000;
      if (agoS < 90) {
        snprintf(ago, sizeof(ago), "ruch %lus temu", static_cast<unsigned long>(agoS));
      } else if (agoS < 5400) {
        snprintf(ago, sizeof(ago), "ruch %lu min temu", static_cast<unsigned long>(agoS / 60));
      } else {
        snprintf(ago, sizeof(ago), "ruch %lu h temu", static_cast<unsigned long>(agoS / 3600));
      }
    }
    glRight(spr, ago, ox + W - 10, 50, d.pirState ? col::OK : col::TEXT_DIM);
  }

  {
    constexpr int kBars = 24;
    const int chartX = ox + 10;
    const int chartW = W - 20;           // 300 px
    const int pitch = chartW / kBars;    // 12 px/godzinę
    const int barW = pitch > 3 ? pitch - 3 : 1;
    const int chartTop = 62, chartBase = 100;   // 38 px wysokości

    uint32_t mx = 1;   // >=1, zeby nie dzielic przez zero, gdy jeszcze nic nie przyszlo
    for (int h = 0; h < kBars; ++h) {
      if (gPir.byHour[h] > mx) mx = gPir.byHour[h];
    }
    // Godzina lokalna TERAZ — tylko do podświetlenia słupka, liczona świeżo (jak
    // w drawHeader) — to zegar ścienny, nie coś, co może się rozjechać między
    // paskami zrzutu w sposób, który by komukolwiek zaszkodził.
    int curHour = -1;
    const time_t nowT = time(nullptr);
    if (nowT > 1700000000) {
      struct tm tmv{};
      localtime_r(&nowT, &tmv);
      curHour = tmv.tm_hour;
    }
    for (int h = 0; h < kBars; ++h) {
      const int x = chartX + h * pitch;
      const int hh = static_cast<int>((gPir.byHour[h] / static_cast<float>(mx)) *
                                      (chartBase - chartTop) * e);
      const uint16_t bc = (h == curHour) ? col::ACCENT : col::WIND;
      if (hh > 0) {
        spr.fillRect(x, chartBase - hh, barW, hh, bc);
      } else {
        spr.drawFastHLine(x, chartBase - 1, barW, col::GRID);   // zerowa godzina — kreska bazowa
      }
    }
    for (int h = 0; h <= 18; h += 6) {
      char hb[4];
      snprintf(hb, sizeof(hb), "%d", h);
      glCenter(spr, hb, chartX + h * pitch + pitch / 2, chartBase + 4, col::TEXT_MUTE);
    }

    // "zbieram od" — to horyzont TEGO histogramu, nie uptime (gPir.collectedS
    // przeżywa OTA, uptime zeruje się przy każdym restarcie — patrz Log.h).
    char durBuf[16];
    const uint32_t collS = gPir.collectedS;
    if (collS < 3600) {
      snprintf(durBuf, sizeof(durBuf), "%lu min", static_cast<unsigned long>(collS / 60));
    } else if (collS < 86400) {
      snprintf(durBuf, sizeof(durBuf), "%lu h", static_cast<unsigned long>(collS / 3600));
    } else {
      snprintf(durBuf, sizeof(durBuf), "%lu dni", static_cast<unsigned long>(collS / 86400));
    }
    char b2[32];
    snprintf(b2, sizeof(b2), "zbieram %s", durBuf);
    gl(spr, b2, ox + 10, 114, col::TEXT_MUTE);

    const float pctActive = collS ? (gPir.totalMs / 1000.f) / collS * 100.f : 0.f;
    char b3[40];
    snprintf(b3, sizeof(b3), "wyzwoleń %lu (%.1f%% doby)",
             static_cast<unsigned long>(gPir.rises), pctActive);
    glRight(spr, b3, ox + W - 10, 114, col::TEXT_DIM);
  }

  // ==================================================== LDR: światło ----------
  gl(spr, "LDR: POZIOMY ŚWIATŁA", ox + 10, 124, col::TEXT_MUTE);
  {
    char mvNow[16];
    snprintf(mvNow, sizeof(mvNow), "%u mV", static_cast<unsigned>(d.ldrMv));
    glRight(spr, mvNow, ox + W - 10, 124, col::TEXT_DIM);
  }
  {
    // Pasek 3 kolory = 3 poziomy podświetlenia (ciemno/półmrok/jasno), szerokość
    // proporcjonalna do sekund spędzonych na każdym — patrz LdrRtc::levelS w Log.h.
    const uint32_t l0 = gLdr.levelS[0], l1 = gLdr.levelS[1], l2 = gLdr.levelS[2];
    const uint32_t lsum = l0 + l1 + l2;
    const int bx = ox + 10, by = 136, bw = W - 20, bh = 10;
    if (lsum > 0) {
      int w0 = static_cast<int>((l0 / static_cast<float>(lsum)) * bw * e);
      int w1 = static_cast<int>((l1 / static_cast<float>(lsum)) * bw * e);
      if (w0 < 0) w0 = 0;
      if (w1 < 0) w1 = 0;
      int w2 = static_cast<int>(bw * e) - w0 - w1;
      if (w2 < 0) w2 = 0;
      int xx = bx;
      spr.fillRect(xx, by, w0, bh, col::TEXT_MUTE); xx += w0;
      spr.fillRect(xx, by, w1, bh, col::ACCENT_WARM); xx += w1;
      spr.fillRect(xx, by, w2, bh, col::SUN);
    } else {
      spr.fillRoundRect(bx, by, bw, bh, 2, col::PV_TRACK);   // jeszcze bez danych
    }

    auto fmtH = [](char* buf, size_t n, uint32_t s) {
      if (s < 3600) snprintf(buf, n, "%lu min", static_cast<unsigned long>(s / 60));
      else snprintf(buf, n, "%.1f h", s / 3600.f);
    };
    char h0[12], h1[12], h2[12];
    fmtH(h0, sizeof(h0), l0);
    fmtH(h1, sizeof(h1), l1);
    fmtH(h2, sizeof(h2), l2);
    char lv[56];
    snprintf(lv, sizeof(lv), "ciemno %s  półmrok %s  jasno %s", h0, h1, h2);
    gl(spr, lv, ox + 10, 150, col::TEXT_DIM);
  }

  {
    // Ostatnie zdarzenie "zostawione światło" (pierścień ma do 8 — pełna historia
    // w /api/diag sensors.ldr_events; tu tylko najświeższe, bo na ekranie nie ma
    // miejsca na więcej).
    if (gLdr.evCount == 0) {
      gl(spr, "zdarzenia (zostawione światło): brak", ox + 10, 163, col::TEXT_MUTE);
    } else {
      // evHead to NASTĘPNY slot do nadpisania, więc ostatni ZAPISANY jest jeden wcześniej.
      const uint32_t lastIdx = (gLdr.evHead + 8 - 1) % 8;
      const LdrEvent& ev = gLdr.events[lastIdx];
      const bool open = (gLdr.evSlot == lastIdx);   // wciąż trwa — patrz LdrRtc::evSlot w Log.h
      char durBuf[16];
      if (ev.durS < 3600) {
        snprintf(durBuf, sizeof(durBuf), "%lu min", static_cast<unsigned long>(ev.durS / 60));
      } else {
        snprintf(durBuf, sizeof(durBuf), "%.1f h", ev.durS / 3600.f);
      }
      char line[64];
      if (ev.startEpoch > 0) {
        const time_t st = static_cast<time_t>(ev.startEpoch);
        struct tm tmv{};
        localtime_r(&st, &tmv);
        snprintf(line, sizeof(line), "ostatnie zdarzenie: %02d:%02d, trwało %s%s",
                 tmv.tm_hour, tmv.tm_min, durBuf, open ? " (TRWA)" : "");
      } else {
        snprintf(line, sizeof(line), "ostatnie zdarzenie: godz. nieznana, trwało %s%s",
                 durBuf, open ? " (TRWA)" : "");
      }
      gl(spr, line, ox + 10, 163, open ? col::WARN : col::TEXT_DIM);
    }
  }

  // ==================================================== wydajność rysowania ---
  gl(spr, "WYDAJNOŚĆ RYSOWANIA", ox + 10, 182, col::TEXT_MUTE);
  {
    const double fpsReal = d.framePeriodUs > 0 ? 1000000.0 / d.framePeriodUs : 0.0;
    const uint32_t busyUs = d.frameDrawUs + d.framePushUs;
    const double fpsMax = busyUs > 0 ? 1000000.0 / busyUs : 0.0;
    char perf[64];
    snprintf(perf, sizeof(perf), "%.1f fps (limit ok. %.0f)  %lu+%lu us/klatkę",
             fpsReal, fpsMax, static_cast<unsigned long>(d.frameDrawUs),
             static_cast<unsigned long>(d.framePushUs));
    gl(spr, perf, ox + 10, 194, col::TEXT);
  }
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
                         (v == cfg::VIEW_BOILER && !settings().hasViessmann()) ||
                         (v == cfg::VIEW_AIR && (!air_ || !air_->ready));
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

// ------------------------------------------------------- NAWIGACJA DOTYKIEM V3 --
// Cala trojka nizej dziala WYLACZNIE na sciezce theme==3 (wola ja switch dotyku w
// pogoda-gdynia.ino oraz symulacja z panelu). V1/V2 nie widza tych metod — ich
// dotyk to nadal restartHold()/prevView(), a rotacja ekranu leci auto (patrz render).

void WeatherUi::setViewV3(uint8_t v) {
  prevView_ = view_;
  view_ = v;
  viewStart_ = millis();
  enterStart_ = viewStart_;
  transitioning_ = false;   // V3 nie slajduje — rysuje wprost nowy widok
  pinned_ = -1;             // dotyk zdejmuje przypiecie z panelu (spec 7a: to OK)
  alertActive_ = false;     // jak prevView/pinView — jawna nawigacja gasi plansze
  v3Sig_ = 0xFFFFFFFFu;     // wymus przerysowanie nowego ekranu (omin skip sygnatury)
}

void WeatherUi::touchTapV3() {
  lastTouchMs_ = millis();
  // W diagnostyce 1x przelacza STATS <-> MEM, nie rusza petli glownej (spec 7a).
  if (view_ == cfg::VIEW_STATS || view_ == cfg::VIEW_MEM) {
    setViewV3(view_ == cfg::VIEW_STATS ? cfg::VIEW_MEM : cfg::VIEW_STATS);
    return;
  }
  // PETLA 8 WIDOKOW — kolejnosc ze specyfikacji 7a, NIE numeryczna cfg::VIEW_*.
  // Zrodlo prawdy dla ruchu 1x w V3.
  static constexpr uint8_t kV3Loop[] = {
      cfg::VIEW_NOW, cfg::VIEW_RADAR, cfg::VIEW_DAYS, cfg::VIEW_PV,
      cfg::VIEW_HOME, cfg::VIEW_BOILER, cfg::VIEW_AIR, cfg::VIEW_FLIGHTS};
  constexpr int kN = sizeof(kV3Loop) / sizeof(kV3Loop[0]);
  // Znajdz biezacy widok w petli; jesli go tam nie ma (np. RUCH z panelu albo stan
  // startowy), traktuj jak pozycje GLOWNEGO, wiec pierwszy krok wejdzie za NOW.
  int idx = 0;
  for (int i = 0; i < kN; ++i)
    if (kV3Loop[i] == view_) { idx = i; break; }
  // Nastepny NIEPOMIJANY (viewSkipped: radar bez opadu, pokoje bez czujnikow, piec
  // bez autoryzacji, powietrze bez danych — to samo pyta rotacja V1/V2). Do kN krokow;
  // gdy wszystko inne pominiete, wracamy na biezacy (nie jest pomijany — stoimy na nim).
  for (int step = 0; step < kN; ++step) {
    idx = (idx + 1) % kN;
    if (!viewSkipped(kV3Loop[idx], air_)) break;
  }
  setViewV3(kV3Loop[idx]);
}

void WeatherUi::touchDoubleV3() {
  lastTouchMs_ = millis();
  // 2x w diagnostyce wychodzi na GLOWNY; poza nia 2x wchodzi w diagnostyke (STATS).
  if (view_ == cfg::VIEW_STATS || view_ == cfg::VIEW_MEM)
    setViewV3(static_cast<uint8_t>(cfg::VIEW_NOW));
  else
    setViewV3(static_cast<uint8_t>(cfg::VIEW_STATS));
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
    // Gdy trwa test podswietlenia, zrzut MUSI pokazywac to samo co ekran — inaczej
    // zdalna weryfikacja testu jest zludzeniem (zrzut szedlby inna sciezka rysowania).
    // paintFrame czyści 0..205, drawFooterTo maluje 206..239 — razem cały ekran,
    // więc świeżo wyzerowany sprite nie prześwituje nigdzie na czarno.
    if (backlightSweepActive(nowMs)) {
      drawBacklightSweep(shot, nowMs);
    } else {
      paintFrame(shot, w, pv, hist, fl, wifiOk, nowMs, heapNow);
    }
    // Ten sam widok co live-render: RETRO ma wlasny dolny HUD zamiast stopki PV,
    // a V3 "Pasmowy" — wlasny dolny pas (POWIETRZE na glownym, os radaru itd.)
    // zamiast stopki PV. Bez tego podglad w panelu (i tylko podglad — fizyczny ekran
    // idzie przez render(), ktory juz to ma) pokazywalby pod ukladem V3 stopke V1.
    if (settings().theme == 3) {
      drawV3Bottom(shot, view_, w, pv, fl, nowMs, heapNow);
    } else if (view_ == cfg::VIEW_RETRO) {
      drawViewRetroFooter(shot, w);
    } else {
      drawFooterTo(shot, pv, wifiOk);  // sama sprawdzi, czy wpada w ten pasek
    }

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
