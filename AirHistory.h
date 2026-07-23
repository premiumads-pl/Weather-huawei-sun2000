#pragma once

#include <cstdint>
#include <climits>
#include <cstdio>
#include <cmath>

// Historia jakosci powietrza: 7 DNI, jedna wartosc (srednia dobowa) na dzien.
//
// To jest OKNO RUCHOME dni kalendarzowych (slot = epoch / 86400), a nie 24-godzinne
// okno probek jak RoomHistory. Wzorzec ten sam: caly bufor leci do NVS (klucz "airh")
// razem z numerem ostatniego dnia, wiec po zaniku zasilania wiadomo, ktore dni sa
// jeszcze wazne, a ktore (przespane) trzeba wyzerowac. Numer dnia jest absolutny, wiec
// po przerwie wystarczy przewinac bufor o roznice dni i wyczyscic dziure.
//
// Zrodlo danych: AirModel z ekranu POWIETRZE (ARMAAG/sensorbox, GA17 z zapasem GA24).
// Kazda doba to SREDNIA z probek godzinowych (PM10, PM2,5) i MAKSIMUM indeksu jakosci
// tego dnia — najgorsza godzina ustala kolor dnia, tak samo jak `index` = max(PM10,PM2,5)
// w AirClient.cpp. Konsument: panel WWW (Portal.cpp) przez toJson() — patrz na dole.
struct AirHistory {
  static constexpr int DAYS = 7;
  static constexpr int16_t NO_V = INT16_MIN;   // brak wartosci PM danego dnia
  static constexpr uint8_t NO_IDX = 255;       // brak indeksu jakosci danego dnia

  // Sloty dobowe (i=0 najstarszy .. i=DAYS-1 = dzis liczy sie POPRZEZ head, patrz slotOf).
  int16_t pm25[DAYS];   // µg/m3 zaokraglone do int (NO_V = brak)
  int16_t pm10[DAYS];   // µg/m3 zaokraglone do int (NO_V = brak)
  // Indeks jakosci ARMAAG 1..6 (1 = BARDZO DOBRE, 6 = BARDZO ZLE) — TE SAME wartosci,
  // co AirModel.index / airCol() / airIndexName(), zeby panel mial jedno mapowanie
  // koloru dla ekranu i dla wykresu. NO_IDX (255) = brak dnia. (Brief mowil "0..5";
  // zostaje ARMAAG 1..6 dla spojnosci z reszta systemu — 0 i tak nie wystapi.)
  uint8_t idx[DAYS];
  uint8_t accIdxMax = 0;     // najgorszy indeks dzis (0 = jeszcze zaden); przy idx[] bez paddingu

  // KOLEJNOSC POL dobrana pod pakowanie (2-bajtowe, potem 4-bajtowe) — bufor leci do NVS
  // jeden do jednego, wiec kazdy bajt paddingu to zmarnowany bajt we flashu i w RAM.
  uint16_t accN = 0;         // liczba par probek dzis
  int16_t head = 0;          // indeks slotu biezacego dnia
  uint32_t lastDay = 0;      // numer dnia (epoch/86400) pod indeksem head; 0 = brak startu

  // Akumulatory BIEZACEGO dnia. accN (wyzej) jest WSPOLNY dla PM10 i PM2,5 — to bezpieczne,
  // bo push() dostaje obie wartosci naraz (wolajacy pilnuje hasPm25 && hasPm10), wiec
  // kazda zliczona probka niesie realny PM10 I PM2,5. Gdyby liczyc probki z brakiem
  // jednej wartosci, 0 zanizyloby te srednia — dlatego takich probek nie zliczamy.
  float accPm25 = 0.f;
  float accPm10 = 0.f;

  void resetAcc() {
    accPm25 = 0.f;
    accPm10 = 0.f;
    accN = 0;
    accIdxMax = 0;
  }

  void clearSlot(int i) {
    pm25[i] = NO_V;
    pm10[i] = NO_V;
    idx[i] = NO_IDX;
  }

  void reset() {
    for (int i = 0; i < DAYS; ++i) clearSlot(i);
    resetAcc();
    lastDay = 0;
    head = 0;
  }

  // PM nieujemne; INT16_MIN zarezerwowane na NO_V, wiec przycinamy do [0, 32767].
  static int16_t roundPm(float x) {
    if (x < 0.f) x = 0.f;
    long v = lroundf(x);
    if (v > 32767) v = 32767;
    return static_cast<int16_t>(v);
  }

  // Zamraza srednia biezacego dnia z akumulatorow do slotu head. Wolane przy KAZDYM
  // push (zeby "dzis" bylo widoczne na wykresie od razu, nie dopiero po polnocy) oraz
  // przez advance() tuz przed przewinieciem — to jest ta "finalizacja poprzedniego
  // dnia z akumulatorow". Dzien bez ani jednej pary probek zostaje NO_V (uczciwa dziura).
  void freezeCurrent() {
    if (accN == 0) return;
    pm25[head] = roundPm(accPm25 / accN);
    pm10[head] = roundPm(accPm10 / accN);
    idx[head] = (accIdxMax > 0) ? accIdxMax : NO_IDX;
  }

