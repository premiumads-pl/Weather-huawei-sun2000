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

enum class Tap { NONE, SINGLE, DOUBLE };

// Wolane co klatke.
//   SINGLE — jedno dotkniecie: odliczanie biezacego ekranu od nowa
//   DOUBLE — dwa szybkie: cofamy sie o ekran
// UWAGA: pojedyncze dotkniecie jest zglaszane z OPOZNIENIEM kDoubleMs — inaczej
// nie dalo by sie odroznic go od pierwszej polowy podwojnego.
Tap poll();

// Surowy stan elektrody Z OSTATNIEGO poll(): true, gdy odczyt jest powyzej progu
// (palec na pinie) — BEZ czekania na okno rozroznienia 1x/2x, ktore opoznia SINGLE
// o ~550 ms. Do natychmiastowej kropki feedbacku V3: petla pyta co klatke i zapala
// kropke, zanim jeszcze wiadomo, czy to bylo pojedyncze czy podwojne. Nie robi
// wlasnego odczytu ADC — zwraca stan policzony w poll(), wiec MUSI byc wolane PO nim.
bool pressedRaw();

// Do diagnostyki: surowy odczyt i aktualna linia bazowa.
uint32_t raw();
uint32_t baseline();

}  // namespace touch
