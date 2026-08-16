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
//   SINGLE — jedno dotkniecie: V3 nastepny ekran, V1/V2 odliczanie od nowa
//   DOUBLE — drugie dotkniecie w oknie kDoubleMs po pierwszym
// (v158) SINGLE leci NATYCHMIAST po zboczu, bez czekania na okno podwojnego —
// patrz dlugie uzasadnienie w Touch.cpp. DOUBLE przychodzi wiec ZAWSZE PO SINGLE
// dla tego samego gestu; odbiorca ma to obsluzyc jako "cofnij pojedyncze i zrob
// podwojne", a nie "wybierz jedno z dwoch".
Tap poll();

// Liczniki dla /api/diag (touch.*): ile zboczy elektrody policzylismy jako gest,
// ile z nich zamknelo sie w gest podwojny i ile odrzucil debounce. To ostatnie
// jest jedyna miara "stukniec, ktore nie przelaczyly ekranu" — do v157 nie bylo
// czego zmierzyc, bo w logu widac tylko udane "Dotyk V3: nastepny ekran".
uint32_t taps();
uint32_t doubles();
uint32_t bounced();
// Okno na drugie stukniecie i debounce, w ms. Wystawione, zeby /api/diag mogl podac
// liczby, wzgledem ktorych liczniki wyzej cokolwiek znacza — i zeby nie trzeba bylo
// ich powtarzac w Portal.cpp (jedno zrodlo: stale w Touch.cpp).
uint32_t doubleWindowMs();
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
