#pragma once

#include <cstdint>
#include <climits>

// Historia temperatury zasilania kotla: 24 godziny, jedna probka co 10 minut.
//
// To jest OKNO RUCHOME, a nie doba kalendarzowa. Wzorzec skopiowany 1:1 z
// RoomHistory SWIADOMIE, zamiast wymyslania wlasnego: tamten ma juz obsluzone
// przypadki brzegowe, ktore tutaj sa identyczne (przerwa w zasilaniu, cofniety
// zegar, przerwa dluzsza niz cale okno).
//
// Slot = epoch / 600. Numer slotu jest absolutny, wiec po przerwie w zasilaniu
// wystarczy przewinac bufor o roznice slotow i wyczyscic to, co przespalismy.
//
// (v199) ZASTAPILA BurnerHistory. Tamta byla doba KALENDARZOWA, wiec o 00:05
// wykres byl pusty, i byla zafalszowana aliasingiem: piec odpytujemy co 3 minuty,
// sloty maja 10 minut, a cykl CWU bywa krotszy niz odstep miedzy odpytami (pelny
// wywod przy Model w Viessmann.h). Temperatura zasilania tego problemu nie ma w
// tym samym stopniu — opada powoli i miedzy dwoma odpytami nie znika.
struct BoilerHistory {
  static constexpr int SLOTS = 144;          // 24 h / 10 min
  static constexpr int16_t NO_T = INT16_MIN;

  int16_t t10[SLOTS];      // temperatura zasilania * 10
  uint32_t lastSlot = 0;   // numer slotu pod indeksem `head`
  int16_t head = 0;

  void reset() {
    for (int i = 0; i < SLOTS; ++i) t10[i] = NO_T;
    lastSlot = 0;
    head = 0;
  }

  // Przewija bufor do biezacego slotu, zerujac przespane. Zwraca false, gdy czas
  // jest jeszcze nieustawiony (NTP nie doszedl) — wtedy nic nie zapisujemy.
  bool advance(uint32_t epoch) {
    if (epoch < 1700000000UL) return false;
    const uint32_t slot = epoch / 600;

    if (lastSlot == 0) {   // pierwsze uruchomienie
      reset();
      lastSlot = slot;
      return true;
    }
    if (slot == lastSlot) return true;
    if (slot < lastSlot) return true;   // zegar cofniety — nie psujemy historii

    const uint32_t gap = slot - lastSlot;
    if (gap >= static_cast<uint32_t>(SLOTS)) {
      reset();   // przerwa dluzsza niz 24 h — cala historia i tak jest nieaktualna
      lastSlot = slot;
      return true;
    }
    for (uint32_t k = 0; k < gap; ++k) {
      head = static_cast<int16_t>((head + 1) % SLOTS);
      t10[head] = NO_T;
    }
    lastSlot = slot;
    return true;
  }

  // Wpisujemy TYLKO to, co model potwierdza flaga. Zero bez flagi bylby zapisem
  // TRWALYM, ktory potem klamie na wykresie — ta sama zasada, co przy profilu
  // palnika, ktory ta struktura zastapila.
  void push(bool hasT, float t) {
    if (!hasT) return;
    t10[head] = static_cast<int16_t>(t * 10.f + (t >= 0.f ? 0.5f : -0.5f));
  }

  // i = 0 to najstarsza probka, i = SLOTS-1 to biezaca
  int idx(int i) const { return (head + 1 + i) % SLOTS; }
};
