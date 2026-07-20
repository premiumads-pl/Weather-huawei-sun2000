#pragma once

#include <cstdint>

// Jakosc powietrza — miejska siec czujnikow Gdyni (ARMAAG/sensorbox.pl).
//
// GA17 (Sandomierska 3, Maly Kack) to stacja GLOWNA — tam mieszka wlasciciel i o TA
// stacje mu chodzi. GA24 (Halicka 8) to AUTOMATYCZNY ZAPAS, wlaczany WYLACZNIE gdy
// GA17 nie ma juz nic swiezego do pokazania. Pelne uzasadnienie fallbacku, indeksu
// ARMAAG (i dlaczego to MAKSIMUM z PM10/PM2.5, nie srednia) oraz filtra JSON —
// patrz komentarze w AirClient.cpp.

// Powyzej tego wieku probki PM z GA17 uznajemy stacje glowna za "milczaca" i
// przelaczamy sie na GA24. ~3 h, bo dane to srednie GODZINOWE (nowa probka raz na
// godzine) — dwie-trzy nieudane godziny z rzedu to juz realny problem stacji, nie
// zwykly poslizg publikacji.
constexpr uint32_t AIR_STALE_S = 3UL * 3600UL;

struct AirModel {
  // false, dopoki ZADNA z dwoch stacji nie dala nic uzytecznego (ani jednej swiezej
  // probki PM10 lub PM2.5). Ekran POWIETRZE i rotacja widokow maja to traktowac
  // dokladnie tak, jak RADAR bez opadu — pomijac, zamiast pokazywac pustke.
  bool ready = false;
  char errorMsg[48] = {};

  // KLUCZOWE dla wlasciciela: musi widziec, z ktorej stacji patrzy na dane, inaczej
  // pomylilby Halicka z Sandomierska (ta sama zasada, co litera E/S przy czujnikach
  // BLE — kto odebral ramke). stationName to gotowy do wyswietlenia napis.
  bool usingFallback = false;   // true = na ekranie GA24 (Halicka), nie GA17
  char stationName[16] = {};    // "SANDOMIERSKA" albo "HALICKA"

  bool hasPm10 = false;
  float pm10 = 0.f;    // µg/m3
  bool hasPm25 = false;
  float pm25 = 0.f;    // µg/m3
  // Epoch (UTC) najswiezszej z wykorzystanych probek PM10/PM2.5 — z NIEGO liczymy
  // "X min temu" na ekranie. 0 = nie znamy (nie powinno sie zdarzyc, gdy ready==true,
  // bo ready wymaga choc jednej probki PM, a kazda probka niesie wlasny czas).
  uint32_t sampleEpoch = 0;

  // Temperatura/wilgotnosc/cisnienie — WYLACZNIE z GA17 (GA24 w ogole ich nie
  // odpytujemy, patrz lista `vars` w AirClient.cpp). Dlatego hasWeather jest zawsze
  // false przy usingFallback == true: pokazanie ich wtedy sugerowaloby pomiar
  // spod Sandomierskiej, podczas gdy caly ekran mowi "Halicka" — dokladnie to
  // klamstwo, przed ktorym ma bronic widoczna nazwa stacji.
  bool hasWeather = false;
  float tempC = 0.f;
  float rh = 0.f;
  float pressureHpa = 0.f;

  // Indeks ARMAAG 1..6 (1 = BARDZO DOBRE, 6 = BARDZO ZLE), policzony LOKALNIE wg
  // tabeli progow w AirClient.cpp — API tego nie liczy i nie trzeba tego sciagac.
  // indexPm10/indexPm25 to CZASTKOWE indeksy (0 = brak danej wartosci), `index` to
  // OGOLNY = WIEKSZY z dwóch, NIGDY srednia (pelne uzasadnienie w AirClient.cpp).
  // Trzymamy WSZYSTKIE TRZY (nie tylko `index`), zeby ekran mogl pokazac kolor
  // KAZDEJ karty (PM10/PM2,5) wg jej WLASNEJ klasy — inaczej nie byloby widac, ktory
  // z dwoch skladnikow naprawde ustala ten gorszy, ogolny wynik.
  int indexPm10 = 0;
  int indexPm25 = 0;
  int index = 0;
};
