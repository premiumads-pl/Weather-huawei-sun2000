#pragma once

#include <cstdint>

// Weryfikacja pieca licznikiem gazu.
//
// DLACZEGO WLASNY LOG, a nie licznik pieca:
// API daje currentDay / lastSevenDays / currentMonth / currentYear. Sprawdzone
// 15.07.2026: currentDay=0.8, lastSevenDays=5.8, ale currentMonth=5.3 (MNIEJ niz
// ostatnie 7 dni!) i currentYear=5.3 przy 4-letniej instalacji. Liczniki miesieczne
// i roczne sa po prostu zepsute. Wiarygodne jest tylko currentDay — wiec zbieramy
// je sami, dzien po dniu, i sumujemy miedzy odczytami licznika.
//
// currentDay to biezaca suma doby, rosnaca do polnocy. Probkujemy ja przy kazdym
// odpycie (co 3 min) i trzymamy NAJWYZSZA wartosc danego dnia — to daje dobowa sume
// nawet, gdy urzadzenie chwilowo nie dziala.

struct GasHistory {
  static constexpr int DAYS = 120;

  uint16_t m3x100[DAYS] = {};  // 0.01 m3 na jednostke — do 655 m3/dobe
  uint32_t lastDay = 0;        // epoch/86400 slotu pod indeksem head
  int16_t head = 0;

  void reset() {
    for (int i = 0; i < DAYS; ++i) m3x100[i] = 0;
    lastDay = 0;
    head = 0;
  }

  // Przewija do biezacej doby, zerujac przespane dni.
  bool advance(uint32_t epoch) {
    if (epoch < 1700000000UL) return false;
    const uint32_t day = epoch / 86400;
    if (lastDay == 0) {
      reset();
      lastDay = day;
      return true;
    }
    if (day == lastDay) return true;
    if (day < lastDay) return true;              // zegar sie cofnal — nie psujemy

    uint32_t gap = day - lastDay;
    if (gap >= static_cast<uint32_t>(DAYS)) {
      reset();
      lastDay = day;
      return true;
    }
    for (uint32_t i = 0; i < gap; ++i) {
      head = static_cast<int16_t>((head + 1) % DAYS);
      m3x100[head] = 0;
    }
    lastDay = day;
    return true;
  }

  // currentDay rosnie w ciagu doby — bierzemy maksimum, nie ostatnia probke.
  void push(float m3) {
    const uint16_t v = static_cast<uint16_t>(m3 * 100.f + 0.5f);
    if (v > m3x100[head]) m3x100[head] = v;
  }

  // Suma zuzycia w dniach [fromDay, toDay) — oba jako epoch/86400.
  // Zwraca -1, gdy okres wykracza poza to, co mamy zapisane.
  float sumBetween(uint32_t fromDay, uint32_t toDay) const {
    if (lastDay == 0 || toDay <= fromDay) return -1.f;
    if (fromDay + DAYS <= lastDay) return -1.f;   // za stare
    if (toDay > lastDay + 1) return -1.f;         // przyszlosc

    float sum = 0.f;
    for (uint32_t d = fromDay; d < toDay; ++d) {
      if (d > lastDay) break;
      const int back = static_cast<int>(lastDay - d);
      if (back >= DAYS) return -1.f;
      const int idx = ((head - back) % DAYS + DAYS) % DAYS;
      sum += m3x100[idx] / 100.f;
    }
    return sum;
  }
};

// Odczyt licznika wpisany recznie przez uzytkownika.
struct MeterRead {
  uint32_t day = 0;    // epoch/86400
  float m3 = 0.f;      // stan licznika, z czescia dziesietna
};


// Profil doby palnika — dokladnie jak PvHistory, tylko zamiast watow modulacja.
//
// DLACZEGO MODULACJA, A NIE GAZ:
// Piec nie oddaje chwilowego przeplywu gazu. Ma currentDay z rozdzielczoscia
// 0.1 m3 — przy odpycie co 3 min przyrost to ~0.002 m3, piecdziesiat razy ponizej
// rozdzielczosci. Wykres z tego bylby schodkami z szumu. Modulacja palnika (0-100%)
// to za to jego realna moc chwilowa — odpowiednik kW z fotowoltaiki. Pole pod
// wykresem = zuzyty gaz.
struct BurnerHistory {
  static constexpr int SLOTS = 144;   // doba co 10 minut
  uint8_t mod[SLOTS] = {};            // 0..100 %
  bool filled[SLOTS] = {};
  // tm_yday; -1 = nie bylo jeszcze ZADNEGO odczytu pieca (profil pusty, nie ma czego
  // kasowac ani zapisywac).
  //
  // Dobe kasuja DWA miejsca i to jest swiadome. push() ponizej — gdy piec odpowiada.
  // I netTask (pogoda-gdynia.ino, "polnoc: profil doby palnika") — co 250 ms, takze
  // gdy piec milczy. To drugie jest tym istotnym: profil przezywa restart w NVS, wiec
  // bez niego wczorajsze slupki wisza pod naglowkiem "PRACA PALNIKA DZIŚ" tak dlugo,
  // jak dlugo nie przyjdzie udany odpyt — a przy wygaslym tokenie to godziny.
  int day = -1;

  void reset(int yday) {
    for (int i = 0; i < SLOTS; ++i) { mod[i] = 0; filled[i] = false; }
    day = yday;
  }

  void push(int yday, int hour, int minute, int modPct, bool active) {
    if (yday != day) reset(yday);
    const int slot = (hour * 60 + minute) / 10;
    if (slot < 0 || slot >= SLOTS) return;
    // W slocie moze byc kilka probek — bierzemy najwyzsza, zeby krotki zaplon
    // nie zniknal miedzy odpytami.
    const uint8_t v = active ? static_cast<uint8_t>(modPct < 0 ? 0 : (modPct > 100 ? 100 : modPct))
                             : 0;
    if (!filled[slot] || v > mod[slot]) mod[slot] = v;
    filled[slot] = true;
  }

  uint8_t peak() const {
    uint8_t m = 0;
    for (int i = 0; i < SLOTS; ++i) if (filled[i] && mod[i] > m) m = mod[i];
    return m;
  }
};
