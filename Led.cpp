#include "Led.h"

#include <Arduino.h>

#include "Config.h"
#include "Log.h"

namespace {

// ESP32-S3 SuperMini ma diodę WS2812 na GPIO 48 (rdzeń definiuje RGB_BUILTIN).
#ifdef RGB_BUILTIN
constexpr int kPin = RGB_BUILTIN;
#else
constexpr int kPin = 48;
#endif

uint32_t gTestStart = 0;
bool gTestDone = false;

uint8_t gLastR = 255, gLastG = 255, gLastB = 255;

void setRgb(uint8_t r, uint8_t g, uint8_t b) {
  if (r == gLastR && g == gLastG && b == gLastB) {
    return;
  }
  gLastR = r;
  gLastG = g;
  gLastB = b;
  rgbLedWrite(kPin, r, g, b);
}

}  // namespace

void ledBegin() {
  gTestStart = millis();
  gTestDone = false;
  gLastR = gLastG = gLastB = 255;
  setRgb(0, 0, 0);
  LOG("LED: dioda RGB na GPIO %d\n", kPin);
}

// 3 x 1,5 s: czerwony -> zielony -> niebieski. Pełna jasność, żeby dało się
// jednoznacznie stwierdzić, czy R to naprawdę R.
const char* ledTestStep() {
  if (gTestDone) {
    return nullptr;
  }
  const uint32_t t = millis() - gTestStart;
  if (t < 1500) {
    setRgb(160, 0, 0);
    return "CZERWONY";
  }
  if (t < 3000) {
    setRgb(0, 160, 0);
    return "ZIELONY";
  }
  if (t < 4500) {
    setRgb(0, 0, 160);
    return "NIEBIESKI";
  }
  setRgb(0, 0, 0);
  gTestDone = true;
  return nullptr;
}

void ledShowGrid(int32_t gridW, bool pvOnline, bool night) {
  if (!gTestDone) {
    return;   // test ma pierwszeństwo
  }
  if (!pvOnline) {
    setRgb(0, 0, 0);
    return;
  }

  // W nocy przygaszamy — dioda wisi w pokoju, nie ma oślepiać.
  const uint8_t lvl = night ? cfg::LED_NIGHT : cfg::LED_DAY;

  if (gridW > cfg::LED_BALANCE_W) {
    setRgb(0, lvl, 0);                      // oddaję do sieci
  } else if (gridW < -cfg::LED_BALANCE_W) {
    setRgb(lvl, 0, 0);                      // pobieram z sieci
  } else {
    setRgb(0, 0, lvl);                      // równowaga
  }
}
