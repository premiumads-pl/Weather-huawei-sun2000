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
// Uzywane WYLACZNIE przez drawViewRetro/drawViewRetroFooter (ekran Mario), ktore po
// usunieciu V1/V2 nie sa juz wolane — zostaja w zrodle do decyzji wlasciciela
// (patrz komentarz przy drawViewRetro w WeatherUi.h). Linker ich nie wciaga.
#include "RetroFont.h"
#include "RetroSprites.h"

// v111: widok PAMIEC czyta te trzy wprost z ESP-IDF (heap_caps_*/partycje/OTA) —
// juz i tak zlinkowane (ESP.getFreeHeap(), Ota.cpp, OtaGuard.cpp korzystaja z tego
// samego), wiec to nie sa nowe zaleznosci, tylko nowe wywolania istniejacego kodu.
#include <esp_heap_caps.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>

// v128/v130: software'owy enkoder JPEG dla podgladu ekranu w panelu. Wersja fmt2jpg
// (komponent kamery espressif__esp32-camera) ZOSTALA ZASTAPIONA biblioteka JPEGENC
// (Larry Bank). Powod: samo ZLINKOWANIE fmt2jpg wciagalo ~8 kB STATYCZNEGO RAM (statyczne
// tablice Huffmana/kwantyzacji enkodera jpge, file-static w libie kamery) -- nie do
// przeniesienia do PSRAM/heap. Zmierzone: RAM statyczny szedl wtedy do 81800 B, czyli
// PONAD projektowy budzet 76000 B. Przy zmierzonym largest_free_block ~39 kB te 8 kB
// zbijaly najwiekszy wolny blok ponizej progu TLS (~40 kB) -- grozba zablokowania OTA
// (urzadzenie jest TYLKO na OTA, bez USB). Bariera 76000 w tools/release.sh przed tym chroni.
//
// JPEGENC trzyma tablice Huffmana/kwantyzacji jako 'const ... PROGMEM' -> lada w OSPI
// FLASH, wiec RAM STATYCZNY NIE ROSNIE. Robocza pamiec enkodera to obiekt JPEGE_IMAGE
// (~3,2 kB: ucFileBuf 2 kB + bufory MCU) alokowany NA STOSIE helpera encodeFrameJpeg()
// (wolanego dopiero PO petli renderujacej, wiec jego 3,2 kB nie nakalda sie na najglebszy
// lancuch paintFrame; stos webTask to 16 kB). Bufory obrazu (RGB565 153,6 kB + wyjscie
// JPEG ~96 kB) ida do PSRAM. Awaryjnie zostaje stara sciezka BMP, gdy PSRAM/enkoder zawiedzie.
//
// Nadal PRZELACZNIK: 1 = JPEG (JPEGENC), 0 = tylko awaryjny BMP (JPEGENC nie referowany).
// Domyslnie 1 -- i teraz RAM statyczny zostaje POD 76000 B nawet przy 1.
#ifndef WEATHER_UI_SCREENSHOT_JPEG
#define WEATHER_UI_SCREENSHOT_JPEG 1
#endif
#if WEATHER_UI_SCREENSHOT_JPEG
#include <JPEGENC.h>
#endif

// ---------------------------------------------------------------- pomocnicze --

