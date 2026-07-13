#pragma once

#include <cstdint>
#include <climits>

// Historia z czujnikow BLE: 24 godziny, jedna probka co 10 minut.
//
// To jest OKNO RUCHOME, a nie doba kalendarzowa — nie kasuje sie o polnocy
// (inaczej o 00:05 wykres bylby pusty). Przezywa tez zanik zasilania: caly bufor
// leci do NVS co 10 minut, razem z numerem ostatniego slotu, wiec po restarcie
// wiadomo, ktore probki sa jeszcze wazne, a ktore trzeba wyzerowac.
//
// Slot = epoch / 600. Numer slotu jest absolutny, wiec po przerwie w zasilaniu
// wystarczy przewinac bufor o roznice slotow i wyczyscic to, co przespalismy.
struct RoomHistory {
  static constexpr int SLOTS = 144;  // 24 h / 10 min
  static constexpr int ROOMS = 4;
  static constexpr int16_t NO_T = INT16_MIN;
  static constexpr uint8_t NO_H = 255;

  int16_t t10[ROOMS][SLOTS];  // temperatura * 10
  uint8_t hum[ROOMS][SLOTS];  // wilgotnosc w %
  uint32_t lastSlot = 0;      // numer slotu pod indeksem `head`
  int16_t head = 0;

  void reset() {
    for (int r = 0; r < ROOMS; ++r) {
      for (int i = 0; i < SLOTS; ++i) {
        t10[r][i] = NO_T;
        hum[r][i] = NO_H;
      }
    }
    lastSlot = 0;
    head = 0;
  }

  void clearSlot(int idx) {
    for (int r = 0; r < ROOMS; ++r) {
      t10[r][idx] = NO_T;
      hum[r][idx] = NO_H;
    }
  }

  // Przewija bufor do biezacego slotu, zerujac przespane. Zwraca false, gdy czas
  // jest jeszcze nieustawiony (NTP nie doszedl) — wtedy nic nie zapisujemy.
  bool advance(uint32_t epoch) {
    if (epoch < 1700000000UL) return false;
    const uint32_t slot = epoch / 600;

    if (lastSlot == 0) {  // pierwsze uruchomienie
      reset();
      lastSlot = slot;
      return true;
    }
    if (slot == lastSlot) return true;
    if (slot < lastSlot) return true;  // zegar cofniety — nie psujemy historii

    uint32_t gap = slot - lastSlot;
    if (gap >= static_cast<uint32_t>(SLOTS)) {
      reset();  // przerwa dluzsza niz 24 h — cala historia i tak jest juz nieaktualna
      lastSlot = slot;
      return true;
    }
    for (uint32_t i = 0; i < gap; ++i) {
      head = static_cast<int16_t>((head + 1) % SLOTS);
      clearSlot(head);  // czas, w ktorym urzadzenie nie dzialalo — dziura, nie zero
    }
    lastSlot = slot;
    return true;
  }

  void push(int room, bool hasT, float t, bool hasH, float h) {
    if (room < 0 || room >= ROOMS) return;
    if (hasT) t10[room][head] = static_cast<int16_t>(t * 10.f);
    if (hasH) hum[room][head] = static_cast<uint8_t>(h < 0 ? 0 : (h > 100 ? 100 : h));
  }

  // i = 0 to najstarsza probka, i = SLOTS-1 to biezaca
  int idx(int i) const { return (head + 1 + i) % SLOTS; }
};
