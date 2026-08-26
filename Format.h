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

// (v180) DWA miejsca po przecinku — grosze. Osobna funkcja OBOK fmt1(), a nie
// parametr `digits`: obie sa jednolinijkowe i inline, wiec parametr nic by nie
// oszczedzil, a kazde wywolanie musialoby wtedy powtarzac liczbe miejsc i mogloby
// ja przekrecic. Powod istnienia tej funkcji jest ten sam, co przeniesienia fmt1 tutaj
// w v175: pierwszym odbiorca jest wiersz "zakup dziś X zł" w module PRAD
// (WeatherUiV3.cpp), a wlasne "%.2f" + podmiana kropki w pliku ekranu byloby
// DRUGA kopia tej samej reguly — dokladnie tym, co v175 stad usunelo.
//
// KWOTY ZAWSZE Z GROSZAMI, nawet gdy koncza sie zerem ("4,80 zł", nie "4,8 zł"):
// tak wyglada cena na paragonie i w rachunku, wiec "4,8" czytaloby sie jak liczba
// techniczna, a nie jak pieniadze. Szerokosc napisu i tak jest policzona pod wariant
// najdluzszy, wiec staly jeden znak wiecej nic nie kosztuje.
inline void fmt2(char* b, size_t n, float v) {
  snprintf(b, n, "%.2f", v);
  for (char* p = b; *p; ++p)
    if (*p == '.') { *p = ','; break; }
}