namespace {

constexpr int W = cfg::SCREEN_W;
constexpr int CY = cfg::CONTENT_Y;                  // 34
constexpr int CH = cfg::CONTENT_H;                  // 172
// CB usuniete — bylo martwe (zero uzyc) i bylo TRZECIM bytem opisujacym liczbe 206,
// obok VIEW_H (jedyny zywy) i skasowanego juz cfg::FOOTER_Y, ktory twierdzil 208.

// PETLA 8 WIDOKOW V3 "Pasmowy" — JEDYNE zrodlo prawdy o kolejnosci (spec 7a):
// GLOWNY->RADAR->5 DNI->PRAD->POKOJE->OGRZEWANIE->POWIETRZE->SAMOLOTY->(GLOWNY).
// NIE numeryczna cfg::VIEW_* — ta kolejnosc jest projektowa. Do v_now byla lokalna
// w touchTapV3(); wyniesiona tu, bo korzystaja z niej TRZY miejsca w TYM pliku:
// touchTapV3() (krok 1x), render() (auto-rotacja co dwellS) i v3ProgressPos()
// (pasek postepu). Pomijanie niedostepnych widokow liczy viewSkipped(v, air_).
constexpr uint8_t kV3Loop[] = {
    cfg::VIEW_NOW, cfg::VIEW_RADAR, cfg::VIEW_DAYS, cfg::VIEW_PV,
    cfg::VIEW_HOME, cfg::VIEW_BOILER, cfg::VIEW_AIR, cfg::VIEW_FLIGHTS};
constexpr int kV3LoopN = sizeof(kV3Loop) / sizeof(kV3Loop[0]);

// TRYB NOCNY "dotyk budzi ekran" (ustalenia wlasciciela). W oknie nocnym (isNightNow: ciemno
// == blNight + pora nocna, domyslnie 22..6) render() rysuje przygaszony zegar nocny na
// settings().blNight. Dotkniecie elektrody ma na chwile przywrocic PELNY, nawigowalny UI —
// jak w dzien — na tej POSREDNIEJ jasnosci, a po kNightWakeMs bez dotyku wrocic do zegara.
// To zachowanie RUNTIME: NIE ruszamy zapisanych nightStartH/EndH/blNight (twarde ogr. 4).
// kNightWakeBl = 130: jasny na tyle, by czytac w nocy z 2 m, ale nie razacy jak 255.
constexpr uint8_t  kNightWakeBl = 130;      // jasnosc wybudzonego UI w nocy (nie blNight, nie 255)
constexpr uint32_t kNightWakeMs = 60000UL;  // 60 s bez dotyku -> powrot do zegara nocnego

float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
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

// Publiczna i statyczna (patrz deklaracja w WeatherUi.h): woła ją takze drawV3()
// przez v3ProgressPos(), bez wlasnej instancji WeatherUi. Warunki SA dokladnie tym,
// co do v118 stalo wprost w pasku postepu; wydzielenie nie zmienia zadnego z nich,
// tylko daje im jedno miejsce zamiast dwoch.

bool WeatherUi::viewSkipped(int i, const AirModel* air) {
  // V3 "Pasmowy" nie ma ekranu RETRO (Mario) ani osobnego GODZINY — prognoza
  // godzinowa jest wchlonieta w pasek opadu ekranu glownego (patrz makieta 01).
  // Pomijamy je w rotacji, zeby kolejnosc byla: GLOWNY→RADAR→5 DNI→PRAD→POKOJE→
  // OGRZEWANIE→POWIETRZE→SAMOLOTY, zgodnie ze specyfikacja projektu.
  // (v160) Warunek byl wczesniej pod `settings().theme == 3`; po usunieciu motywow
  // V1/V2 zostal jeden uklad, wiec obowiazuje bezwarunkowo.
  if (i == cfg::VIEW_RETRO || i == cfg::VIEW_HOURS) {
    return true;
  }
  return (i == cfg::VIEW_RADAR && !radarmap::hasRain()) ||
         (i == cfg::VIEW_HOME && ble::count() == 0) ||
         (i == cfg::VIEW_BOILER && !settings().hasViessmann()) ||
         (i == cfg::VIEW_AIR && (!air || !air->ready));
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


// ----------------------------------------------------- opis pogody pod ikona --
// Do 2 linii, lamane przy spacji tak, zeby linie byly mozliwie rowne.
// Dlaczego 2, a nie 1: "Częściowe zachmurzenie" ma 161 px, a pod ikona (srodek
// x=258) miesci sie najwyzej 116 px do krawedzi ekranu. Kazde POJEDYNCZE slowo
// sie miesci (najdluzsze, "Zachmurzenie", ma 91 px), wiec lamanie przy spacji
// zawsze wystarcza — sprawdzone dla wszystkich 28 kodow WMO.

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

// HUD dolny RETRO. To DOKLADNIE ten sam pas (y=VIEW_H..SCREEN_H-1 = 206..239), ktory
// dzis maluje drawV3Bottom() — kiedys wybieral miedzy nimi wywolujacy (render()/
// streamScreenshot()); po usunieciu V1/V2 (v160) ta funkcja nie jest juz wolana.
// `dst` generyczny: dziala i na zywym TFT, i na pasku zrzutu ekranu.
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

// --------------------------------------------------------- WIDOK 3: 5 DNI ----

// ------------------------------------------------------------- WIDOK 4: PV ----

// ------------------------------------------------------- wskaźnik ze strefami --
// Pionowy słupek, w którym TŁO niesie tyle samo informacji co wypełnienie:
// przygaszone pasy pokazują, gdzie kończy się strefa bezpieczna, a gdzie zaczyna
// niebezpieczna. Dzięki temu widać nie tylko "ile jest", ale też "ile jeszcze można".
// Sama liczba (53 °C, 112 kB) nic nie mówi, jeśli nie wiadomo, co jest granicą.

// ------------------------------------------------------------------- ALERT ----

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
  // --- prefetch tuz przed przejsciem na ekran lotow ---------------------------
  // ARYTMETYKA NA NUMERACH WIDOKOW JEST TU ZAKAZANA. Do v160 stalo w tym miejscu
  //     (cfg::VIEW_FLIGHTS + cfg::VIEW_COUNT - 1) % cfg::VIEW_COUNT
  // czyli "poprzedni ekran to ten o numer mniejszy". To jest nieprawda, i to z dwoch
  // niezaleznych powodow:
  //   1) NUMERY WIDOKOW TO NIE KOLEJNOSC. cfg::VIEW_* ma 13 pozycji i historyczna
  //      numeracje (patrz Config.h), a petla V3 ma osiem ekranow w kolejnosci
  //      PROJEKTOWEJ z kV3Loop. VIEW_FLIGHTS == 8, wiec arytmetyka wskazywala
  //      VIEW_PV (7) — a w kV3Loop przed SAMOLOTAMI stoi POWIETRZE. PRAD jest
  //      CZTERY pozycje wczesniej, wiec pobranie startowalo cztery ekrany za wczesnie
  //      i tuz przed wejsciem na SAMOLOTY juz nikt nie odswiezal: wchodzac na ekran
  //      widac bylo przez chwile stara liste.
  //   2) "POPRZEDNI" NIE JEST STALY. Ekrany wypadaja z petli warunkowo (viewSkipped:
  //      POWIETRZE bez danych, OGRZEWANIE bez autoryzacji, POKOJE bez czujnikow BLE,
  //      RADAR gdy nie pada), wiec poprzednikiem SAMOLOTOW bywa POWIETRZE, OGRZEWANIE,
  //      POKOJE albo PRAD — zaleznie od stanu urzadzenia w tej sekundzie. Zadna stala
  //      tego nie opisze.
  // Dlatego liczymy poprzednika z DOKLADNIE TEJ SAMEJ pary, ktorej uzywa rotacja
  // (render()) i nawigacja dotykiem (touchTapV3()): kV3Loop + viewSkipped(). Jedno
  // zrodlo prawdy o kolejnosci — inaczej prefetch znowu rozjedzie sie z rotacja przy
  // pierwszej zmianie ukladu ekranow.
  int idx = -1;
  for (int i = 0; i < kV3LoopN; ++i)
    if (kV3Loop[i] == cfg::VIEW_FLIGHTS) { idx = i; break; }
  if (idx < 0) return false;   // SAMOLOTOW nie ma w petli — nie ma czego wyprzedzac
  uint8_t prev = cfg::VIEW_FLIGHTS;
  for (int step = 0; step < kV3LoopN; ++step) {
    idx = (idx + kV3LoopN - 1) % kV3LoopN;   // krok WSTECZ po petli
    if (!viewSkipped(kV3Loop[idx], air_)) { prev = kV3Loop[idx]; break; }
  }
  if (view_ == prev && !transitioning_ && !alertActive_) {
    const uint32_t hold = holdFor(view_);
    const uint32_t el = nowMs - viewStart_;
    if (hold > cfg::FLIGHT_PREFETCH_MS && el >= hold - cfg::FLIGHT_PREFETCH_MS) {
      return true;
    }
  }
  return false;
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
  // awaria — makiety 13/18/19), nie zwykly ekran; postep liczony jak dawniej (260 ms).
  //
  // (v160) Znikly stad dwie galezie po usunieciu motywow V1/V2: slajd przejscia
  // (transitioning_ rysowal dwa widoki obok siebie) i wspolna belka+pasek postepu.
  // V3 nigdy z nich nie korzystal — robi ciecie i rysuje wlasny pasek postepu w
  // drawV3(). Dlatego `transitioning_` nie ma juz wplywu na to, CO sie rysuje.
  if (alertActive_) {
    drawV3Alert(spr, clampf(static_cast<float>(nowMs - alertStart_) / 260.f, 0.f, 1.f));
  } else {
    drawV3(spr, view_, 0, enterT, w, pv, hist, fl, nowMs, heapNow);
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
    } else {
      // V3 "Pasmowy" (spec 7a): domyslnie BRAK auto-rotacji — nawigacja recznie
      // dotykiem (touchTapV3/touchDoubleV3). Wlasciciel moze wlaczyc auto-rotacje w
      // panelu (settings().autoRotate); wtedy widoki petli zmieniaja sie same co dwellS.
      if (settings().autoRotate && pinned_ < 0 && lastTouchMs_ == 0) {
        // AUTO-ROTACJA: tylko gdy wlaczona, bez pinu z panelu i bez SWIEZEGO dotyku
        // (lastTouchMs_==0 znaczy "rotacja nie jest zapauzowana"). Co dwellS sekund
        // przechodzimy do NASTEPNEGO niepomijanego widoku w kV3Loop — ta sama logika
        // co touchTapV3(), ale BEZ diag-toggle i BEZ ruszania lastTouchMs_ (zostaje 0,
        // wiec cykl leci dalej klatka po klatce).
        if (nowMs - viewStart_ >= static_cast<uint32_t>(settings().dwellS) * 1000UL) {
          int idx = 0;
          for (int i = 0; i < kV3LoopN; ++i)
            if (kV3Loop[i] == view_) { idx = i; break; }
          for (int step = 0; step < kV3LoopN; ++step) {
            idx = (idx + 1) % kV3LoopN;
            if (!viewSkipped(kV3Loop[idx], air_)) break;
          }
          prevView_ = view_;
          view_ = kV3Loop[idx];
          viewStart_ = nowMs;
          enterStart_ = nowMs;
          v3Sig_ = 0xFFFFFFFFu;
          // NIE ruszamy lastTouchMs_ — auto-rotacja to nie dotyk.
        }
      } else if (view_ != cfg::VIEW_NOW && lastTouchMs_ != 0 &&
                 nowMs - lastTouchMs_ >= 60000UL) {
        // Powrot po 60 s ciszy: albo auto-rotacja wylaczona (dotyk to jedyna
        // nawigacja), albo wlaczona, ale zapauzowana SWIEZYM dotykiem (lastTouchMs_!=0).
        // Kazdy widok wraca do GLOWNEGO (VIEW_NOW). Liczymy od ostatniego STUKNIECIA —
        // panel-pin bez dotyku (lastTouchMs_==0) NIE wraca, zeby pin z /api/view dalej
        // dzialal (twarde ograniczenie 4). Po powrocie lastTouchMs_=0, wiec przy
        // autoRotate=on cykl wznawia sie naturalnie od GLOWNEGO.
        prevView_ = view_;
        view_ = static_cast<uint8_t>(cfg::VIEW_NOW);
        viewStart_ = nowMs;
        enterStart_ = nowMs;
        v3Sig_ = 0xFFFFFFFFu;
        lastTouchMs_ = 0;   // juz na GLOWNYM — nie odliczaj w kolko
      }
    }
  }