  // Przewija bufor do biezacego dnia, zerujac przespane. Zwraca false, gdy czas jest
  // jeszcze nieustawiony (NTP nie doszedl) — wtedy nic nie ruszamy. slot dnia = epoch/86400.
  bool advance(uint32_t epoch) {
    if (epoch < 1700000000UL) return false;
    const uint32_t day = epoch / 86400UL;

    if (lastDay == 0) {           // pierwsze uruchomienie
      reset();
      lastDay = day;
      return true;
    }
    if (day == lastDay) return true;
    if (day < lastDay) return true;   // zegar cofniety — nie psujemy historii

    freezeCurrent();              // zamroz WYCHODZACY dzien z akumulatorow

    uint32_t gap = day - lastDay;
    if (gap >= static_cast<uint32_t>(DAYS)) {
      reset();                    // przerwa dluzsza niz tydzien — cala historia nieaktualna
      lastDay = day;
      return true;
    }
    for (uint32_t i = 0; i < gap; ++i) {
      head = static_cast<int16_t>((head + 1) % DAYS);
      clearSlot(head);            // przespany / nowy dzien = dziura (NO_V/NO_IDX)
    }
    resetAcc();                   // nowy biezacy dzien liczymy od zera
    lastDay = day;
    return true;
  }

  // Dodaje probke do sredniej DZIS (srednia PM, maksimum indeksu). Wolac po advance().
  void push(float pm25v, float pm10v, int index) {
    if (lastDay == 0) return;     // advance() nie ustawil jeszcze dnia (brak NTP)
    accPm25 += pm25v;
    accPm10 += pm10v;
    ++accN;
    if (index > 0 && static_cast<uint8_t>(index) > accIdxMax) {
      accIdxMax = static_cast<uint8_t>(index);
    }
    freezeCurrent();              // "dzis" natychmiast widoczne w slocie head
  }

  // i = 0 to najstarszy z 7 dni, i = DAYS-1 to DZIS.
  int slotOf(int i) const { return (head + 1 + i) % DAYS; }
  int16_t pm25At(int i) const { return (i < 0 || i >= DAYS) ? NO_V : pm25[slotOf(i)]; }
  int16_t pm10At(int i) const { return (i < 0 || i >= DAYS) ? NO_V : pm10[slotOf(i)]; }
  int idxAt(int i) const { return (i < 0 || i >= DAYS) ? NO_IDX : idx[slotOf(i)]; }

  // Serializacja dla panelu WWW (Portal.cpp / /api/diag). Ksztalt STABILNY:
  //   {"pm25":[..7 od najstarszego do dzis..],"pm10":[..7..],"idx":[..7..]}
  // brak dnia -> null. Bufor min. ~200 B (256 bezpiecznie). Bezpieczne na obciecie:
  // przy zbyt malym n dopisywanie sie zatrzymuje, a string zostaje zakonczony '\0'.
  void toJson(char* buf, size_t n) const {
    if (buf == nullptr || n == 0) return;
    size_t o = 0;
    jputs(buf, n, o, "{\"pm25\":[");
    for (int i = 0; i < DAYS; ++i) {
      if (i) jputs(buf, n, o, ",");
      const int16_t v = pm25At(i);
      if (v == NO_V) jputs(buf, n, o, "null"); else jputi(buf, n, o, v);
    }
    jputs(buf, n, o, "],\"pm10\":[");
    for (int i = 0; i < DAYS; ++i) {
      if (i) jputs(buf, n, o, ",");
      const int16_t v = pm10At(i);
      if (v == NO_V) jputs(buf, n, o, "null"); else jputi(buf, n, o, v);
    }
    jputs(buf, n, o, "],\"idx\":[");
    for (int i = 0; i < DAYS; ++i) {
      if (i) jputs(buf, n, o, ",");
      const int v = idxAt(i);
      if (v == NO_IDX) jputs(buf, n, o, "null"); else jputi(buf, n, o, v);
    }
    jputs(buf, n, o, "]}");
  }

 private:
  // Dopisywanie z pilnowaniem konca bufora (kumuluje offset). Po obcieciu o >= n i
  // kolejne wywolania nic nie robia; snprintf zawsze zostawia '\0' w zakresie n-o.
  static void jputs(char* buf, size_t n, size_t& o, const char* s) {
    if (o >= n) return;
    int w = snprintf(buf + o, n - o, "%s", s);
    if (w > 0) o += static_cast<size_t>(w);
  }
  static void jputi(char* buf, size_t n, size_t& o, long v) {
    if (o >= n) return;
    int w = snprintf(buf + o, n - o, "%ld", v);
    if (w > 0) o += static_cast<size_t>(w);
  }
};

// Definicja globalu zyje w pogoda-gdynia.ino (obok RoomHistory gRooms{}). Deklaracja
// tutaj, zeby panel (Portal.cpp) potrzebowal tylko #include "AirHistory.h" i mial od razu
// typ + dostep do gAirHistory.toJson(...). Zapis/odczyt NVS: air*History w Settings.cpp.
extern AirHistory gAirHistory;
