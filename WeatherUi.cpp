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
  if (spr_.createSprite(cfg::SCREEN_W, SPR_H) == nullptr) {
    return false;
  }
  spr_.setSwapBytes(false);
  spr_.fillSprite(col::BG);
  spr_.pushSprite(0, 0);
  tft_.fillRect(0, SPR_H, cfg::SCREEN_W, cfg::SCREEN_H - SPR_H, col::BG);

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
  spr_.fillSprite(col::BG);

  // delikatna poświata u góry
  for (int y = 0; y < 60; ++y) {
    const uint16_t c = lerp565(col::HEADER, col::BG, y / 60.f);
    spr_.drawFastHLine(0, y, W, c);
  }

  wxico::draw(spr_, 0, W / 2, 92, 64);
  plCenter(spr_, PLF18, settings().city, W / 2, 148, col::TEXT);
  plCenter(spr_, PLF14, status, W / 2, 172, col::TEXT_DIM);

  // pasek postępu — animowany "knight rider"
  const int bx = 70, bw = 180, by = 182;
  spr_.fillRoundRect(bx, by, bw, 6, 3, col::PV_TRACK);
  const uint32_t ph = (millis() / 12) % (bw + 60);
  const int sx = bx + static_cast<int>(ph) - 60;
  for (int i = 0; i < 60; ++i) {
    const int x = sx + i;
    if (x < bx || x >= bx + bw) continue;
    spr_.drawFastVLine(x, by, 6, lerp565(col::PV_TRACK, col::ACCENT, i / 59.f));
  }

  if (attempt > 1) {
    char b[32];
    snprintf(b, sizeof(b), "próba %d", attempt);
    plCenter(spr_, PLF14, b, W / 2, 202, col::TEXT_MUTE);
  }
  spr_.pushSprite(0, 0);
  tft_.fillRect(0, SPR_H, W, cfg::SCREEN_H - SPR_H, col::BG);
  if (blTarget_ == 0) blTarget_ = cfg::BL_DAY;
  tickBacklight();
}

void WeatherUi::drawFatal(const char* msg) {
  if (!ready_) return;
  spr_.fillSprite(col::BG);
  spr_.fillRoundRect(20, 78, W - 40, 86, 8, col::ALERT_BG);
  spr_.drawRoundRect(20, 78, W - 40, 86, 8, col::ERR);
  plCenter(spr_, PLF18, "Błąd", W / 2, 112, col::ERR);
  plCenter(spr_, PLF14, msg, W / 2, 142, col::TEXT);
  spr_.pushSprite(0, 0);
}

void WeatherUi::drawColorTest() {
  if (!ready_) return;
  spr_.fillSprite(TFT_BLACK);
  spr_.fillRect(0, 0, W, 68, TFT_RED);
  spr_.fillRect(0, 68, W, 68, TFT_GREEN);
  spr_.fillRect(0, 136, W, 70, TFT_BLUE);
  plStr(spr_, PLF18, "CZERWONY", 12, 42, TFT_WHITE);
  plStr(spr_, PLF18, "ZIELONY", 12, 110, TFT_BLACK);
  plStr(spr_, PLF18, "NIEBIESKI", 12, 178, TFT_WHITE);
  spr_.pushSprite(0, 0);
  blTarget_ = cfg::BL_DAY;
}

// ------------------------------------------------------------------- chrome ----