  const float enterT = clampf(static_cast<float>(nowMs - enterStart_) / cfg::ENTER_ANIM_MS,
                              0.f, 1.f);

  // Temperatura CPU: cache co 10 s. Do v135 aktualizowana TYLKO w stopce PV motywow
  // V1/V2 (drawFooter, dzis nieistniejacej) — w V3 nikt jej nie wolal, wiec cpuTempC_
  // zostawal 0 i ekran diagnostyki pokazywal "0 °C". Odswiezamy tu, PRZED ewentualnym
  // pominieciem klatki ponizej (inaczej przy statycznym V3 nigdy by sie nie zmienila).
  if (nowMs - cpuTempAt_ > 10000 || cpuTempAt_ == 0) {
    cpuTempC_ = temperatureRead();
    cpuTempAt_ = nowMs;
  }

  // --- TRYB NOCNY: dotyk WYBUDZA normalny UI (runtime; NIE zmienia nightStartH/EndH/blNight) -
  // W oknie nocnym (isNightNow: ciemno + pora nocna) domyslnie leci przygaszony zegar nocny —
  // drawV3 rysuje v3MainNight, bo isNightNow(blTarget_) == true. Gdy od ostatniego STUKNIECIA
  // minelo < kNightWakeMs, podbijamy blTarget_ do kNightWakeBl (~130): wtedy isNightNow(blTarget_)
  // w drawV3/drawV3Bottom zwroci FALSE, wiec TE SAME funkcje narysuja pelny, nawigowalny UI jak
  // w dzien, tyle ze na jasnosci ~130. Po kNightWakeMs bez dotyku blTarget_ zostaje blNight (z
  // automatu LDR w .ino) i zegar nocny wraca. Robimy to PRZED sygnatura nizej (mix(blTarget_)),
  // zeby przejscie spanie<->czuwanie ZAWSZE wymusilo przerysowanie (inaczej skip zjadlby klatke).
  // nightAsleep_ = noc bez swiezego dotyku — czyta go touchTapV3/touchDoubleV3, zeby PIERWSZY
  // dotyk budzil na Glowny, a nie przeskakiwal ekranu. Liczymy od lastTouchMs_ (STUKNIECIE, ta
  // sama nawigacja co w dzien), wiec pin z panelu bez dotyku NIE wybudza (twarde ogr. 4).
  nightAsleep_ = false;
  if (isNightNow(blTarget_)) {
    if (lastTouchMs_ != 0 && nowMs - lastTouchMs_ < kNightWakeMs) {
      blTarget_ = kNightWakeBl;   // wybudzony: ~130 => isNightNow() w drawV3 false => dzienny UI
    } else {
      nightAsleep_ = true;        // przygaszony zegar nocny; czekamy na wybudzajacy dotyk
    }
  }

