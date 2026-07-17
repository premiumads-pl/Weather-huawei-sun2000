#pragma once

#include <cstdint>

// Wbudowana dioda RGB (WS2812) na ESP32-S3.
// Sygnalizacja bilansu z siecią — widoczna kątem oka, bez patrzenia na ekran:
//   ZIELONY   = oddajesz do sieci (nadwyżka z PV)
//   NIEBIESKI = równowaga (produkcja ≈ zużycie)
//   CZERWONY  = pobierasz z sieci
//   zgaszona  = brak danych z falownika

void ledBegin();

// Autotest kolejności kolorów przy starcie: R -> G -> B po 500 ms, potem gaśnie.
// NIEBLOKUJĄCY i niewidoczny dla ekranu — wołać z loop() w każdej iteracji, bez
// wstrzymywania rysowania. Sam pilnuje, kiedy się skończył; po teście ledShowGrid()
// przejmuje diodę.
void ledTestStep();

void ledShowGrid(int32_t gridW, bool pvOnline, bool night);

// DIAGNOSTYKA na czas pomiaru PIR: krotki bialy blysk (~120 ms), potem powrot do
// koloru energii. Dziala takze przy diodzie zgaszonej (falownik offline).
// Wolac z loop() przy zmianie diag().pirLastAt — NIGDY z ISR: samego zapisu do
// diody ta funkcja nie robi, ale kontrakt "tylko z petli" trzyma cala reszte.
// Do usuniecia razem z pomiarem szerokosci/przerw impulsow PIR.
void ledPirFlash();
