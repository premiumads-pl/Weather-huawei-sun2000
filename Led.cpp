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

// --- DIAGNOSTYKA (na czas pomiaru PIR) ---------------------------------------
// Blysk przy kazdym wyzwoleniu czujnika, zeby wlasciciel widzial PIR na zywo,
// zamiast odpytywac /api/diag. Do wyrzucenia razem z pomiarem.
//
// BIALY, bo semantyka energii zajmuje kanaly pojedynczo: G=oddaje, R=pobieram,
// B=rownowaga. Fiolet to R+B — kątem oka czyta sie jako "cos miedzy pobieraniem
// a rownowaga" i klamie. Bialy (R+G+B) jest jedynym kolorem, ktorego nie da sie
// pomylic z zadnym stanem czastkowym, i ma najwiekszy kontrast wzgledem diody
// zgaszonej — czyli wobec przypadku, w ktorym wlasciciel bedzie testowal
// (ciemna lazienka, falownik offline).
constexpr uint32_t kFlashMs = 120;
constexpr uint8_t kFlashLvl = 120;

// 0 = nie ma blysku. Sentinel, a nie samo porownanie z millis(): po ~24,8 dnia
// uptime'u millis() przekracza 2^31 i "millis() - 0" jako int32 robi sie ujemne,
// czyli warunek trwalby wiecznie. Urzadzenie chodzi dluzej niz 24 dni.
uint32_t gFlashUntil = 0;

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
  const uint32_t step = cfg::LED_TEST_MS;
  if (t < step) {
    setRgb(160, 0, 0);
    return "CZERWONY";
  }
  if (t < step * 2) {
    setRgb(0, 160, 0);
    return "ZIELONY";
  }
  if (t < step * 3) {
    setRgb(0, 0, 160);
    return "NIEBIESKI";
  }
  setRgb(0, 0, 0);
  gTestDone = true;
  return nullptr;
}

// Wolane z loop(), NIE z pirIsr(): rgbLedWrite() to RMT i nie ma prawa byc w
// przerwaniu. Sama funkcja diody nie rusza — stawia tylko znacznik czasu, a caly
// zapis zostaje w ledShowGrid(). Dzieki temu blysk nie moze zostawic diody w
// zlym stanie: jedynym pisarzem po tescie nadal jest ledShowGrid().
void ledPirFlash() {
  gFlashUntil = millis() + kFlashMs;
  if (gFlashUntil == 0) gFlashUntil = 1;   // 0 jest zarezerwowane na "brak blysku"
}

void ledShowGrid(int32_t gridW, bool pvOnline, bool night) {
  if (!gTestDone) {
    return;   // test ma pierwszeństwo
  }

  // Blysk PIR ma pierwszenstwo nad bilansem, ale nie nad testem. Idzie przez
  // setRgb(), a nie wprost przez rgbLedWrite() — i to jest cala odpowiedz na
  // gLastR/G/B: cache nie trzeba obchodzic, trzeba go nie oklamac. setRgb()
  // aktualizuje gLast na biel, wiec powrot do koloru energii (albo do zera przy
  // falowniku offline) jest juz zmiana i zostanie zapisany. Zapis wprost do
  // rgbLedWrite() zostawilby gLast na starym kolorze i powrot bylby pominiety —
  // dioda utknelaby na bialo.
  if (gFlashUntil != 0) {
    if (static_cast<int32_t>(millis() - gFlashUntil) < 0) {
      setRgb(kFlashLvl, kFlashLvl, kFlashLvl);
      return;
    }
    gFlashUntil = 0;   // blysk sie skonczyl — nizej normalny kolor go nadpisze
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
