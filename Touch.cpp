#include "Touch.h"

#include "Log.h"

#include <Arduino.h>

namespace touch {
namespace {

constexpr uint8_t kPin = 7;          // GPIO7 = TOUCH7 (wolny: TFT ma 8-12 i 14)
constexpr float kRise = 1.25f;       // dotkniecie = odczyt o 25% powyzej bazy

// (v185) GESTU PODWOJNEGO TU JUZ NIE MA. Zgloszenie wlasciciela brzmialo: "naciskam,
// zeby przelaczac ekrany, i nie zawsze pojawia sie kolejny, czasami wraca do glownego".
// Przyczyna siedziala dokladnie w tym pliku: przy debounce 120 ms i oknie 600 ms dwa
// stukniecia w odstepie 120-600 ms skladaly sie w gest DOUBLE, a odbiorca (touchDoubleV3)
// ustawial widok BEZWZGLEDNIE — na STATS albo na GLOWNY — czyli COFAL skutek pojedynczego
// stukniecia. Przy stukaniu co ~0,3 s robilo to CO DRUGIE stukniecie i wygladalo jak
// "ekran sie zacial". Sciezka pojedyncza byla przy tym sprawna (zmierzone: 10 stuknien
// x 3 rozne odstepy, za kazdym razem dokladnie jeden ekran do przodu), wiec skasowany
// zostal GEST, a nie debounce.
//
// kHoldOffMs ZOSTAJE 120 ms. On chroni przed DRGANIEM STYKU, a nie przed uzytkownikiem:
// jest grubo powyzej drgan komparatora progu (touchRead na S3 jest wygladzony sprzetowo,
// sasiednie odczyty roznia sie o promile bazy), a nie zjada zadnego ludzkiego tempa —
// szybciej niz ~8 razy na sekunde palcem po elektrodzie sie nie stuka.
constexpr uint32_t kHoldOffMs = 120;  // debounce — jedno dotkniecie to jedno zdarzenie
constexpr float kDrift = 0.002f;     // jak szybko baza goni otoczenie (tylko gdy nie dotykamy)

uint32_t gBase = 0;
uint32_t gRaw = 0;
bool gDown = false;
uint32_t gLastEvent = 0;
bool gReady = false;

// Liczniki do /api/diag — jedyne zrodlo wiedzy o stuknieciach ODRZUCONYCH.
uint32_t gTaps = 0;      // zbocza przyjete jako stukniecie (kazde daje SINGLE)
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

  Tap out = Tap::NONE;

  // (v185) KAZDE PRZYJETE ZBOCZE DAJE SINGLE — nic tu juz nie czeka na "pare", nie ma
  // stanu miedzy klatkami i nie ma czego przeterminowywac. SINGLE leci natychmiast po
  // zboczu, wiec ekran przelacza sie w tej samej klatce, a RYTM STUKANIA NIE MA
  // ZNACZENIA: dziesiec stuknien to dziesiec ekranow do przodu, czy padna co 0,2 s,
  // czy co 2 s. To jest cala poprawka zgloszenia "czasami wraca do glownego" —
  // przedtem drugie zbocze w oknie 600 ms zamienialo sie w gest cofajacy pierwszy.
  if (down && !gDown) {
    if (now - gLastEvent > kHoldOffMs) {
      gLastEvent = now;
      ++gTaps;
      out = Tap::SINGLE;
    } else {
      // Zbocze zjedzone przez debounce. Do v157 nie zostawialo po sobie NICZEGO —
      // ani w logu, ani w /api/diag — wiec "stukniecie nie zadzialalo" bylo
      // niemierzalne. Teraz jest liczba: touch.bounced w /api/diag.
      ++gBounced;
    }
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
uint32_t bounced() { return gBounced; }
uint32_t holdOffMs() { return kHoldOffMs; }

uint32_t raw() {
  return gRaw;
}

uint32_t baseline() {
  return gBase;
}

}  // namespace touch