  // --- V3: pomijanie przerysowania, gdy nic widocznego sie nie zmienilo -----------
  // loop() wola render() co ~50 ms i BEZWARUNKOWO wypycha bufor na TFT. Na ciemnym
  // tle (V1/V2) przepisanie tych samych pikseli jest niewidoczne; na JASNYM ukladzie
  // V3, na fizycznym ST7789, kazde wypchniecie widac jako blysk odswiezenia — ekran
  // "mrucze" bez przerwy (zgloszone przez wlasciciela: na V1 to samo bylo widac tylko
  // przy animacji Mario). V3 czyta SUROWE modele, stale miedzy pobraniami z sieci, wiec
  // tresc realnie zmienia sie rzadko. Liczymy sygnature tego, co widac; gdy bez zmian —
  // nie rysujemy i nie wypychamy. Radar (dryf chmur), przejscia i alerty rysuja sie zawsze.
  // `&& !settings().autoRotate`: przy WLACZONEJ auto-rotacji pasek postepu ANIMUJE sie
  // (wypelnienie aktualnego segmentu rosnie do przelaczenia), wiec nie wolno pomijac
  // klatek — rysujemy co klatke. Przy wylaczonej (domyslnie) pasek jest statyczny,
  // wiec pomijanie zostaje i migotanie na ST7789 nie wraca (patrz spec V3, ograniczenie 4).
  if (!settings().autoRotate && view_ != cfg::VIEW_RADAR &&
      !transitioning_ && !alertActive_ && (nowMs - enterStart_) >= cfg::ENTER_ANIM_MS) {
    uint32_t sig = 2166136261u;
    auto mix = [&](uint32_t x) { sig = (sig ^ x) * 16777619u; };
    mix(view_);
    // (v158) Licznik "x z y" w lewym gornym rogu (drawV3). Mianownik zmienia sie SAM,
    // bez zmiany widoku: przestaje padac -> RADAR wypada z petli, znika ostatni czujnik
    // BLE -> wypadaja POKOJE. Bez tej linii pomijanie klatek trzymaloby na ekranie stary
    // mianownik az do nastepnego taktu minuty (mix(nt/60) nizej), czyli licznik klamalby
    // do 60 s. Koszt: osiem wywolan viewSkipped() na klatke, same odczyty pol.
    {
      int pcur = 0, ptot = 0;
      v3ProgressPos(pcur, ptot);
      mix(static_cast<uint32_t>(pcur + 1) | (static_cast<uint32_t>(ptot) << 8));
    }
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
    // Ekran GLOWNY: przerysowanie co SEKUNDE, zeby dwukropek zegara mogl mrugac
    // (wlasciciel). 1 klatka/s na jasnym tle jest niezauwazalna, a nie 20/s jak przed
    // naprawa migotania. Reszta ekranow (radar animuje sam) bez sekundowego ticku.
    if (view_ == cfg::VIEW_NOW || view_ == cfg::VIEW_RETRO || view_ == cfg::VIEW_HOURS)
      mix(nowMs / 1000);
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
      // Ekran POKOJE (V3) to teraz WYKRES wszystkich pokoi na wspolnej osi — jego wyglad
      // zalezy od kazdej temperatury i od okna historii, wiec obok wartosci per-wiersz
      // mieszamy tez SUME biezacych temperatur (domyka przypadek zamiany wartosci miedzy
      // wierszami, przy ktorej same skladniki daja te sama sygnature).
      int32_t tSum = 0;
      for (int i = 0; i < roomModel_->count && i < 6; ++i) {
        tSum += static_cast<int>(roomModel_->rows[i].tempC * 10);
        mix(static_cast<uint32_t>(static_cast<int>(roomModel_->rows[i].tempC * 10)) ^
            ((roomModel_->rows[i].ageS / 60) << 16));
      }
      mix(static_cast<uint32_t>(tSum));
      // rooms_->head/lastSlot: numer biezacego slotu ruchomego okna 24 h. Zmienia sie co
      // 10 min przy przewinieciu historii — wtedy wykres przesuwa sie w lewo i MUSI sie
      // przerysowac, nawet gdy biezace temperatury sa identyczne (inaczej okno "zamarza").
      if (rooms_) mix(static_cast<uint32_t>(rooms_->head) ^ (rooms_->lastSlot << 16));
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
  } else {
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

  // V3 rysuje dolny pas (206..239) sam, wprost na TFT — uklad V3 siega pelnej
  // wysokosci (POWIETRZE na glownym, osie wykresow), a tego nie da sie zmiescic w
  // sprite 206 px. (v160) Po usunieciu V1/V2 to JEDYNA sciezka dolnego pasa: stopka
  // PV (drawFooter) i HUD ekranu RETRO nalezaly do tamtych ukladow i zniknely razem
  // z nimi.
  drawV3Bottom(tft_, view_, w, pv, fl, nowMs, heapNow);
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

}  // namespace

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

// -------------------------------------------------- WIDOK 6: PIEC ------------
// Vitodens 050-W przez chmure ViCare. Piec jest slepy — nie ma czujnika
// zewnetrznego ani krzywej grzewczej — wiec pokazujemy to, co naprawde wie:
// wlasna wode, palnik i zuzycie gazu.

// ---------------------------------------------- WIDOK 7: W DOMU (czujniki BLE) --
// Sens tego ekranu nie polega na pokazaniu dwoch liczb — te sa w telefonie.
// Polega na ZESTAWIENIU ich z tym, co na zewnatrz: od razu widac, czy warto
// otworzyc okno, i gdzie robi sie duszno.

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
// Cala trojka nizej to JEDYNA nawigacja dotykiem (wola ja switch dotyku w
// pogoda-gdynia.ino oraz symulacja z panelu). Do v159 stal obok niej wariant V1/V2
// (restartHold/prevView) — zniknal razem z tamtymi motywami.

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
  // TRYB NOCNY: gdy TERAZ swieci przygaszony zegar nocny (nightAsleep_ ustawia render()),
  // PIERWSZY dotyk ma tylko WYBUDZIC na Glowny — bez przeskakiwania na nastepny ekran.
  // Ustawione wyzej lastTouchMs_ sprawia, ze od tej klatki render() podbije jasnosc do
  // kNightWakeBl i narysuje pelny UI; kolejne dotkniecia (juz wybudzony, nightAsleep_==false)
  // nawiguja normalnie. Decyzja "pierwszy dotyk budzi, nie skacze" — wg ustalen wlasciciela.
  if (nightAsleep_) {
    nightAsleep_ = false;
    v3WokeByTap_ = true;   // gdyby zaraz przyszlo DOUBLE — ma tylko dokonczyc wybudzenie
    setViewV3(static_cast<uint8_t>(cfg::VIEW_NOW));
    return;
  }
  v3WokeByTap_ = false;
  // W diagnostyce 1x przelacza STATS <-> MEM, nie rusza petli glownej (spec 7a).
  if (view_ == cfg::VIEW_STATS || view_ == cfg::VIEW_MEM) {
    setViewV3(view_ == cfg::VIEW_STATS ? cfg::VIEW_MEM : cfg::VIEW_STATS);
    return;
  }
  // PETLA 8 WIDOKOW: zrodlo prawdy (kolejnosc 7a) stoi w kV3Loop na gorze pliku —
  // ta sama, ktorej uzywa auto-rotacja w render() i pasek postepu (v3ProgressPos).
  // Znajdz biezacy widok w petli; jesli go tam nie ma (np. RUCH z panelu albo stan
  // startowy), traktuj jak pozycje GLOWNEGO, wiec pierwszy krok wejdzie za NOW.
  int idx = 0;
  for (int i = 0; i < kV3LoopN; ++i)
    if (kV3Loop[i] == view_) { idx = i; break; }
  // Nastepny NIEPOMIJANY (viewSkipped: radar bez opadu, pokoje bez czujnikow, piec
  // bez autoryzacji, powietrze bez danych — to samo pyta rotacja V1/V2). Do kV3LoopN
  // krokow; gdy wszystko inne pominiete, wracamy na biezacy (na nim stoimy — nie jest
  // pomijany).
  for (int step = 0; step < kV3LoopN; ++step) {
    idx = (idx + 1) % kV3LoopN;
    if (!viewSkipped(kV3Loop[idx], air_)) break;
  }
  setViewV3(kV3Loop[idx]);
}

