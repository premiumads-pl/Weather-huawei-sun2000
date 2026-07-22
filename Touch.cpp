#include "Touch.h"

#include "Log.h"

#include <Arduino.h>

namespace touch {
namespace {

constexpr uint8_t kPin = 7;          // GPIO7 = TOUCH7 (wolny: TFT ma 8-12 i 14)
constexpr float kRise = 1.25f;       // dotkniecie = odczyt o 25% powyzej bazy
constexpr uint32_t kHoldOffMs = 250;  // debounce — jedno dotkniecie to jedno zdarzenie
constexpr uint32_t kDoubleMs = 550;  // okno na drugie dotkniecie (palec, nie mysz)
constexpr float kDrift = 0.002f;     // jak szybko baza goni otoczenie (tylko gdy nie dotykamy)

uint32_t gBase = 0;
uint32_t gRaw = 0;
bool gDown = false;
uint32_t gLastEvent = 0;
bool gReady = false;

}  // namespace

void begin() {
  // Linia bazowa: srednia z 32 odczytow. Palca ma wtedy nie byc na pinie —
  // gdyby byl, baza wyjdzie za wysoko i pierwszy dotyk sie nie wykryje, ale
  // dryf i tak sciagnie ja z powrotem w ciagu kilkunastu sekund.
  uint64_t sum = 0;
  for (int i = 0; i < 32; ++i) {
    sum += touchRead(kPin);
    delay(3);
  }
  gBase = static_cast<uint32_t>(sum / 32);
  gReady = gBase > 0;

  if (gReady) {
    LOG("Dotyk: GPIO%u gotowy, baza %lu (prog %lu)", kPin,
        static_cast<unsigned long>(gBase),
        static_cast<unsigned long>(gBase * kRise));
  } else {
    LOG("Dotyk: GPIO%u nie odpowiada — wylaczony", kPin);
  }
}

Tap poll() {
  if (!gReady) return Tap::NONE;

  gRaw = touchRead(kPin);
  const uint32_t thr = static_cast<uint32_t>(gBase * kRise);
  const bool down = gRaw > thr;
  const uint32_t now = millis();

  static bool pending = false;      // czekamy, czy nie bedzie drugiego dotkniecia
  static uint32_t pendingAt = 0;

  Tap out = Tap::NONE;

  if (down && !gDown && now - gLastEvent > kHoldOffMs) {
    gLastEvent = now;
    if (pending && now - pendingAt <= kDoubleMs) {
      pending = false;
      out = Tap::DOUBLE;
    } else {
      pending = true;
      pendingAt = now;
    }
  } else if (pending && now - pendingAt > kDoubleMs) {
    pending = false;
    out = Tap::SINGLE;   // drugie nie przyszlo — to jednak bylo pojedyncze
  }
  gDown = down;

  // Baza dryfuje TYLKO gdy nikt nie dotyka — inaczej dluzsze przytrzymanie palca
  // "nauczyloby" ja wartosci dotknietej i dotyk przestalby byc wykrywany.
  if (!down) {
    gBase = static_cast<uint32_t>(gBase * (1.f - kDrift) + gRaw * kDrift);
  }
  return out;
}

bool pressedRaw() {
  // gDown jest aktualizowany na koncu poll() do stanu Z TEJ klatki, wiec zwracamy
  // surowy stan progu bez ponownego odczytu ADC. Gdy dotyk wylaczony (!gReady),
  // poll() nie rusza gDown i zostaje false — kropka feedbacku sie nie zapala.
  return gDown;
}

uint32_t raw() {
  return gRaw;
}

uint32_t baseline() {
  return gBase;
}

}  // namespace touch
