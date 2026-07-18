#pragma once

#include <cstdint>

#include "MapDataRadar.h"

// Animowana mapa opadow nad Zatoka Gdanska — 2 godziny wstecz, co 10 minut.
//
// RainViewer daje 13 klatek co 10 min (dokladnie 2 h wstecz) i — w darmowym API —
// ZERO klatek prognozy (sprawdzone: "nowcast": []). Wiec to, co pokazujemy, jest
// POMIAREM, nie przewidywaniem. Kierunek i predkosc frontu widac z samego ruchu.
// Do v109 bralismy co DRUGA klatke (7 z 13, co 20 min) — front przeskakiwal 48 px
// naraz (15% szerokosci ekranu) i wygladal na "latajacy". Teraz bierzemy KAZDA
// klatke, jaka RainViewer oddaje: sam skok bazowy spada o polowe, a reszte
// plynnosci dorzuca interpolacja wektorem wiatru w WeatherUi::drawViewRadar
// (potrzebuje gestszych klatek, zeby miec miedzy czym "dojezdzac").
//
// Geometria (v110, POSZERZENIE Z 111 NA ~300 KM — sprostowanie: ten komentarz
// do niedawna mowil o jednym kaflu zoom 7, co juz NIE JEST prawda): mapa to dzis
// gmapr (300 km szerokosci, LAT 53.8457-55.2943, LON 16.2756-20.9244), nie gmapw
// (ta zostaje wylacznie dla ekranu samolotow — patrz MapDataWide.h). Przy takiej
// rozpietosci okno NIE miesci sie w jednym kaflu RainViewera nawet na zoomie 6:
// pokrywa DWA kafle w poziomie i jeden w pionie (patrz RadarMap.cpp,
// computeGeometry() — zakres liczony DYNAMICZNIE z granic gmapr, nie zaszyty).
// Zoom 6 (nie 7): przy zoom 7 to samo okno siegaloby TRZECH kafli w poziomie
// (sprawdzone numerycznie), zoom 6 starcza na dwa — mniej pobran, a rozdzielczosc
// kafla i tak jest grubsza od potrzebnej (radar ma piksele grubsze niz nasza mapa).
// Zoom > 7 nie dziala: serwer zwraca obrazek "Zoom Level Not Supported".
//
// Pamiec: 13 klatek x 320x172 B (=55 040 B/klatka) = 715 520 B (~715 kB) w PSRAM.
// (Ten komentarz liczyl kiedys "7 klatek po 224x172 B = 270 kB" — 224 to szerokosc
// mapy SPRZED przejscia na szeroka gmapw; W ponizej to od dawna 320 (dzis z gmapr,
// tej samej wielkosci co gmapw), wiec przy okazji 7->13 liczba wraca do prawdy.)
// Do v50 nie do pomyslenia; dzis siedzi w PSRAM i nawet tego nie czuc.

namespace radarmap {

// -120, -110, -100, -90, -80, -70, -60, -50, -40, -30, -20, -10, 0 min (co 10 min)
constexpr int FRAMES = 13;
constexpr int W = gmapr::MAP_W;
constexpr int H = gmapr::MAP_H;

struct Frame {
  uint32_t epoch = 0;       // czas klatki
  int32_t offsetMin = 0;    // minuty wzgledem "teraz" (ujemne = przeszlosc)
  bool valid = false;
};

bool begin();               // alokacja buforow w PSRAM
bool fetch();               // pobiera komplet klatek (wolane z netTask)

int count();                             // ile klatek jest gotowych
const Frame& frame(int i);
uint8_t levelAt(int i, int x, int y);    // 0 = brak opadu, 1..5 rosnaco
uint32_t updatedAt();                    // millis() ostatniego udanego pobrania
const char* lastError();

// Czy w KTOREJKOLWIEK klatce jest opad. Ekran radaru bez deszczu to pusta mapa,
// wiec rotacja go wtedy pomija — a pasek postepu zaznacza go innym kolorem.
bool hasRain();

// Symulacja: sztuczny front przechodzacy z zachodu na wschod. Do obejrzenia,
// jak wyglada wizualizacja, gdy akurat nie pada. Wlaczana z panelu.
void setDemo(bool on);
bool demo();

// Po wylaczeniu symulacji trzeba NATYCHMIAST sciagnac prawdziwe klatki — inaczej
// ekran wisi na "Pobieram mapę opadów" az do najblizszego cyklu (10 minut).
bool wantsFetch();

}  // namespace radarmap