void WeatherUi::touchDoubleV3() {
  lastTouchMs_ = millis();
  // TRYB NOCNY: spojnie z touchTapV3 — pierwsza interakcja w nocy (takze podwojna) tylko
  // WYBUDZA na Glowny, zamiast od razu wchodzic w diagnostyke. Kolejne gesty dzialaja normalnie.
  if (nightAsleep_) {
    nightAsleep_ = false;
    setViewV3(static_cast<uint8_t>(cfg::VIEW_NOW));
    return;
  }
  // (v158) Ten sam gest, drugie zbocze: SINGLE juz poszlo i to ONO wybudzilo ekran
  // (v3WokeByTap_). Bez tego warunku podwojne stukniecie w nocy budzilo i w tej samej
  // chwili wchodzilo w diagnostyke — a ustalenie brzmi "pierwsza interakcja w nocy
  // tylko wybudza". Konsumujemy flage i wychodzimy: ekran zostaje na GLOWNYM.
  if (v3WokeByTap_) {
    v3WokeByTap_ = false;
    return;
  }
  // 2x w diagnostyce wychodzi na GLOWNY; poza nia 2x wchodzi w diagnostyke (STATS).
  if (view_ == cfg::VIEW_STATS || view_ == cfg::VIEW_MEM)
    setViewV3(static_cast<uint8_t>(cfg::VIEW_NOW));
  else
    setViewV3(static_cast<uint8_t>(cfg::VIEW_STATS));
}

