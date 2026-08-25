#pragma once

#include <cstdio>
#include <cstddef>

// Liczby dla CZLOWIEKA — polski przecinek dziesietny.
//
// (v175) TA FUNKCJA MIESZKALA W ANONIMOWEJ PRZESTRZENI NAZW WeatherUiV3.cpp i to
// bylo dobre dopoty, dopoki liczby rysowal JEDEN ekran. Panel OLED (OledPanel.cpp)
// jest drugim urzadzeniem z wlasnym rdzeniem rysujacym i tez pisze moc w kW —
// a dwie kopie tej samej reguly to dwa miejsca, w ktorych mozna po cichu zostawic
// kropke. Dlatego jedno zrodlo prawdy w naglowku, a nie kopia obok.
//
// DLACZEGO NIE setlocale(): to jest firmware bez pelnej biblioteki locale, a nawet
// gdyby byla, przelaczanie globalnego stanu formatowania dla dwoch znakow w calym
// programie jest gorsze od podmiany jednego znaku na miejscu.
//
// Zamieniamy TYLKO PIERWSZA kropke: w wyniku "%.1f" jest ich najwyzej jedna, a
// przerwanie po niej odroznia liczbe od napisu, ktory ktos kiedys tu poda.
inline void fmt1(char* b, size_t n, float v) {
  snprintf(b, n, "%.1f", v);
  for (char* p = b; *p; ++p)
    if (*p == '.') { *p = ','; break; }
}
