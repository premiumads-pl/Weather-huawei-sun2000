#pragma once

#include <cstdint>

// Wbudowana dioda RGB (WS2812) na ESP32-S3.
// Sygnalizacja bilansu z siecią — widoczna kątem oka, bez patrzenia na ekran:
//   ZIELONY   = oddajesz do sieci (nadwyżka z PV)
//   NIEBIESKI = równowaga (produkcja ≈ zużycie)
//   CZERWONY  = pobierasz z sieci
//   zgaszona  = brak danych z falownika

void ledBegin();

// Test kolejności kolorów przy starcie. Zwraca nazwę aktualnie świecącego koloru
// albo nullptr, gdy test się skończył.
const char* ledTestStep();

void ledShowGrid(int32_t gridW, bool pvOnline, bool night);