// Pozycja biezacego widoku w PETLI V3 (kV3Loop) wsrod NIEPOMIJANYCH ekranow — zrodlo
// dla paska postepu rysowanego w drawV3() (WeatherUiV3.cpp). Definicja tutaj, bo kV3Loop
// i viewSkipped zyja w TEJ jednostce kompilacji (anonimowy namespace); WeatherUiV3.cpp
// wola ten helper, zamiast duplikowac kolejnosc petli. `total` = ile ekranow petli jest
// TERAZ dostepnych (viewSkipped: radar bez opadu, pokoje bez czujnikow, piec bez
// autoryzacji, powietrze bez danych). `cur` = pozycja biezacego wsrod nich (0..total-1).
// Zwraca false, gdy biezacy widok NIE nalezy do petli (diagnostyka STATS/MEM/MOTION albo
// stan startowy) — wtedy pasek nie ma czego pokazac i drawV3 go nie rysuje.
bool WeatherUi::v3ProgressPos(int& cur, int& total) const {
  cur = -1;
  total = 0;
  for (int i = 0; i < kV3LoopN; ++i) {
    if (viewSkipped(kV3Loop[i], air_)) continue;
    if (kV3Loop[i] == view_) cur = total;
    ++total;
  }
  return cur >= 0 && total > 0;
}

