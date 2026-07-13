#include "WeatherUi.h"

#include <Arduino.h>
#include <cmath>
#include <cstdio>
#include <ctime>

#include "Colors.h"
#include "Config.h"
#include "MapData.h"
#include "PlText.h"
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
constexpr int CB = cfg::CONTENT_Y + cfg::CONTENT_H; // 206 (poza obszarem)

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

void gl(TFT_eSPI& s, const char* t, int x, int y, uint16_t c) {
  s.setTextFont(1);
  s.setTextSize(1);
  s.setTextDatum(TL_DATUM);
  s.setTextColor(c);
  s.drawString(t, x, y);
}

void glCenter(TFT_eSPI& s, const char* t, int cx, int y, uint16_t c) {
  s.setTextFont(1);
  s.setTextSize(1);
  s.setTextDatum(TC_DATUM);
  s.setTextColor(c);
  s.drawString(t, cx, y);
}

void glRight(TFT_eSPI& s, const char* t, int right, int y, uint16_t c) {
  s.setTextFont(1);
  s.setTextSize(1);
  s.setTextDatum(TR_DATUM);
  s.setTextColor(c);
  s.drawString(t, right, y);
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

void WeatherUi::drawHeader(TFT_eSPI& spr, const WeatherModel& w, bool wifiOk, uint32_t nowMs) {
  // Belka mieści się w całości w pasie górnym — w dolnym nie ma co liczyć.
  if (!spr.checkViewport(0, 0, W, cfg::HEADER_H)) {
    return;
  }
  spr.fillRect(0, 0, W, cfg::HEADER_H, col::HEADER);
  spr.drawFastHLine(0, cfg::HEADER_H - 1, W, col::DIVIDER);

  // status sieci — kropka + delikatny puls przy braku WiFi
  uint16_t dot = wifiOk ? col::OK : col::ERR;
  if (!wifiOk && ((nowMs / 500) % 2)) {
    dot = col::BG_CARD;
  }
  spr.fillCircle(12, 14, 4, dot);

  plStr(spr, PLF14, settings().city, 24, 19, col::TEXT);

  time_t now = time(nullptr);
  if (now < 1700000000) {
    plRight(spr, PLF18, "--:--", W - 10, 21, col::TEXT_MUTE);
    plCenter(spr, PLF14, "synchronizacja czasu", W / 2 + 10, 19, col::TEXT_MUTE);
    return;
  }

  struct tm tmv{};
  localtime_r(&now, &tmv);

  static const char* kMon[12] = {"sty", "lut", "mar", "kwi", "maj", "cze",
                                 "lip", "sie", "wrz", "paź", "lis", "gru"};
  static const char* kDow[7] = {"niedz", "pon", "wt", "śr", "czw", "pt", "sob"};

  char mid[32];
  snprintf(mid, sizeof(mid), "%s %d %s", kDow[tmv.tm_wday % 7], tmv.tm_mday,
           kMon[tmv.tm_mon % 12]);
  plCenter(spr, PLF14, mid, W / 2 + 10, 19, col::TEXT_DIM);

  char clk[8];
  snprintf(clk, sizeof(clk), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
  plRight(spr, PLF18, clk, W - 10, 21, col::ACCENT);
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

static void viewHeader(TFT_eSPI& spr, int ox, const char* title, const char* right = nullptr,
                       uint16_t rightCol = col::TEXT_MUTE, uint16_t dotCol = 0) {
  plStr(spr, PLF14, title, ox + 10, HDR_Y, col::ACCENT);
  if (right == nullptr || right[0] == '\0') return;

  const int rw = pltxt::stringWidth(PLF14, right);
  if (dotCol != 0) spr.fillCircle(ox + W - 10 - rw - 11, HDR_Y - 5, 4, dotCol);
  plRight(spr, PLF14, right, ox + W - 10, HDR_Y, rightCol);
}

// ------------------------------------------------------------ WIDOK 1: TERAZ --

void WeatherUi::drawViewNow(TFT_eSPI& spr, int ox, float t, const WeatherModel& w) {
  const WeatherSnapshot& c = w.current;
  const float e = easeOutCubic(t);

  char hdr[24];
  snprintf(hdr, sizeof(hdr), "dane z %s", w.updatedAt[0] ? w.updatedAt : "--:--");
  viewHeader(spr, ox, "TERAZ", hdr);

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

  // --- ikona + opis ---
  const int icx = ox + 258;
  const int size = 40 + static_cast<int>(22 * e);
  wxico::draw(spr, c.weatherCode, icx, 82, size);
  plCenter(spr, PLF14, wxico::labelForCode(c.weatherCode), icx, 118, col::TEXT);

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

  const int cy0 = 122;
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

  viewHeader(spr, ox, "NAJBLIŻSZE 12 GODZIN", "TEMP / OPAD");

  // 13 punktów: teraz + 12 godzin
  float temp[13];
  float rain[13];
  int code[13];
  int hourLbl[13];
  bool ok[13];

  temp[0] = w.current.tempC;
  rain[0] = w.current.precipMm;
  code[0] = w.current.weatherCode;
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
    hourLbl[i + 1] = s.hourOfDay;
  }

  float vmin = 1e9f, vmax = -1e9f, rmax = 0.f;
  for (int i = 0; i < 13; ++i) {
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
  auto px = [&](int i) { return ox + 16 + i * 24; };
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
  for (int i = 0; i < 12; ++i) {
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
  for (int i = 0; i < 13; ++i) {
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
    for (int i = 1; i < 13; ++i) {
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
  for (int i = 0; i <= 12; i += 2) {
    if (!ok[i]) continue;
    const int x = px(i);
    char h[8];
    snprintf(h, sizeof(h), "%02d", hourLbl[i]);
    glCenter(spr, h, x, 160, i == 0 ? col::ACCENT : col::TEXT_DIM);
    wxico::draw(spr, code[i], x, 186, 24);
  }
}

// --------------------------------------------------------- WIDOK 3: 5 DNI ----

void WeatherUi::drawViewDays(TFT_eSPI& spr, int ox, float t, const WeatherModel& w) {
  const float e = easeOutCubic(t);

  viewHeader(spr, ox, "PROGNOZA 5 DNI", "MAX / MIN °C");

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

    fmtTempInt(b, sizeof(b), d.tempMin);
    plCenter(spr, PLF14, b, cx, 171, col::TEXT_DIM);

    // opad — pasek pod słupkiem
    if (d.precipMm > 0.2f) {
      const float scale = rmax < 4.f ? 4.f : rmax;
      int rw = static_cast<int>((d.precipMm / scale) * 44.f * e);
      if (rw < 3) rw = 3;
      if (rw > 44) rw = 44;
      spr.fillRoundRect(cx - 22, 176, 44, 4, 2, col::PV_TRACK);
      spr.fillRoundRect(cx - 22, 176, rw, 4, 2, col::RAIN);
    }

    // Pierwszy dzień był po prostu NIEBIESKI — kolor niósł informację "to jutro",
    // której nikt nie miał jak odczytać. Piszemy to słowem, w tym samym kolorze
    // co reszta. Kolor zostaje wyłącznie dla danych (temperatura, opad).
    plCenter(spr, PLF14, i == 0 ? "JUTRO" : d.name, cx, 195, col::TEXT);
    glCenter(spr, d.date, cx, 197, col::TEXT_MUTE);
  }
}

void WeatherUi::drawNoData(TFT_eSPI& spr, int ox, const char* msg, const char* sub) {
  spr.fillRoundRect(ox + 40, CY + 50, W - 80, 60, 8, col::BG_CARD);
  if (sub && sub[0]) {
    plCenter(spr, PLF14, msg, ox + W / 2, CY + 78, col::TEXT_DIM);
    glCenter(spr, sub, ox + W / 2, CY + 84, col::TEXT_MUTE);
  } else {
    plCenter(spr, PLF14, msg, ox + W / 2, CY + 86, col::TEXT_DIM);
  }
}

// ------------------------------------------------------------- WIDOK 4: PV ----

void WeatherUi::drawViewPv(TFT_eSPI& spr, int ox, float t, const PvModel& pv, const PvHistory& hist) {
  const float e = easeOutCubic(t);
  if (!pv.online) {
    viewHeader(spr, ox, "FOTOWOLTAIKA", pv.asleep ? "uśpiony" : "brak łączności",
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
  viewHeader(spr, ox, "FOTOWOLTAIKA", pvStatusLabel(d.statusCode), sc, sc);

  // --- wielka moc AC (animowana) ---
  const int32_t acNow = static_cast<int32_t>(lroundf(animAcW_));
  char v[16], u[8];
  fmtPower(v, sizeof(v), u, sizeof(u), acNow);
  gl(spr, "MOC CHWILOWA", ox + 13, 54, col::TEXT_MUTE);
  const int pw = bigStr(spr, &FreeSansBold24pt7b, v, ox + 12, 100, col::PV_SOLAR);
  plStr(spr, PLF18, u, ox + 16 + pw, 74, col::TEXT_DIM);

  char sub[40];
  snprintf(sub, sizeof(sub), "DC %ld W   |   %.0f V", static_cast<long>(d.powerDcW),
           d.pvVoltageV);
  gl(spr, sub, ox + 13, 108, col::TEXT_DIM);

  // --- wskaźnik (arc) ---
  if (pvScaleW_ < static_cast<float>(settings().pvPeakW)) pvScaleW_ = static_cast<float>(settings().pvPeakW);
  if (static_cast<float>(d.powerAcW) > pvScaleW_) pvScaleW_ = static_cast<float>(d.powerAcW) * 1.05f;

  const int gx = ox + 266, gy = 88, gr = 34, gir = 26;
  const float frac = clampf(animAcW_ / pvScaleW_, 0.f, 1.f) * e;
  glCenter(spr, "OBCIAZENIE", gx, 48, col::TEXT_MUTE);
  smoothArc(spr, gx, gy, gr, gir, 30, 330, col::PV_TRACK, col::BG);
  if (frac > 0.005f) {
    const int end = 30 + static_cast<int>(300.f * frac);
    const uint16_t ac = lerp565(col::PV_SOLAR, col::PV_EXPORT, clampf(frac, 0.f, 1.f));
    smoothArc(spr, gx, gy, gr, gir, 30, end, ac, col::BG);
  }
  char pct[8];
  snprintf(pct, sizeof(pct), "%d%%", static_cast<int>(lroundf(frac * 100.f)));
  plCenter(spr, PLF18, pct, gx, gy + 3, col::TEXT);
  char peak[14];
  snprintf(peak, sizeof(peak), "z %.1f kWp", pvScaleW_ / 1000.f);
  glCenter(spr, peak, gx, gy + 9, col::TEXT_MUTE);

  // --- profil dnia: produkcja + zużycie w jednej skali ---
  // Kolory są DOKŁADNIE te, którymi świeci dioda RGB — jeden kod barw w całym
  // urządzeniu, więc wykres czyta się bez legendy:
  //   zielony   = nadwyżka oddana do sieci      (dioda zielona)
  //   niebieski = zużycie pokryte z własnej PV  (dioda niebieska — równowaga)
  //   czerwony  = prąd dobrany z sieci          (dioda czerwona)
  gl(spr, "PRODUKCJA / ZUZYCIE", ox + 12, 122, col::TEXT_MUTE);
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
  switch (view) {
    case 0:
      if (w.ready) drawViewNow(spr, ox, t, w);
      else drawNoData(spr, ox, "Pobieram prognozę...");
      break;
    case 1:
      if (w.ready) drawViewHours(spr, ox, t, w);
      else drawNoData(spr, ox, "Pobieram prognozę...");
      break;
    case 2:
      if (w.ready) drawViewDays(spr, ox, t, w);
      else drawNoData(spr, ox, "Pobieram prognozę...");
      break;
    case 3:
      drawViewPv(spr, ox, t, pv, hist);
      break;
    case 4:
      drawViewFlights(spr, ox, t, fl);
      break;
    case 5:
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

  uint32_t tPaint = 0, tPush = 0;
  for (int b = 0; b < BAND_N; ++b) {
    const int top = b * BAND_H;
    const uint32_t t0 = cfg::PROFILE_FRAME ? micros() : 0;
    setBand(spr_, top, VIEW_H);
    paintFrame(spr_, w, pv, hist, fl, wifiOk, nowMs, heapNow);
    const uint32_t t1 = cfg::PROFILE_FRAME ? micros() : 0;
    spr_.pushSprite(0, top);
    if (cfg::PROFILE_FRAME) {
      tPaint += t1 - t0;
      tPush += micros() - t1;
    }
  }
  spr_.resetViewport();

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

void mapProject(float lat, float lon, int ox, int& x, int& y) {
  x = ox + static_cast<int>((lon - gmap::LON_MIN) / (gmap::LON_MAX - gmap::LON_MIN) *
                            gmap::MAP_W);
  y = CY + static_cast<int>((gmap::LAT_MAX - lat) / (gmap::LAT_MAX - gmap::LAT_MIN) *
                            gmap::MAP_H);
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
  spr.fillRect(mx, my, gmap::MAP_W, gmap::MAP_H, col::MAP_SEA);

  // --- ląd (pasy poziome z prekalkulowanej rasteryzacji) ---
  for (int row = 0; row < gmap::MAP_H; ++row) {
    const uint16_t a = pgm_read_word(&gmap::LAND_ROW_OFF[row]);
    const uint16_t b = pgm_read_word(&gmap::LAND_ROW_OFF[row + 1]);
    for (uint16_t s = a; s < b; ++s) {
      const uint8_t x0 = pgm_read_byte(&gmap::LAND_SPANS[s][0]);
      const uint8_t x1 = pgm_read_byte(&gmap::LAND_SPANS[s][1]);
      spr.drawFastHLine(mx + x0, my + row, x1 - x0 + 1, col::MAP_LAND);
    }
  }

  // --- linia brzegowa ---
  int px = 0, py = 0;
  for (int i = 0; i < gmap::COAST_COUNT; ++i) {
    const int16_t x = static_cast<int16_t>(pgm_read_word(&gmap::COAST_PTS[i][0]));
    const int16_t y = static_cast<int16_t>(pgm_read_word(&gmap::COAST_PTS[i][1]));
    if (i > 0) {
      spr.drawLine(mx + px, my + py, mx + x, my + y, col::MAP_COAST);
    }
    px = x;
    py = y;
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
    if (nx > mx + gmap::MAP_W - 9) nx = x - 12;
    if (ny < my + 9) ny = y + 12;
    if (ny > my + gmap::MAP_H - 9) ny = y - 12;
    spr.fillCircle(nx, ny, 7, c);
    spr.drawCircle(nx, ny, 7, col::BG);
    char nb[4];
    snprintf(nb, sizeof(nb), "%d", i + 1);
    glCenter(spr, nb, nx, ny - 3, col::BG);
  }

  // --- legenda ---
  const int ly0 = my + gmap::MAP_H - 12;
  spr.fillRect(mx, ly0, gmap::MAP_W, 12, col::BG);
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
    char fw[10];
    snprintf(fw, sizeof(fw), "v%d", FW_VERSION);
    plRight(spr, PLF14, fw, W - 10, 19, col::TEXT_MUTE);

    // ikona WiFi — łuki o sile zależnej od RSSI
    const int wx = 42, wy = 96;
    const int bars = rssi >= -55 ? 3 : (rssi >= -70 ? 2 : (rssi >= -82 ? 1 : 0));
    for (int i = 0; i < 3; ++i) {
      const int r = 14 + i * 10;
      const uint16_t c = (i < bars) ? col::ACCENT : col::PV_TRACK;
      smoothArc(spr, wx, wy, r, r - 4, 225, 315, c, col::BG);
    }
    spr.fillCircle(wx, wy, 4, bars > 0 ? col::ACCENT : col::PV_TRACK);

    // sieć
    gl(spr, "SIEC", 92, 50, col::TEXT_MUTE);
    plStr(spr, PLF18, ssid, 92, 76, col::TEXT);
    char sig[20];
    snprintf(sig, sizeof(sig), "%d dBm", rssi);
    gl(spr, sig, 92, 84, col::TEXT_DIM);

    // adres IP — duży, żeby dało się przepisać
    spr.fillRoundRect(14, 112, W - 28, 62, 10, col::BG_CARD);
    spr.fillRoundRect(14, 112, 4, 62, 2, col::ACCENT);
    gl(spr, "ADRES IP URZADZENIA", 30, 120, col::TEXT_MUTE);
    bigStr(spr, &FreeSansBold18pt7b, ip, 30, 162, col::ACCENT);

    plCenter(spr, PLF14, "Panel konfiguracji dostępny pod tym adresem", W / 2, 196,
             col::TEXT_DIM);

    // odliczanie
    const int bx = 40, bw = W - 80, by = 186;
    spr.fillRoundRect(bx, by, bw, 6, 3, col::PV_TRACK);
    const float f = (total > 0) ? clampf(static_cast<float>(secsLeft) / total, 0.f, 1.f) : 0.f;
    if (f > 0.f) {
      spr.fillRoundRect(bx, by, static_cast<int>(bw * f), 6, 3, col::ACCENT);
    }
    char cd[24];
    snprintf(cd, sizeof(cd), "start za %d s", secsLeft);
    plCenter(spr, PLF14, cd, W / 2, 204, col::TEXT_MUTE);
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

void WeatherUi::drawLedTest(const char* colorName) {
  if (!ready_) return;

  uint16_t c = col::TEXT_MUTE;
  if (strcmp(colorName, "CZERWONY") == 0) c = C565(255, 40, 40);
  else if (strcmp(colorName, "ZIELONY") == 0) c = C565(40, 230, 90);
  else if (strcmp(colorName, "NIEBIESKI") == 0) c = C565(60, 130, 255);

  pushBands([&](TFT_eSPI& spr) {
    spr.fillRect(0, 0, W, VIEW_H, col::BG);

    spr.fillRect(0, 0, W, cfg::HEADER_H, col::HEADER);
    spr.drawFastHLine(0, cfg::HEADER_H - 1, W, col::DIVIDER);
    plStr(spr, PLF14, "TEST DIODY RGB", 12, 19, col::ACCENT);

    spr.fillCircle(W / 2, 96, 40, c);
    spr.drawCircle(W / 2, 96, 46, col::DIVIDER);

    plCenter(spr, PLF14, "Dioda powinna teraz świecić na:", W / 2, 152, col::TEXT_DIM);
    plCenter(spr, PLF18, colorName, W / 2, 180, c);
    plCenter(spr, PLF14, "Sprawdź, czy kolory się zgadzają.", W / 2, 202, col::TEXT_MUTE);
  });

  tft_.fillRect(0, VIEW_H, W, cfg::SCREEN_H - VIEW_H, col::BG);
  blTarget_ = cfg::BL_DAY;
  tickBacklight();
}

// ---------------------------------------------- WIDOK 6: STATYSTYKI ----------

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
    snprintf(b, sizeof(b), "v%d · próbna", FW_VERSION);
    viewHeader(spr, ox, "STATYSTYKI URZĄDZENIA", b, col::WARN);
  } else if (d.otaRemote > FW_VERSION) {
    snprintf(b, sizeof(b), "v%d → v%d", FW_VERSION, d.otaRemote);
    viewHeader(spr, ox, "STATYSTYKI URZĄDZENIA", b, col::WARN);
  } else {
    snprintf(b, sizeof(b), "v%d", FW_VERSION);
    viewHeader(spr, ox, "STATYSTYKI URZĄDZENIA", b, col::OK);
  }

  // --- źródła danych: zielona kropka = działa, czerwona = błąd, szara = wyłączone ---
  // "off" = stan neutralny, NIE awaria: falownik śpi po zachodzie (Modbus wyłączony),
  // MQTT jest świadomie wyłączony przez użytkownika. W obu razach kropka szara.
  struct Src {
    const char* name;
    uint32_t okAt;
    const char* err;
    bool off;
    const char* offMsg;
  } src[5] = {
      {"Pogoda", d.weatherOkAt, d.weatherErr, false, ""},
      {"Falownik", d.pvOkAt, d.pvErr, d.pvAsleep, "uśpiony (noc)"},
      {"Radar", d.radarOkAt, d.radarErr, false, ""},
      {"Samoloty", d.flightOkAt, d.flightErr, false, ""},
      {"MQTT", d.mqttOkAt, d.mqttErr, !settings().hasMqtt(), "wyłączony"},
  };

  // 5 wierszy po 15 px zamiast 4 po 17 — karty niżej muszą się zmieścić.
  for (int i = 0; i < 5; ++i) {
    const int y0 = 52 + i * 15;
    if (e < (i + 1) * 0.11f) continue;

    const bool off = src[i].off;
    const bool bad = !off && src[i].err[0] != '\0';
    const bool never = !off && src[i].okAt == 0;
    const uint16_t dot = off ? col::TEXT_MUTE : (bad ? col::ERR : (never ? col::WARN : col::OK));

    spr.fillCircle(ox + 12, y0 + 6, 4, dot);
    plStr(spr, PLF14, src[i].name, ox + 24, y0 + 11, off ? col::TEXT_MUTE : col::TEXT);

    if (off) {
      // PlFont, nie GLCD: "wyłączony" i "uśpiony" mają polskie znaki, których
      // GLCD nie zna (wychodziło "wy czony").
      plRight(spr, PLF14, src[i].offMsg, ox + W - 10, y0 + 11, col::TEXT_MUTE);
    } else if (bad) {
      glRight(spr, src[i].err, ox + W - 10, y0 + 3, col::ERR);
    } else if (never) {
      glRight(spr, "czekam...", ox + W - 10, y0 + 3, col::TEXT_MUTE);
    } else {
      const uint32_t ago = (now - src[i].okAt) / 1000;
      if (ago < 90) {
        snprintf(b, sizeof(b), "OK  %lus temu", static_cast<unsigned long>(ago));
      } else {
        snprintf(b, sizeof(b), "OK  %lu min temu", static_cast<unsigned long>(ago / 60));
      }
      glRight(spr, b, ox + W - 10, y0 + 3, col::TEXT_DIM);
    }
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

  // minimalna sterta jest ważniejsza niż bieżąca — to ona ostrzega przed padem
  cards[1] = {"WOLNY RAM", {0}, {0},
              minHeap < cfg::HEAP_DANGER
                  ? col::ERR
                  : (minHeap < cfg::HEAP_WARN ? col::WARN : col::OK)};
  snprintf(cards[1].value, sizeof(cards[1].value), "%lu kB",
           static_cast<unsigned long>(heap / 1024));
  snprintf(cards[1].sub, sizeof(cards[1].sub), "min %lu kB",
           static_cast<unsigned long>(minHeap / 1024));

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

  const int cy0 = 128, chh = 44;
  for (int i = 0; i < 3; ++i) {
    const int x = ox + 6 + i * 104;
    const int grow = static_cast<int>(chh * clampf(e * 1.8f - 0.3f - i * 0.15f, 0.f, 1.f));
    if (grow < 5) continue;
    spr.fillRoundRect(x, cy0 + (chh - grow), 100, grow, 6, col::BG_CARD);
    if (grow < chh - 2) continue;
    spr.fillRoundRect(x, cy0, 3, chh, 1, cards[i].color);
    gl(spr, cards[i].label, x + 9, cy0 + 4, col::TEXT_MUTE);
    plStr(spr, PLF18, cards[i].value, x + 9, cy0 + 30, cards[i].color);
    gl(spr, cards[i].sub, x + 9, cy0 + 34, col::TEXT_MUTE);

    // wskaźnik po prawej stronie kafelka (temperatura / sterta)
    const int gx = x + 82, gy = cy0 + 7, gw = 11, gh = 30;
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

  // --- sieć: siła sygnału + adres panelu ---
  const int rssi = WiFi.RSSI();
  const int bars = rssi >= -55 ? 4 : (rssi >= -65 ? 3 : (rssi >= -75 ? 2 : (rssi >= -85 ? 1 : 0)));
  for (int i = 0; i < 4; ++i) {
    const int bh = 4 + i * 3;
    spr.fillRect(ox + 10 + i * 6, 190 - bh, 4, bh,
                  i < bars ? col::ACCENT : col::PV_TRACK);
  }

  snprintf(b, sizeof(b), "%s  %d dBm", WiFi.SSID().c_str(), rssi);
  plStr(spr, PLF14, b, ox + 40, 190, col::TEXT_DIM);

  snprintf(b, sizeof(b), "http://%s", WiFi.localIP().toString().c_str());
  plStr(spr, PLF14, b, ox + 10, 205, col::ACCENT);
  glRight(spr, "panel", ox + W - 10, 197, col::TEXT_MUTE);
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
