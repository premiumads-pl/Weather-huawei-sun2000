#pragma once

#include <cstdint>

// Dotyk pojemnosciowy na GPIO7 (kanal TOUCH7). Plytka nie ma zadnego przycisku,
// wiec wystarczy przylozyc palec do pinu albo doprowadzic z niego kawalek folii
// czy sruby — pojemnosc ciala wystarczy.
//
// UWAGA: w ESP32-S3 (inaczej niz w pierwszym ESP32) odczyt przy dotknieciu
// ROSNIE, a nie maleje. Progu nie da sie zaszyc na sztywno — zalezy od dlugosci
// przewodu i wilgotnosci — wiec liczymy go od linii bazowej, ktora dodatkowo
// powoli dryfuje razem z otoczeniem.

namespace touch {

void begin();

// (v185) DWA STANY, NIE TRZY. Gest podwojny zniknal w calosci — patrz uzasadnienie
// w Touch.cpp. Kazde przyjete zbocze jest zwyklym stuknieciem, niezaleznie od tego,
// jak szybko po poprzednim padlo.
enum class Tap { NONE, SINGLE };

// Wolane co klatke.
//   SINGLE — jedno dotkniecie: nastepny ekran w petli V3.
// SINGLE leci NATYCHMIAST po zboczu: poll() nie trzyma zadnego stanu miedzy klatkami
// i na nic nie czeka, wiec reakcja ekranu jest w tej samej klatce co dotkniecie.
Tap poll();

// Liczniki dla /api/diag (touch.*): ile zboczy elektrody przyjelismy jako stukniecie
// i ile odrzucil debounce. To drugie jest jedyna miara "stukniec, ktore nie przelaczyly
// ekranu" — do v157 nie bylo czego zmierzyc, bo w logu widac tylko udane
// "Dotyk V3: nastepny ekran".
uint32_t taps();
uint32_t bounced();
// Debounce w ms. Wystawiony, zeby /api/diag mogl podac liczbe, wzgledem ktorej `bounced`
// cokolwiek znaczy — i zeby nie trzeba bylo jej powtarzac w Portal.cpp (jedno zrodlo:
// stala w Touch.cpp).
uint32_t holdOffMs();

// Surowy stan elektrody Z OSTATNIEGO poll(): true, gdy odczyt jest powyzej progu
// (palec na pinie). Do kropki feedbacku V3 — zapala sie takze przy PRZYTRZYMANIU
// palca, kiedy zadne zbocze juz nie leci, wiec nadal ma sens mimo natychmiastowego
// SINGLE (kropka mowi "czuje palec", zdarzenie mowi "policzylem gest"). Nie robi
// wlasnego odczytu ADC — zwraca stan policzony w poll(), wiec MUSI byc wolane PO nim.
bool pressedRaw();

// Do diagnostyki: surowy odczyt i aktualna linia bazowa.
uint32_t raw();
uint32_t baseline();

}  // namespace touch