#if WEATHER_UI_SCREENSHOT_JPEG
namespace {

// Enkoduje CALA klatke WxH RGB565 (ciagly bufor 'rgb', bytesPerLine = w*2) do bufora
// wyjsciowego 'out' o pojemnosci 'outCap' za pomoca JPEGENC. Zwraca dlugosc gotowego
// JPEG w bajtach (>0) albo 0, gdy ktorykolwiek krok zawiedzie (wtedy wolajacy leci na BMP).
//
// UWAGA STOS: klasa JPEGENC trzyma w sobie JPEGE_IMAGE (~3,2 kB: ucFileBuf[2048] + bufory
// MCU) i obiekt 'jpg' zyje NA STOSIE tej funkcji. Jest wolana DOPIERO po zakonczeniu petli
// renderujacej klatke, wiec te 3,2 kB nie wspolistnieje z najglebszym lancuchem paintFrame()
// (stos webTask to 16 kB, ciasno). Dlatego enkoder jest tu, a nie inline w streamScreenshot.
//
// BEZPIECZENSTWO BUFORA: JPEGENC pisze wprost do 'out', ale gdy wskaznik dojdzie do
// out+outCap-512 (pHighWater), ustawia iError=JPEGE_NO_BUFFER i przerywa -- brak przepelnienia.
// Zwracamy wtedy 0 (addFrame != SUCCESS lub getLastError() != SUCCESS) i wolajacy leci na BMP.
size_t encodeFrameJpeg(uint8_t* rgb, int w, int h, uint8_t* out, size_t outCap) {
  JPEGENC jpg;
  JPEGENCODE enc;
  if (jpg.open(out, static_cast<int>(outCap)) != JPEGE_SUCCESS) {
    return 0;
  }
  // 4:2:0 + jakosc HIGH: rozsadny kompromis rozmiar/jakosc dla plaskich teł i gradientow UI.
  if (jpg.encodeBegin(&enc, w, h, JPEGE_PIXEL_RGB565,
                      JPEGE_SUBSAMPLE_420, JPEGE_Q_HIGH) != JPEGE_SUCCESS) {
    return 0;
  }
  // addFrame enkoduje wszystkie MCU jednym wywolaniem; iPitch = bajty na linie = w*2 = 640.
  if (jpg.addFrame(&enc, rgb, w * 2) != JPEGE_SUCCESS) {
    return 0;
  }
  const int jpgLen = jpg.close();   // dopisuje marker EOI, zwraca laczna dlugosc JPEG
  if (jpgLen <= 0 || jpg.getLastError() != JPEGE_SUCCESS) {
    return 0;
  }
  return static_cast<size_t>(jpgLen);
}

}  // namespace
#endif  // WEATHER_UI_SCREENSHOT_JPEG

