#pragma once

#include <cstdint>

// Bramka BLE: druga para uszu.
//
// PO CO: Bluetooth nie ma sieci kratowej — kazdy czujnik musi dosiegnac jednego,
// konkretnego odbiornika. Zmierzone u uzytkownika: czujnik na schodach dochodzi do
// wyswietlacza z -86 dBm (na granicy), a do Shelly stojacego obok z -57 dBm.
// Zadne przekladanie plytki tego nie naprawi — to geometria mieszkania.
//
// PODZIAL PRACY: Shelly przekazuje SUROWY szyfrogram MiBeacon, bindkeys zostaja
// w NVS wyswietlacza. Gdyby ktos zajrzal w skrypt bramki, nie dowie sie nic.
//
// Wyswietlacz NIE przestaje sluchac sam — bierze to, co przyjdzie. Padnie bramka,
// blizsze czujniki dzialaja dalej.

namespace blegw {

void poll();              // wolane z netTask
uint32_t lastOkAt();
int lastCount();
const char* lastError();

}  // namespace blegw