void WeatherUi::drawHeader(const WeatherModel& w, bool wifiOk, uint32_t nowMs) {
  spr_.fillRect(0, 0, W, cfg::HEADER_H, col::HEADER);
  spr_.drawFastHLine(0, cfg::HEADER_H - 1, W, col::DIVIDER);

  // status sieci — kropka + delikatny puls przy braku WiFi
  uint16_t dot = wifiOk ? col::OK : col::ERR;
  if (!wifiOk && ((nowMs / 500) % 2)) {
    dot = col::BG_CARD;
  }
  spr_.fillCircle(12, 14, 4, dot);

  plStr(spr_, PLF14, settings().city, 24, 19, col::TEXT);

  time_t now = time(nullptr);
  if (now < 1700000000) {
    plRight(spr_, PLF18, "--:--", W - 10, 21, col::TEXT_MUTE);
    plCenter(spr_, PLF14, "synchronizacja czasu", W / 2 + 10, 19, col::TEXT_MUTE);
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
  plCenter(spr_, PLF14, mid, W / 2 + 10, 19, col::TEXT_DIM);

  char clk[8];
  snprintf(clk, sizeof(clk), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
  plRight(spr_, PLF18, clk, W - 10, 21, col::ACCENT);
}

void WeatherUi::drawProgress(uint32_t nowMs) {
  spr_.fillRect(0, cfg::PROG_Y, W, cfg::PROG_H + 2, col::BG);

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
    spr_.fillRect(x, cfg::PROG_Y, wSeg, cfg::PROG_H, col::PV_TRACK);
    if (i < view_) {
      spr_.fillRect(x, cfg::PROG_Y, wSeg, cfg::PROG_H, col::GRID_HI);
    } else if (i == view_) {
      spr_.fillRect(x, cfg::PROG_Y, static_cast<int>(wSeg * frac), cfg::PROG_H,
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
  if (footerInit_ && ac == lastAc_ && g == lastGrid_ && kwh == lastKwh_ &&
      cpu == lastCpu_ && pv.online == lastOnline_) {
    return;
  }
  footerInit_ = true;
  lastAc_ = ac;
  lastGrid_ = g;
  lastKwh_ = kwh;
  lastCpu_ = cpu;
  lastOnline_ = pv.online;

  drawFooterTo(tft_, pv, wifiOk);
}

// Ta sama stopka, ale na dowolnym celu — TFT albo tymczasowy sprite (zrzut ekranu).
void WeatherUi::drawFooterTo(TFT_eSPI& dst, const PvModel& pv, bool wifiOk) {
  const int y = (&dst == &tft_) ? 206 : 0;   // w sprite'cie stopka zaczyna sie od 0

  dst.fillRect(0, y, W, 34, col::HEADER);
  dst.drawFastHLine(0, y, W, col::DIVIDER);

  if (!pv.online) {
    const bool connecting = (pv.errorMsg[0] == '\0');
    dst.fillCircle(14, y + 17, 4, connecting ? col::WARN : col::ERR);
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

void WeatherUi::drawContentBg() {
  spr_.fillRect(0, CY, W, CH, col::BG);
}

// ------------------------------------------------------------ WIDOK 1: TERAZ --

void WeatherUi::drawViewNow(int ox, float t, const WeatherModel& w) {
  const WeatherSnapshot& c = w.current;
  const float e = easeOutCubic(t);

  // --- wielka temperatura ---
  char big[12];
  fmtTemp(big, sizeof(big), c.tempC);
  const uint16_t tc = tempColor(c.tempC);
  const int bw = bigStr(spr_, &FreeSansBold24pt7b, big, ox + 12, 96, tc);
  plStr(spr_, PLF18, "°C", ox + 16 + bw, 70, col::TEXT_DIM);
  gl(spr_, "TEMPERATURA", ox + 13, 54, col::TEXT_MUTE);

  char feels[32];
  snprintf(feels, sizeof(feels), "odczuwalna %d°C", static_cast<int>(lroundf(c.feelsC)));
  plStr(spr_, PLF14, feels, ox + 12, 114, col::TEXT_DIM);

  // --- ikona + opis ---
  const int icx = ox + 258;
  const int size = 40 + static_cast<int>(24 * e);
  wxico::draw(spr_, c.weatherCode, icx, 76, size);
  plCenter(spr_, PLF14, wxico::labelForCode(c.weatherCode), icx, 114, col::TEXT);

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
    spr_.fillRoundRect(x, yy, cw, grow, 6, col::BG_CARD);
    if (grow < chh - 2) continue;

    spr_.fillRect(x, cy0, 3, chh, cards[i].color);
    spr_.fillRoundRect(x, cy0, 3, chh, 1, cards[i].color);

    plStr(spr_, PLF14, cards[i].label, x + 8, cy0 + 17, col::TEXT_DIM);
    if (cards[i].extra != nullptr) {
      plRight(spr_, PLF14, cards[i].extra, x + cw - 7, cy0 + 17, cards[i].color);
    }
    const int vw = pltxt::drawString(spr_, PLF18, cards[i].value, x + 8, cy0 + 43, col::TEXT,
                                     col::TEXT);
    gl(spr_, cards[i].unit, x + 10 + vw, cy0 + 36, col::TEXT_MUTE);
  }

  // --- pasek dolny: wschód / zachód / UV ---
  const int by = 196;
  spr_.fillCircle(ox + 14, by - 4, 5, col::SUN);
  spr_.fillRect(ox + 8, by - 3, 13, 5, col::BG);
  char b[24];
  snprintf(b, sizeof(b), "%s", w.sunrise[0] ? w.sunrise : "--:--");
  plStr(spr_, PLF14, b, ox + 26, by, col::TEXT_DIM);

  spr_.fillCircle(ox + 118, by - 4, 5, col::ACCENT_WARM);
  spr_.fillRect(ox + 112, by - 9, 13, 5, col::BG);
  snprintf(b, sizeof(b), "%s", w.sunset[0] ? w.sunset : "--:--");
  plStr(spr_, PLF14, b, ox + 130, by, col::TEXT_DIM);

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
  plRight(spr_, PLF14, b, ox + W - 10, by, uvc);
  // Opad — priorytet ma RADAR (realny pomiar). Model bywa ślepy na lokalne ulewy.
  if (w.radarValid && w.radarLevel > 0) {
    const uint16_t rc = w.radarLevel >= 4 ? col::ERR
                        : (w.radarLevel == 3 ? col::WARN : col::RAIN);
    snprintf(b, sizeof(b), "RADAR: %s", radarLabel(w.radarLevel));
    plCenter(spr_, PLF14, b, ox + 218, by, rc);
  } else if (c.precipMm > 0.05f) {
    snprintf(b, sizeof(b), "deszcz %.1f mm/h", c.precipMm);
    plCenter(spr_, PLF14, b, ox + 218, by, col::RAIN);
  } else {
    float next12 = 0.f;
    for (int i = 0; i < WX_HOURS; ++i) {
      if (w.hours[i].valid) next12 += w.hours[i].data.precipMm;
    }
    if (next12 > 0.2f) {
      snprintf(b, sizeof(b), "opad 12h %.1f mm", next12);
      plCenter(spr_, PLF14, b, ox + 218, by, col::RAIN_DK);
    }
  }
}

// --------------------------------------------------------- WIDOK 2: GODZINY --

void WeatherUi::drawViewHours(int ox, float t, const WeatherModel& w) {
  const float e = easeOutCubic(t);

  plStr(spr_, PLF14, "NAJBLIŻSZE 12 GODZIN", ox + 10, 46, col::ACCENT);
  glRight(spr_, "TEMP / OPAD", ox + W - 10, 38, col::TEXT_MUTE);

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
    drawNoData(ox, "Brak danych godzinowych");
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
      spr_.drawPixel(x, y, col::GRID);
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
      spr_.drawFastVLine(x, ya, yBot - ya, lerp565(col::BG, lc, 0.16f));
      spr_.drawFastVLine(x, ya - 1, 3, lc);
    }
  }

  // punkty + wartości co 2 godziny
  for (int i = 0; i < 13; ++i) {
    if (!ok[i]) continue;
    const int x = px(i);
    const int y = yBot - static_cast<int>((yBot - py(i)) * e);
    if (i % 2 == 0) {
      spr_.fillCircle(x, y, 3, col::BG);
      spr_.fillCircle(x, y, 2, tempColor(temp[i]));
      char b[8];
      fmtTempInt(b, sizeof(b), temp[i]);
      plCenter(spr_, PLF14, b, x, y - 7, col::TEXT);
    }
  }

  // opady
  const int rBase = 156;
  spr_.drawFastHLine(ox + 12, rBase, W - 24, col::GRID_HI);
  if (rmax < 0.05f) {
    int prob = 0;
    for (int i = 0; i < WX_HOURS; ++i) {
      if (w.hours[i].valid && w.hours[i].data.precipProb > prob) {
        prob = w.hours[i].data.precipProb;
      }
    }
    char b[40];
    snprintf(b, sizeof(b), "bez opadów  (max szansa %d%%)", prob);
    plCenter(spr_, PLF14, b, ox + W / 2, rBase - 6, col::TEXT_MUTE);
  } else {
    const float scale = rmax < 1.f ? 1.f : rmax;
    for (int i = 1; i < 13; ++i) {
      if (!ok[i] || rain[i] <= 0.f) continue;
      const int h = static_cast<int>((rain[i] / scale) * 26.f * e);
      if (h < 1) continue;
      const int x = px(i) - 6;
      spr_.fillRoundRect(x, rBase - h, 12, h, 2, col::RAIN);
    }
    char b[16];
    snprintf(b, sizeof(b), "%.1f mm", rmax);
    glRight(spr_, b, ox + W - 12, rBase - 34, col::RAIN);
    gl(spr_, "OPAD", ox + 12, rBase - 34, col::RAIN_DK);
  }

  // godziny + ikony
  for (int i = 0; i <= 12; i += 2) {
    if (!ok[i]) continue;
    const int x = px(i);
    char h[8];
    snprintf(h, sizeof(h), "%02d", hourLbl[i]);
    glCenter(spr_, h, x, 160, i == 0 ? col::ACCENT : col::TEXT_DIM);
    wxico::draw(spr_, code[i], x, 186, 24);
  }
}

// --------------------------------------------------------- WIDOK 3: 5 DNI ----

void WeatherUi::drawViewDays(int ox, float t, const WeatherModel& w) {
  const float e = easeOutCubic(t);

  plStr(spr_, PLF14, "PROGNOZA 5 DNI", ox + 10, 46, col::ACCENT);
  glRight(spr_, "MAX / MIN °C", ox + W - 10, 38, col::TEXT_MUTE);

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
    drawNoData(ox, "Brak prognozy dziennej");
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

    wxico::draw(spr_, d.weatherCode, cx, 66, 28);

    char b[12];
    fmtTempInt(b, sizeof(b), d.tempMax);
    plCenter(spr_, PLF14, b, cx, 95, tempColor(d.tempMax));

    // słupek zakresu temperatur — rośnie od dołu
    const int y1 = mapY(d.tempMax);
    const int y0 = mapY(d.tempMin);
    const int full = y0 - y1;
    const int h = static_cast<int>(full * clampf(e * 1.25f - i * 0.06f, 0.f, 1.f));
    const int bx = cx - 8;
    spr_.fillRoundRect(bx, yTop, 16, yBot - yTop, 8, col::PV_TRACK);
    if (h > 2) {
      const int top = y0 - h;
      for (int y = top; y <= y0; ++y) {
        const float f = (y0 == top) ? 0.f : static_cast<float>(y0 - y) / (y0 - top);
        const float tv = d.tempMin + (d.tempMax - d.tempMin) * f;
        spr_.drawFastHLine(bx + 1, y, 14, tempColor(tv));
      }
      spr_.fillRoundRect(bx, top, 16, 5, 2, tempColor(d.tempMax));
      spr_.fillRoundRect(bx, y0 - 4, 16, 5, 2, tempColor(d.tempMin));
    }

    fmtTempInt(b, sizeof(b), d.tempMin);
    plCenter(spr_, PLF14, b, cx, 171, col::TEXT_DIM);

    // opad — pasek pod słupkiem
    if (d.precipMm > 0.2f) {
      const float scale = rmax < 4.f ? 4.f : rmax;
      int rw = static_cast<int>((d.precipMm / scale) * 44.f * e);
      if (rw < 3) rw = 3;
      if (rw > 44) rw = 44;
      spr_.fillRoundRect(cx - 22, 176, 44, 4, 2, col::PV_TRACK);
      spr_.fillRoundRect(cx - 22, 176, rw, 4, 2, col::RAIN);
    }

    plCenter(spr_, PLF14, d.name, cx, 195, i == 0 ? col::ACCENT : col::TEXT);
    glCenter(spr_, d.date, cx, 197, col::TEXT_MUTE);
  }
}

void WeatherUi::drawNoData(int ox, const char* msg) {
  spr_.fillRoundRect(ox + 40, CY + 50, W - 80, 60, 8, col::BG_CARD);
  plCenter(spr_, PLF14, msg, ox + W / 2, CY + 86, col::TEXT_DIM);
}

// ------------------------------------------------------------- WIDOK 4: PV ----

void WeatherUi::drawViewPv(int ox, float t, const PvModel& pv, const PvHistory& hist) {
  const float e = easeOutCubic(t);
  plStr(spr_, PLF14, "FOTOWOLTAIKA", ox + 10, 46, col::PV_SOLAR);

  if (!pv.online) {
    drawNoData(ox, pv.errorMsg[0] ? pv.errorMsg : "Falownik nie odpowiada");
    return;
  }

  const PvSnapshot& d = pv.data;

  // status
  const bool fault = pvStatusIsFault(d.statusCode);
  const uint16_t sc = fault ? col::ERR : (pvStatusIsRunning(d.statusCode) ? col::OK : col::WARN);
  const char* sl = pvStatusLabel(d.statusCode);
  const int slw = pltxt::stringWidth(PLF14, sl);
  spr_.fillCircle(ox + W - 16 - slw - 10, 42, 4, sc);
  plRight(spr_, PLF14, sl, ox + W - 12, 46, sc);

  // --- wielka moc AC (animowana) ---
  const int32_t acNow = static_cast<int32_t>(lroundf(animAcW_));
  char v[16], u[8];
  fmtPower(v, sizeof(v), u, sizeof(u), acNow);
  gl(spr_, "MOC CHWILOWA", ox + 13, 54, col::TEXT_MUTE);
  const int pw = bigStr(spr_, &FreeSansBold24pt7b, v, ox + 12, 100, col::PV_SOLAR);
  plStr(spr_, PLF18, u, ox + 16 + pw, 74, col::TEXT_DIM);

  char sub[40];
  snprintf(sub, sizeof(sub), "DC %ld W   |   %.0f V", static_cast<long>(d.powerDcW),
           d.pvVoltageV);
  gl(spr_, sub, ox + 13, 108, col::TEXT_DIM);

  // --- wskaźnik (arc) ---
  if (pvScaleW_ < static_cast<float>(settings().pvPeakW)) pvScaleW_ = static_cast<float>(settings().pvPeakW);
  if (static_cast<float>(d.powerAcW) > pvScaleW_) pvScaleW_ = static_cast<float>(d.powerAcW) * 1.05f;

  const int gx = ox + 266, gy = 88, gr = 34, gir = 26;
  const float frac = clampf(animAcW_ / pvScaleW_, 0.f, 1.f) * e;
  glCenter(spr_, "OBCIAZENIE", gx, 48, col::TEXT_MUTE);
  spr_.drawSmoothArc(gx, gy, gr, gir, 30, 330, col::PV_TRACK, col::BG, true);
  if (frac > 0.005f) {
    const uint32_t end = 30 + static_cast<uint32_t>(300.f * frac);
    const uint16_t ac = lerp565(col::PV_SOLAR, col::PV_EXPORT, clampf(frac, 0.f, 1.f));
    spr_.drawSmoothArc(gx, gy, gr, gir, 30, end, ac, col::BG, true);
  }
  char pct[8];
  snprintf(pct, sizeof(pct), "%d%%", static_cast<int>(lroundf(frac * 100.f)));
  plCenter(spr_, PLF18, pct, gx, gy + 3, col::TEXT);
  char peak[14];
  snprintf(peak, sizeof(peak), "z %.1f kWp", pvScaleW_ / 1000.f);
  glCenter(spr_, peak, gx, gy + 9, col::TEXT_MUTE);

  // --- sparkline: profil produkcji dziś ---
  gl(spr_, "PRODUKCJA DZIS", ox + 12, 122, col::TEXT_MUTE);
  char tot[24];
  snprintf(tot, sizeof(tot), "RAZEM %.0f kWh", d.energyTotalKwh);
  glRight(spr_, tot, ox + W - 12, 122, col::TEXT_MUTE);

  const int sx = ox + 12, sy = 132, sw = W - 24, sh = 26;
  spr_.fillRoundRect(sx, sy, sw, sh, 3, col::CHART_SPARK_BG);
  const int s0 = 30, s1 = 137;  // 05:00 .. 22:50
  uint16_t hpk = hist.peak();
  if (hpk < 500) hpk = 500;
  for (int s = s0; s <= s1; ++s) {
    if (!hist.filled[s] || hist.watts[s] == 0) continue;
    const int x = sx + ((s - s0) * (sw - 2)) / (s1 - s0);
    int h = static_cast<int>((static_cast<float>(hist.watts[s]) / hpk) * (sh - 3) * e);
    if (h < 1) h = 1;
    spr_.drawFastVLine(x, sy + sh - 1 - h, h, col::PV_SOLAR);
    spr_.drawFastVLine(x + 1, sy + sh - 1 - h, h, col::PV_SOLAR_DK);
  }
  // znaczniki godzin 8 / 12 / 16 / 20
  for (int hh = 8; hh <= 20; hh += 4) {
    const int s = hh * 6;
    const int x = sx + ((s - s0) * (sw - 2)) / (s1 - s0);
    spr_.drawFastVLine(x, sy + sh - 3, 3, col::GRID_HI);
  }

  // --- 4 kafelki ---
  struct PvCard {
    const char* label;
    char value[16];
    uint16_t color;
  } cards[4];

  cards[0] = {"Dziś", {0}, col::PV_SOLAR};
  snprintf(cards[0].value, sizeof(cards[0].value), "%.1f kWh", d.energyTodayKwh);

  cards[1] = {"Dom", {0}, col::PV_HOUSE};
  fmtPower(v, sizeof(v), u, sizeof(u), static_cast<int32_t>(lroundf(animLoadW_)));
  snprintf(cards[1].value, sizeof(cards[1].value), "%s %s", v, u);

  const int32_t g = static_cast<int32_t>(lroundf(animGridW_));
  const bool exporting = g >= 0;
  cards[2] = {exporting ? "Oddaję" : "Pobór", {0}, exporting ? col::PV_EXPORT : col::PV_IMPORT};
  fmtPower(v, sizeof(v), u, sizeof(u), g < 0 ? -g : g);
  snprintf(cards[2].value, sizeof(cards[2].value), "%s %s", v, u);

  cards[3] = {"Falownik", {0}, d.inverterTempC > 65.f ? col::WARN : col::TEXT_DIM};
  snprintf(cards[3].value, sizeof(cards[3].value), "%.0f °C", d.inverterTempC);

  const int cy0 = 164, chh = 41;
  for (int i = 0; i < 4; ++i) {
    const int x = ox + 6 + i * 78;
    const int grow = static_cast<int>(chh * clampf(e * 1.3f - i * 0.08f, 0.f, 1.f));
    if (grow < 4) continue;
    spr_.fillRoundRect(x, cy0 + (chh - grow), 74, grow, 6, col::BG_CARD);
    if (grow < chh - 2) continue;
    spr_.fillRoundRect(x, cy0, 3, chh, 1, cards[i].color);
    plStr(spr_, PLF14, cards[i].label, x + 8, cy0 + 14, col::TEXT_DIM);
    plStr(spr_, PLF18, cards[i].value, x + 8, cy0 + 37, cards[i].color);
  }
}

// ------------------------------------------------------------------- ALERT ----

void WeatherUi::drawAlert(float t) {
  const float e = easeOutCubic(t);
  const int pad = static_cast<int>(20 * (1.f - e));
  const int x = 8 + pad, y = CY + 4 + pad / 2;
  const int w = W - 16 - pad * 2, h = CH - 12 - pad;

  spr_.fillRoundRect(x, y, w, h, 10, col::ALERT_BG);
  spr_.drawRoundRect(x, y, w, h, 10, alert_.color);
  spr_.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 9, alert_.color);

  if (alert_.iconCode >= 0) {
    wxico::draw(spr_, alert_.iconCode, x + 52, y + 76, 56);
  } else {
    // trójkąt ostrzegawczy
    const int cx = x + 52, cy = y + 76;
    spr_.fillTriangle(cx, cy - 26, cx - 28, cy + 22, cx + 28, cy + 22, alert_.color);
    spr_.fillRect(cx - 3, cy - 12, 6, 20, col::ALERT_BG);
    spr_.fillRect(cx - 3, cy + 12, 6, 6, col::ALERT_BG);
  }

  plStr(spr_, PLF18, alert_.title, x + 100, y + 62, alert_.color);
  plStr(spr_, PLF14, alert_.text, x + 100, y + 92, col::TEXT);
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

void WeatherUi::drawView(uint8_t view, int ox, float t, const WeatherModel& w, const PvModel& pv,
                         const PvHistory& hist, const FlightModel& fl) {
  switch (view) {
    case 0:
      if (w.ready) drawViewNow(ox, t, w);
      else drawNoData(ox, "Pobieram prognozę...");
      break;
    case 1:
      if (w.ready) drawViewHours(ox, t, w);
      else drawNoData(ox, "Pobieram prognozę...");
      break;
    case 2:
      if (w.ready) drawViewDays(ox, t, w);
      else drawNoData(ox, "Pobieram prognozę...");
      break;
    case 3:
      drawViewPv(ox, t, pv, hist);
      break;
    case 4:
      drawViewFlights(ox, t, fl);
      break;
    case 5:
      drawViewStats(ox, t);
      break;
    default:
      break;
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

  // --- rotacja widoków ---
  if (!alertActive_) {
    if (transitioning_) {
      if (nowMs - transStart_ >= cfg::TRANSITION_MS) {
        transitioning_ = false;
        viewStart_ = nowMs;
      }
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

  // --- rysowanie ---
  drawContentBg();

  if (alertActive_) {
    drawAlert(clampf(static_cast<float>(nowMs - alertStart_) / 260.f, 0.f, 1.f));
  } else if (transitioning_) {
    const float p =
        easeOutCubic(static_cast<float>(nowMs - transStart_) / cfg::TRANSITION_MS);
    const int off = static_cast<int>(p * W);
    drawView(prevView_, -off, 1.f, w, pv, hist, fl);
    drawView(view_, W - off, enterT, w, pv, hist, fl);
  } else {
    drawView(view_, 0, enterT, w, pv, hist, fl);
  }

  drawHeader(w, wifiOk, nowMs);
  drawProgress(nowMs);
  spr_.pushSprite(0, 0);
  drawFooter(pv, wifiOk);   // poza buforem, wprost na TFT
  tickBacklight();

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

void WeatherUi::drawViewFlights(int ox, float t, const FlightModel& fl) {
  const float e = easeOutCubic(t);
  const int mx = ox;
  const int my = CY;

  // --- morze ---
  spr_.fillRect(mx, my, gmap::MAP_W, gmap::MAP_H, col::MAP_SEA);

  // --- ląd (pasy poziome z prekalkulowanej rasteryzacji) ---
  for (int row = 0; row < gmap::MAP_H; ++row) {
    const uint16_t a = pgm_read_word(&gmap::LAND_ROW_OFF[row]);
    const uint16_t b = pgm_read_word(&gmap::LAND_ROW_OFF[row + 1]);
    for (uint16_t s = a; s < b; ++s) {
      const uint8_t x0 = pgm_read_byte(&gmap::LAND_SPANS[s][0]);
      const uint8_t x1 = pgm_read_byte(&gmap::LAND_SPANS[s][1]);
      spr_.drawFastHLine(mx + x0, my + row, x1 - x0 + 1, col::MAP_LAND);
    }
  }

  // --- linia brzegowa ---
  int px = 0, py = 0;
  for (int i = 0; i < gmap::COAST_COUNT; ++i) {
    const int16_t x = static_cast<int16_t>(pgm_read_word(&gmap::COAST_PTS[i][0]));
    const int16_t y = static_cast<int16_t>(pgm_read_word(&gmap::COAST_PTS[i][1]));
    if (i > 0) {
      spr_.drawLine(mx + px, my + py, mx + x, my + y, col::MAP_COAST);
    }
    px = x;
    py = y;
  }

  // --- etykiety miejsc ---
  int lx, ly;
  mapProject(54.6053f, 18.8026f, ox, lx, ly);
  gl(spr_, "HEL", lx + 5, ly - 3, col::MAP_LABEL);

  mapProject(54.3500f, 18.6600f, ox, lx, ly);
  gl(spr_, "GDANSK", lx - 12, ly, col::MAP_LABEL);

  mapProject(54.7186f, 18.4092f, ox, lx, ly);
  glRight(spr_, "PUCK", lx - 3, ly - 4, col::MAP_LABEL);

  // --- lotnisko EPGD ---
  mapProject(cfg::EPGD_LAT, cfg::EPGD_LON, ox, lx, ly);
  spr_.drawRect(lx - 3, ly - 3, 7, 7, col::TEXT);
  spr_.drawFastHLine(lx - 5, ly, 11, col::TEXT);
  gl(spr_, "EPGD", lx + 8, ly - 3, col::TEXT_DIM);

  // --- dom (Częstochowska) ---
  mapProject(settings().lat, settings().lon, ox, lx, ly);
  spr_.drawCircle(lx, ly, 6, col::ACCENT);
  spr_.fillCircle(lx, ly, 3, col::ACCENT);
  gl(spr_, "GDYNIA", lx - 40, ly - 4, col::ACCENT);

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
    spr_.fillTriangle(tx, ty, ax, ay, bx, by, c);
    spr_.drawTriangle(tx, ty, ax, ay, bx, by, col::BG);

    // numer wiążący z listą
    int nx = x + 12;
    int ny = y - 12;
    if (nx > mx + gmap::MAP_W - 9) nx = x - 12;
    if (ny < my + 9) ny = y + 12;
    if (ny > my + gmap::MAP_H - 9) ny = y - 12;
    spr_.fillCircle(nx, ny, 7, c);
    spr_.drawCircle(nx, ny, 7, col::BG);
    char nb[4];
    snprintf(nb, sizeof(nb), "%d", i + 1);
    glCenter(spr_, nb, nx, ny - 3, col::BG);
  }

  // --- legenda ---
  const int ly0 = my + gmap::MAP_H - 12;
  spr_.fillRect(mx, ly0, gmap::MAP_W, 12, col::BG);
  spr_.fillCircle(mx + 7, ly0 + 6, 4, col::FLY_ARRIVE);
  gl(spr_, "przylot GDN", mx + 14, ly0 + 2, col::TEXT_DIM);
  spr_.fillCircle(mx + 100, ly0 + 6, 4, col::FLY_DEPART);
  gl(spr_, "odlot GDN", mx + 107, ly0 + 2, col::TEXT_DIM);
  spr_.fillCircle(mx + 180, ly0 + 6, 4, col::FLY_OVER);
  gl(spr_, "przelot", mx + 187, ly0 + 2, col::TEXT_DIM);

  // ---------------------------------------------------------- panel listy ----
  const int lxp = ox + 228;
  spr_.drawFastVLine(ox + 224, CY, CH, col::DIVIDER);

  plStr(spr_, PLF14, "NA NIEBIE", lxp, 46, col::ACCENT);
  if (fl.ready) {
    char cb[8];
    snprintf(cb, sizeof(cb), "%d", fl.total);
    plRight(spr_, PLF14, cb, ox + 318, 46, col::TEXT_DIM);
  }

  if (!fl.ready) {
    plStr(spr_, PLF14, "Pobieram", lxp, 110, col::TEXT_MUTE);
    plStr(spr_, PLF14, "dane...", lxp, 128, col::TEXT_MUTE);
    return;
  }
  if (fl.count == 0) {
    plStr(spr_, PLF14, "Pusto", lxp, 110, col::TEXT_DIM);
    gl(spr_, "brak samolotow", lxp, 118, col::TEXT_MUTE);
    gl(spr_, "nad zatoka", lxp, 128, col::TEXT_MUTE);
    return;
  }

  for (int i = 0; i < fl.count; ++i) {
    const Flight& f = fl.list[i];
    const uint16_t c = flightColor(f);
    const int y0 = 52 + i * 25;
    const int grow = static_cast<int>(23 * clampf(e * 1.5f - i * 0.09f, 0.f, 1.f));
    if (grow < 6) continue;

    spr_.fillRoundRect(lxp - 2, y0, 92, 23, 4, col::BG_CARD);
    spr_.fillCircle(lxp + 8, y0 + 9, 8, c);
    char nb[4];
    snprintf(nb, sizeof(nb), "%d", i + 1);
    glCenter(spr_, nb, lxp + 8, y0 + 6, col::BG);

    plStr(spr_, PLF14, f.callsign, lxp + 20, y0 + 12, col::TEXT);
    gl(spr_, f.routeKnown ? f.route : "lokalny", lxp + 20, y0 + 15, c);

    char ab[10];
    fmtAlt(ab, sizeof(ab), f.altFt);
    glRight(spr_, ab, ox + 318, y0 + 15, col::TEXT_MUTE);
  }
}

// ------------------------------------------------ EKRAN KONFIGURACJI (AP) ----

void WeatherUi::drawSetup(const char* apSsid, const char* apPass, const char* apIp) {
  if (!ready_) return;
  spr_.fillSprite(col::BG);

  spr_.fillRect(0, 0, W, cfg::HEADER_H, col::HEADER);
  spr_.drawFastHLine(0, cfg::HEADER_H - 1, W, col::DIVIDER);
  plStr(spr_, PLF14, "KONFIGURACJA", 12, 19, col::ACCENT);
  glRight(spr_, "krok 1 z 2", W - 12, 10, col::TEXT_MUTE);

  plStr(spr_, PLF14, "1. Połącz telefon z siecią:", 14, 56, col::TEXT_DIM);

  spr_.fillRoundRect(14, 64, 292, 40, 8, col::BG_CARD);
  spr_.fillRoundRect(14, 64, 4, 40, 2, col::ACCENT);
  plStr(spr_, PLF18, apSsid, 26, 82, col::TEXT);
  plStr(spr_, PLF14, "hasło:", 26, 99, col::TEXT_MUTE);
  plStr(spr_, PLF14, apPass, 68, 99, col::ACCENT);

  plStr(spr_, PLF14, "2. Otwórz w przeglądarce:", 14, 128, col::TEXT_DIM);

  spr_.fillRoundRect(14, 136, 292, 38, 8, col::BG_CARD);
  spr_.fillRoundRect(14, 136, 4, 38, 2, col::ACCENT_WARM);
  char url[32];
  snprintf(url, sizeof(url), "http://%s", apIp);
  plStr(spr_, PLF18, url, 26, 161, col::TEXT);

  plStr(spr_, PLF14, "Tam wybierzesz swoją sieć Wi-Fi,", 14, 186, col::TEXT_MUTE);
  plStr(spr_, PLF14, "lokalizację i adres falownika.", 14, 202, col::TEXT_MUTE);

  // pulsujaca kropka aktywnosci
  const uint32_t ph = (millis() / 400) % 4;
  for (uint32_t i = 0; i < 3; ++i) {
    spr_.fillCircle(292 + i * 8 - 16, 196, 2, (i == ph) ? col::ACCENT : col::PV_TRACK);
  }

  spr_.pushSprite(0, 0);
  tft_.fillRect(0, SPR_H, W, cfg::SCREEN_H - SPR_H, col::BG);
  blTarget_ = cfg::BL_DAY;
  tickBacklight();
}

// ------------------------------------------------------ EKRAN AKTUALIZACJI ---

void WeatherUi::drawOta(int progress, const char* msg) {
  if (!ready_) return;
  spr_.fillSprite(col::BG);

  for (int y = 0; y < 70; ++y) {
    spr_.drawFastHLine(0, y, W, lerp565(col::HEADER, col::BG, y / 70.f));
  }

  // strzalka w dol
  const int cx = W / 2;
  spr_.fillRect(cx - 5, 52, 10, 26, col::ACCENT);
  spr_.fillTriangle(cx, 92, cx - 16, 74, cx + 16, 74, col::ACCENT);
  spr_.fillRoundRect(cx - 22, 100, 44, 5, 2, col::ACCENT);

  plCenter(spr_, PLF18, "Aktualizacja", cx, 134, col::TEXT);
  plCenter(spr_, PLF14, msg && msg[0] ? msg : "Pobieram...", cx, 158, col::TEXT_DIM);

  const int bx = 40, bw = W - 80, by = 176;
  spr_.fillRoundRect(bx, by, bw, 10, 5, col::PV_TRACK);
  const int p = progress < 0 ? 0 : (progress > 100 ? 100 : progress);
  if (p > 0) {
    spr_.fillRoundRect(bx, by, (bw * p) / 100, 10, 5, col::ACCENT);
  }
  char b[8];
  snprintf(b, sizeof(b), "%d%%", p);
  plCenter(spr_, PLF14, b, cx, 208, col::ACCENT);
  plCenter(spr_, PLF14, "Nie odłączaj zasilania", cx, 230, col::TEXT_MUTE);

  spr_.pushSprite(0, 0);
  blTarget_ = cfg::BL_DAY;
  tickBacklight();
}

// ------------------------------------------- EKRAN: POŁĄCZONO / ADRES IP -----

void WeatherUi::drawNetInfo(const char* ssid, const char* ip, int rssi, int secsLeft,
                            int total) {
  if (!ready_) return;
  spr_.fillSprite(col::BG);

  // belka
  spr_.fillRect(0, 0, W, cfg::HEADER_H, col::HEADER);
  spr_.drawFastHLine(0, cfg::HEADER_H - 1, W, col::DIVIDER);
  spr_.fillCircle(12, 14, 4, col::OK);
  plStr(spr_, PLF14, "POŁĄCZONO Z SIECIĄ", 24, 19, col::OK);
  char fw[10];
  snprintf(fw, sizeof(fw), "v%d", FW_VERSION);
  plRight(spr_, PLF14, fw, W - 10, 19, col::TEXT_MUTE);

  // ikona WiFi — łuki o sile zależnej od RSSI
  const int wx = 42, wy = 96;
  const int bars = rssi >= -55 ? 3 : (rssi >= -70 ? 2 : (rssi >= -82 ? 1 : 0));
  for (int i = 0; i < 3; ++i) {
    const int r = 14 + i * 10;
    const uint16_t c = (i < bars) ? col::ACCENT : col::PV_TRACK;
    spr_.drawSmoothArc(wx, wy, r, r - 4, 225, 315, c, col::BG, true);
  }
  spr_.fillCircle(wx, wy, 4, bars > 0 ? col::ACCENT : col::PV_TRACK);

  // sieć
  gl(spr_, "SIEC", 92, 50, col::TEXT_MUTE);
  plStr(spr_, PLF18, ssid, 92, 76, col::TEXT);
  char sig[20];
  snprintf(sig, sizeof(sig), "%d dBm", rssi);
  gl(spr_, sig, 92, 84, col::TEXT_DIM);

  // adres IP — duży, żeby dało się przepisać
  spr_.fillRoundRect(14, 112, W - 28, 62, 10, col::BG_CARD);
  spr_.fillRoundRect(14, 112, 4, 62, 2, col::ACCENT);
  gl(spr_, "ADRES IP URZADZENIA", 30, 120, col::TEXT_MUTE);
  bigStr(spr_, &FreeSansBold18pt7b, ip, 30, 162, col::ACCENT);

  plCenter(spr_, PLF14, "Panel konfiguracji dostępny pod tym adresem", W / 2, 196,
           col::TEXT_DIM);

  // odliczanie
  const int bx = 40, bw = W - 80, by = 186;
  spr_.fillRoundRect(bx, by, bw, 6, 3, col::PV_TRACK);
  const float f = (total > 0) ? clampf(static_cast<float>(secsLeft) / total, 0.f, 1.f) : 0.f;
  if (f > 0.f) {
    spr_.fillRoundRect(bx, by, static_cast<int>(bw * f), 6, 3, col::ACCENT);
  }
  char cd[24];
  snprintf(cd, sizeof(cd), "start za %d s", secsLeft);
  plCenter(spr_, PLF14, cd, W / 2, 204, col::TEXT_MUTE);

  spr_.pushSprite(0, 0);
  tft_.fillRect(0, SPR_H, W, cfg::SCREEN_H - SPR_H, col::BG);
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
  if (spr_.createSprite(cfg::SCREEN_W, SPR_H) == nullptr) {
    Serial.println("UI: nie udalo sie odtworzyc bufora!");
    return false;
  }
  footerInit_ = false;
  spr_.setSwapBytes(false);
  spr_.fillSprite(col::BG);
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
  spr_.fillSprite(col::BG);

  spr_.fillRect(0, 0, W, cfg::HEADER_H, col::HEADER);
  spr_.drawFastHLine(0, cfg::HEADER_H - 1, W, col::DIVIDER);
  plStr(spr_, PLF14, "TEST DIODY RGB", 12, 19, col::ACCENT);

  uint16_t c = col::TEXT_MUTE;
  if (strcmp(colorName, "CZERWONY") == 0) c = C565(255, 40, 40);
  else if (strcmp(colorName, "ZIELONY") == 0) c = C565(40, 230, 90);
  else if (strcmp(colorName, "NIEBIESKI") == 0) c = C565(60, 130, 255);

  spr_.fillCircle(W / 2, 96, 40, c);
  spr_.drawCircle(W / 2, 96, 46, col::DIVIDER);

  plCenter(spr_, PLF14, "Dioda powinna teraz świecić na:", W / 2, 152, col::TEXT_DIM);
  plCenter(spr_, PLF18, colorName, W / 2, 180, c);
  plCenter(spr_, PLF14, "Sprawdź, czy kolory się zgadzają.", W / 2, 202, col::TEXT_MUTE);

  spr_.pushSprite(0, 0);
  tft_.fillRect(0, SPR_H, W, cfg::SCREEN_H - SPR_H, col::BG);
  blTarget_ = cfg::BL_DAY;
  tickBacklight();
}

// ---------------------------------------------- WIDOK 6: STATYSTYKI ----------

void WeatherUi::drawViewStats(int ox, float t) {
  const float e = easeOutCubic(t);
  const Diag& d = diag();
  const uint32_t now = millis();

  plStr(spr_, PLF14, "STATYSTYKI URZĄDZENIA", ox + 10, 46, col::ACCENT);

  char b[48];
  if (d.otaRemote > FW_VERSION) {
    snprintf(b, sizeof(b), "v%d → v%d", FW_VERSION, d.otaRemote);
    plRight(spr_, PLF14, b, ox + W - 10, 46, col::WARN);
  } else {
    snprintf(b, sizeof(b), "v%d", FW_VERSION);
    plRight(spr_, PLF14, b, ox + W - 10, 46, col::OK);
  }

  // --- źródła danych: zielona kropka = działa, czerwona = błąd ---
  struct Src {
    const char* name;
    uint32_t okAt;
    const char* err;
  } src[4] = {
      {"Pogoda", d.weatherOkAt, d.weatherErr},
      {"Falownik", d.pvOkAt, d.pvErr},
      {"Radar", d.radarOkAt, d.radarErr},
      {"Samoloty", d.flightOkAt, d.flightErr},
  };

  for (int i = 0; i < 4; ++i) {
    const int y0 = 54 + i * 17;
    if (e < (i + 1) * 0.12f) continue;

    const bool bad = src[i].err[0] != '\0';
    const bool never = src[i].okAt == 0;
    const uint16_t dot = bad ? col::ERR : (never ? col::WARN : col::OK);

    spr_.fillCircle(ox + 12, y0 + 6, 4, dot);
    plStr(spr_, PLF14, src[i].name, ox + 24, y0 + 11, col::TEXT);

    if (bad) {
      glRight(spr_, src[i].err, ox + W - 10, y0 + 3, col::ERR);
    } else if (never) {
      glRight(spr_, "czekam...", ox + W - 10, y0 + 3, col::TEXT_MUTE);
    } else {
      const uint32_t ago = (now - src[i].okAt) / 1000;
      if (ago < 90) {
        snprintf(b, sizeof(b), "OK  %lus temu", static_cast<unsigned long>(ago));
      } else {
        snprintf(b, sizeof(b), "OK  %lu min temu", static_cast<unsigned long>(ago / 60));
      }
      glRight(spr_, b, ox + W - 10, y0 + 3, col::TEXT_DIM);
    }
  }

  // --- zdrowie: temperatura / RAM / czas pracy ---
  const uint32_t up = now / 1000;
  const uint32_t heap = ESP.getFreeHeap();
  const uint32_t minHeap = d.minHeap == 0xFFFFFFFF ? heap : d.minHeap;

  struct Card {
    const char* label;
    char value[14];
    char sub[16];
    uint16_t color;
  } cards[3];

  cards[0] = {"Temperatura", {0}, {0}, cpuTempC_ >= 75.f ? col::ERR : col::OK};
  snprintf(cards[0].value, sizeof(cards[0].value), "%.0f°C", cpuTempC_);
  snprintf(cards[0].sub, sizeof(cards[0].sub), "rdzeń ESP32");

  // minimalna sterta jest ważniejsza niż bieżąca — to ona ostrzega przed padem
  cards[1] = {"Wolna pamięć", {0}, {0},
              minHeap < 25000 ? col::ERR : (minHeap < 45000 ? col::WARN : col::OK)};
  snprintf(cards[1].value, sizeof(cards[1].value), "%luk", static_cast<unsigned long>(heap / 1024));
  snprintf(cards[1].sub, sizeof(cards[1].sub), "min %luk",
           static_cast<unsigned long>(minHeap / 1024));

  cards[2] = {"Czas pracy", {0}, {0}, col::TEXT};
  if (up < 3600) {
    snprintf(cards[2].value, sizeof(cards[2].value), "%lum", static_cast<unsigned long>(up / 60));
  } else if (up < 86400) {
    snprintf(cards[2].value, sizeof(cards[2].value), "%luh", static_cast<unsigned long>(up / 3600));
  } else {
    snprintf(cards[2].value, sizeof(cards[2].value), "%lud",
             static_cast<unsigned long>(up / 86400));
  }
  snprintf(cards[2].sub, sizeof(cards[2].sub), "restartów %lu",
           static_cast<unsigned long>(d.wifiConnects));

  const int cy0 = 126, chh = 42;
  for (int i = 0; i < 3; ++i) {
    const int x = ox + 6 + i * 104;
    const int grow = static_cast<int>(chh * clampf(e * 1.8f - 0.3f - i * 0.15f, 0.f, 1.f));
    if (grow < 5) continue;
    spr_.fillRoundRect(x, cy0 + (chh - grow), 100, grow, 6, col::BG_CARD);
    if (grow < chh - 2) continue;
    spr_.fillRoundRect(x, cy0, 3, chh, 1, cards[i].color);
    gl(spr_, cards[i].label, x + 9, cy0 + 5, col::TEXT_MUTE);
    const int vw = pltxt::drawString(spr_, PLF18, cards[i].value, x + 9, cy0 + 33,
                                     cards[i].color, cards[i].color);
    gl(spr_, cards[i].sub, x + 12 + vw, cy0 + 26, col::TEXT_MUTE);
  }

  // --- sieć: siła sygnału + adres panelu ---
  const int rssi = WiFi.RSSI();
  const int bars = rssi >= -55 ? 4 : (rssi >= -65 ? 3 : (rssi >= -75 ? 2 : (rssi >= -85 ? 1 : 0)));
  for (int i = 0; i < 4; ++i) {
    const int bh = 4 + i * 3;
    spr_.fillRect(ox + 10 + i * 6, 190 - bh, 4, bh,
                  i < bars ? col::ACCENT : col::PV_TRACK);
  }

  snprintf(b, sizeof(b), "%s  %d dBm", WiFi.SSID().c_str(), rssi);
  plStr(spr_, PLF14, b, ox + 40, 190, col::TEXT_DIM);

  snprintf(b, sizeof(b), "http://%s", WiFi.localIP().toString().c_str());
  plStr(spr_, PLF14, b, ox + 10, 205, col::ACCENT);
  glRight(spr_, "panel", ox + W - 10, 197, col::TEXT_MUTE);
}

// ------------------------------------- ZRZUT EKRANU DO PRZEGLĄDARKI ----------
// BMP 320x240 24-bit, wysyłany wiersz po wierszu — w RAM-ie trzymamy tylko
// jedną linię (960 B), a nie cały obraz (230 kB).

void WeatherUi::streamScreenshot(WiFiClient& client, const PvModel& pv, bool wifiOk) {
  if (!ready_ || freed_) {
    return;
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

  client.print("HTTP/1.1 200 OK\r\nContent-Type: image/bmp\r\n");
  client.printf("Content-Length: %lu\r\n", static_cast<unsigned long>(fileSize));
  client.print("Cache-Control: no-store\r\nConnection: close\r\n\r\n");
  client.write(hdr, sizeof(hdr));

  // Stopka leży poza buforem — odtwarzamy ją w małym sprite'cie tylko na zrzut.
  TFT_eSprite foot(&tft_);
  foot.setColorDepth(16);
  const bool haveFoot = (foot.createSprite(WD, 34) != nullptr);
  if (haveFoot) {
    foot.setSwapBytes(false);
    drawFooterTo(foot, pv, wifiOk);
  }

  static uint8_t line[WD * 3];
  for (int y = HT - 1; y >= 0; --y) {          // BMP idzie od dołu
    for (int x = 0; x < WD; ++x) {
      uint16_t c;
      if (y < SPR_H) {
        c = spr_.readPixel(x, y);
      } else if (haveFoot) {
        c = foot.readPixel(x, y - SPR_H);
      } else {
        c = col::HEADER;
      }
      line[x * 3 + 0] = static_cast<uint8_t>((c & 0x1F) << 3);          // B
      line[x * 3 + 1] = static_cast<uint8_t>(((c >> 5) & 0x3F) << 2);   // G
      line[x * 3 + 2] = static_cast<uint8_t>(((c >> 11) & 0x1F) << 3);  // R
    }
    client.write(line, sizeof(line));
  }

  if (haveFoot) {
    foot.deleteSprite();
  }
  client.flush();
}
