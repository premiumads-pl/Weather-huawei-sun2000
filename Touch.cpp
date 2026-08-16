#include "Touch.h"

#include "Log.h"

#include <Arduino.h>

namespace touch {
namespace {

constexpr uint8_t kPin = 7;          // GPIO7 = TOUCH7 (wolny: TFT ma 8-12 i 14)
constexpr float kRise = 1.25f;       // dotkniecie = odczyt o 25% powyzej bazy

// (v158) DWIE LICZBY, KTORE ZJADALY STUKNIECIA. Wlasciciel: "pojedyncze stukniecia
// nie zawsze przelaczaja ekran". Zmierzone w kodzie v157, nie zgadniete:
//
//   kHoldOffMs = 250, kDoubleMs = 550  =>  drugie stukniecie liczylo sie WYLACZNIE
//   w oknie 250-550 ms od pierwszego, czyli w szczelinie szerokiej 300 ms.
//
// Typowe ludzkie podwojne stukniecie to 150-400 ms (domyslny prog dwukliku w macOS
// i Windows to 500 ms). Wszystko ponizej 250 ms wpadalo w debounce i bylo kasowane
// BEZ SLADU — a poniewaz kasowanie odbieralo tylko DRUGIE zbocze, gest konczyl sie
// jako pojedyncze stukniecie. Czyli: szybki dwuklik dawal "nastepny ekran" zamiast
// diagnostyki. Nowe 120 ms jest wciaz grubo powyzej drgan komparatora progu
// (touchRead na S3 jest wygladzony sprzetowo; sasiednie odczyty roznia sie o
// promile bazy), a wpuszcza caly zakres ludzki od 120 ms w gore.
//
// Okno na drugie stukniecie: 550 -> 600 ms. Powod NIE jest kosmetyczny — patrz
// akapit o natychmiastowym SINGLE w poll(). Dopoki SINGLE czekalo na wygasniecie
// okna, kazde poszerzenie okna wydluzalo REAKCJE na zwykle stukniecie; teraz okno
// nie kosztuje juz nic i moze byc szczodre. 600 ms = 500 ms systemowego progu
// dwukliku + 100 ms zapasu na to, ze poll() widzi zbocze dopiero w kolejnej klatce
// (FRAME_IDLE_MS = 50 ms na statycznym ekranie).
constexpr uint32_t kHoldOffMs = 120;  // debounce — jedno dotkniecie to jedno zdarzenie
constexpr uint32_t kDoubleMs = 600;   // okno na drugie dotkniecie (palec, nie mysz)
constexpr float kDrift = 0.002f;     // jak szybko baza goni otoczenie (tylko gdy nie dotykamy)

uint32_t gBase = 0;
uint32_t gRaw = 0;
bool gDown = false;
uint32_t gLastEvent = 0;
bool gReady = false;

// Liczniki do /api/diag — jedyne zrodlo wiedzy o stuknieciach ODRZUCONYCH.
uint32_t gTaps = 0;      // zbocza policzone jako gest (kazde daje SINGLE)
uint32_t gDoubles = 0;   // z tego: zamkniete jako gest podwojny
uint32_t gBounced = 0;   // zbocza odrzucone przez debounce (kHoldOffMs)

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

  static bool pending = false;      // pierwsze stukniecie moze jeszcze dostac pare
  static uint32_t pendingAt = 0;

  Tap out = Tap::NONE;

  // (v158) SINGLE LECI NATYCHMIAST PO ZBOCZU. Do v157 pierwsze stukniecie NIE robilo
  // nic az do wygasniecia okna (kDoubleMs), bo poll() zwracalo SINGLE dopiero w
  // galezi "drugie nie przyszlo". Zmierzone opoznienie reakcji na zwykle stukniecie
  // wynosilo wiec 550 ms + do jednej klatki (FRAME_IDLE_MS = 50 ms) ~ 0,6 s — i to
  // JEST przyczyna zgloszenia "stukniecie nie zawsze przelacza ekran": ekran
  // przelaczal sie zawsze, tylko po czasie dluzszym niz cierpliwosc reki, ktora
  // w tym czasie stukala drugi raz (i trafiala albo w debounce, albo w DOUBLE).
  // To ZUPELNIE INNY problem niz zbyt waskie okno i wymaga innej decyzji, wiec
  // podjeta jest wprost: reagujemy od razu, a gest podwojny COFA skutek gestu
  // pojedynczego. Cofniecie jest darmowe, bo touchDoubleV3() ustawia widok
  // BEZWZGLEDNIE (VIEW_STATS albo VIEW_NOW), nie wzglednie — wiec nieistotne jest,
  // na ktory ekran przeskoczylo wczesniej pojedyncze stukniecie. Alternatywa
  // (czekanie na okno) juz byla i to jej wlasnie dotyczy zgloszenie.
  if (down && !gDown) {
    if (now - gLastEvent > kHoldOffMs) {
      gLastEvent = now;
      ++gTaps;
      if (pending && now - pendingAt <= kDoubleMs) {
        pending = false;
        ++gDoubles;
        out = Tap::DOUBLE;
      } else {
        pending = true;
        pendingAt = now;
        out = Tap::SINGLE;
      }
    } else {
      // Zbocze zjedzone przez debounce. Do v157 nie zostawialo po sobie NICZEGO —
      // ani w logu, ani w /api/diag — wiec "stukniecie nie zadzialalo" bylo
      // niemierzalne. Teraz jest liczba: touch.bounced w /api/diag.
      ++gBounced;
    }
  } else if (pending && now - pendingAt > kDoubleMs) {
    pending = false;   // okno minelo, gest zamkniety jako pojedynczy (juz zgloszony)
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

uint32_t taps() { return gTaps; }
uint32_t doubles() { return gDoubles; }
uint32_t bounced() { return gBounced; }
uint32_t doubleWindowMs() { return kDoubleMs; }
uint32_t holdOffMs() { return kHoldOffMs; }

uint32_t raw() {
  return gRaw;
}

uint32_t baseline() {
  return gBase;
}

}  // namespace touch
