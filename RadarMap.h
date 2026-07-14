#pragma once

#include <cstdint>

#include "MapData.h"

// Animowana mapa opadow nad Zatoka Gdanska — 2 godziny wstecz, co 20 minut.
//
// RainViewer daje 13 klatek co 10 min (dokladnie 2 h wstecz) i — w darmowym API —
// ZERO klatek prognozy (sprawdzone: "nowcast": []). Wiec to, co pokazujemy, jest
// POMIAREM, nie przewidywaniem. Kierunek i predkosc frontu widac z samego ruchu.
//
// Geometria: cala nasza mapa (18.0-19.2 E, 54.30-54.84 N) miesci sie w JEDNYM
// kafelku RainViewera na zoomie 7 (x=70, y=40) — wiec jedna klatka to jedno
// pobranie. Zoom > 7 nie dziala: serwer zwraca obrazek "Zoom Level Not Supported".
//
// Pamiec: 7 klatek po 224x172 B = 270 kB. Do v50 nie do pomyslenia; dzis siedzi
// w PSRAM i nawet tego nie czuc.

namespace radarmap {

constexpr int FRAMES = 7;                  // -120, -100, -80, -60, -40, -20, 0 min
constexpr int W = gmap::MAP_W;
constexpr int H = gmap::MAP_H;

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

}  // namespace radarmap