void WeatherUi::streamScreenshot(WiFiClient& client, const WeatherModel& w, const PvModel& pv,
                                 const PvHistory& hist, const FlightModel& fl, bool wifiOk) {
  if (!ready_ || freed_) {
    return;   // trwa OTA — sterty i tak nie ma na nic
  }

  constexpr int WD = cfg::SCREEN_W;
  constexpr int HT = cfg::SCREEN_H;

  // Jedna klatka = jeden moment w czasie. Lapiemy nowMs i heapNow RAZ i wieziemy przez
  // OBA warianty (JPEG glowny i awaryjny BMP). Gdyby kazdy pas/wiersz bral swieze millis()
  // albo swiezy odczyt sterty, rozjechalyby sie nie tylko animacje: przy BMP miedzy
  // wierszami leci transmisja (setki ms), wiec napisy "OK 12s temu" i "WOLNY RAM" na
  // ekranie statystyk pokazalyby w kolejnych pasach ROZNE wartosci i litery rwalyby sie
  // w pol. W JPEG cala klatka trafia najpierw do bufora, wiec tam to mniej pali, ale
  // spojnosc obu sciezek trzymamy tak samo.
  const uint32_t nowMs = millis();
  const uint32_t heapNow = ESP.getFreeHeap();

#if WEATHER_UI_SCREENSHOT_JPEG
  // === WARIANT GLOWNY: JPEG enkodowany NA URZADZENIU ==========================
  // Cala klatka 320x240 idzie do jednego ciaglego bufora RGB565 w PSRAM (153,6 kB),
  // a JPEGENC (encodeFrameJpeg nizej) robi z niego JPEG. Panel pobiera ~10x mniej
  // bajtow i odswieza sie plynnie. Wszystko dzieje sie PRZED wyslaniem naglowkow HTTP —
  // jesli cokolwiek zawiedzie (malo PSRAM, brak sprite'a, blad enkodera), po cichu
  // spadamy do awaryjnej sciezki BMP na dole, ktora dopiero wtedy zaczyna pisac do klienta.
  {
    const size_t rgbLen = static_cast<size_t>(WD) * HT * 2;   // 320*240*2 = 153600
    const size_t outCap = 96u * 1024u;                        // 98304 -- z zapasem na JPEG 320x240

    // Dwa bufory ida do PSRAM: ciagly RGB565 calej klatki (153,6 kB) oraz wyjscie JPEG
    // (96 kB -- 320x240 przy 4:2:0/HIGH miesci sie z ogromnym zapasem, a JPEGENC i tak
    // przerwie na pHighWater=outCap-512 bez przepelnienia). Wymagamy zapasu, zeby nie
    // zabrac pamieci radarowi/TLS; przy niedoborze po cichu lecimy BMP. getFreePsram()==0
    // (brak PSRAM) tez zbija nas do BMP.
    uint8_t* rgb = nullptr;
    uint8_t* out = nullptr;
    if (ESP.getFreePsram() >= rgbLen + outCap + 24u * 1024u) {   // prog ~276 kB
      rgb = static_cast<uint8_t*>(ps_malloc(rgbLen));   // MALLOC_CAP_SPIRAM, NIE stos webTask
    }

    if (rgb != nullptr) {
      // Pas roboczy 320x24x16bpp = 15,4 kB — ten sam co w sciezce BMP. NIE bufor
      // wyswietlacza: rysujemy od nowa, wiec obraz na TFT sie nie zatrzymuje.
      TFT_eSprite shot(&tft_);
      shot.setColorDepth(16);
      if (shot.createSprite(WD, SHOT_H) != nullptr) {
        shot.setSwapBytes(false);
        // Renderujemy pas po pasie (jak BMP), ale zamiast wysylac -- przepisujemy piksele
        // do ciaglego bufora RGB565. KOLEJNOSC BAJTOW: JPEGENC czyta kazdy piksel jako
        // natywny uint16_t (us = *(uint16_t*)pSrc w JPEGSubSample16), czyli na ESP32-S3
        // (little-endian) MLODSZY bajt idzie pierwszy. readPixel() zwraca 565 w porzadku
        // hosta (uint16_t, R w bitach 15-11), wiec pakujemy LSB-first: dst[0]=c&0xFF,
        // dst[1]=c>>8. JPEGENC rozklada wtedy R=(us&0xf800), G=(us&0x7e0), B=(us&0x1f) --
        // te same bity co sciezka BMP nizej, wiec kolory wychodza wiernie. GDYBY kolory
        // wyszly zamienione (czerwony<->niebieski), odwroc te dwie linie na big-endian:
        //   dst[x*2+0]=c>>8; dst[x*2+1]=c&0xFF;
        for (int top = 0; top < HT; top += SHOT_H) {
          setBand(shot, top, HT);          // uklad globalny 0..239 (ze stopka)
          if (backlightSweepActive(nowMs)) {
            drawBacklightSweep(shot, nowMs);
          } else {
            paintFrame(shot, w, pv, hist, fl, wifiOk, nowMs, heapNow);
          }
          // Ten sam dolny pas co live-render i sciezka BMP — inaczej podglad
          // pokazywalby inny dol niz fizyczny ekran.
          drawV3Bottom(shot, view_, w, pv, fl, nowMs, heapNow);
          for (int y = top; y < top + SHOT_H && y < HT; ++y) {
            uint8_t* dst = rgb + static_cast<size_t>(y) * WD * 2;
            for (int x = 0; x < WD; ++x) {
              const uint16_t c = shot.readPixel(x, y);              // wspolrzedne globalne
              dst[x * 2 + 0] = static_cast<uint8_t>(c & 0xFF);      // LSB: GGGBBBBB (little-endian)
              dst[x * 2 + 1] = static_cast<uint8_t>(c >> 8);        // MSB: RRRRRGGG
            }
          }
        }
        shot.deleteSprite();

        // Enkodujemy DOPIERO teraz -- petla renderujaca (najglebszy paintFrame) juz zeszla
        // ze stosu, wiec ~3,2 kB obiektu JPEGENC w encodeFrameJpeg() nie nakalda sie na nia.
        // Bufor wyjsciowy w PSRAM; przy jego braku po cichu lecimy BMP.
        out = static_cast<uint8_t*>(ps_malloc(outCap));
        if (out != nullptr) {
          const size_t jpgLen = encodeFrameJpeg(rgb, WD, HT, out, outCap);
          if (jpgLen > 0) {
            free(rgb);   // bufor RGB565 juz zbedny -- gotowy JPEG jest w 'out'
            rgb = nullptr;
            client.print("HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\n");
            client.printf("Content-Length: %u\r\n", static_cast<unsigned>(jpgLen));
            client.print("Cache-Control: no-store\r\nConnection: close\r\n\r\n");
            // Body w kawalkach -- jeden wielki write bywa krotki, gdy bufor TCP jest pelny.
            size_t sent = 0;
            while (sent < jpgLen) {
              const size_t n = client.write(out + sent, jpgLen - sent);
              if (n == 0) break;   // klient zerwal polaczenie
              sent += n;
            }
            client.flush();
            free(out);
            return;   // sukces JPEG -- NIE schodzimy do BMP
          }
          free(out);   // enkoder zawiodl (malo miejsca/blad) -- oddajemy PSRAM i lecimy BMP
        }
      }
      free(rgb);   // sprite sie nie udal albo enkoder zawiodl -- oddajemy PSRAM i lecimy BMP
    }
  }
#endif  // WEATHER_UI_SCREENSHOT_JPEG

  // === WARIANT AWARYJNY: nieskompresowany BMP 320x240x24 (jak dotad) ==========
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

  static uint8_t line[WD * 3];
  for (int top = HT - SHOT_H; top >= 0; top -= SHOT_H) {   // BMP idzie od dołu
    setBand(shot, top, HT);          // układ globalny 0..239 (ze stopką)
    // Gdy trwa test podswietlenia, zrzut MUSI pokazywac to samo co ekran — inaczej
    // zdalna weryfikacja testu jest zludzeniem (zrzut szedlby inna sciezka rysowania).
    // paintFrame czyści 0..205, drawV3Bottom maluje 206..239 — razem cały ekran,
    // więc świeżo wyzerowany sprite nie prześwituje nigdzie na czarno.
    if (backlightSweepActive(nowMs)) {
      drawBacklightSweep(shot, nowMs);
    } else {
      paintFrame(shot, w, pv, hist, fl, wifiOk, nowMs, heapNow);
    }
    // Ten sam dolny pas co live-render (drawV3Bottom): podglad w panelu ma pokazywac
    // dokladnie to, co fizyczny ekran.
    drawV3Bottom(shot, view_, w, pv, fl, nowMs, heapNow);

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
