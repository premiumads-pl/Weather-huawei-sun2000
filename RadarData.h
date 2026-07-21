#pragma once

#include <cstdint>

// Model posredni ekranu RADAR (v126). Odpowiednik RoomData.h dla drugiego ekranu,
// ktory do v125 sam sobie liczyl dane w trakcie rysowania.
//
// CO STAD ZNIKNELO Z RYSOWANIA:
//  * wybor klatki animacji (nowMs / RADAR_FRAME_MS % steps, z pauza na najnowszej),
//  * wiek/godzina tej klatki (offsetMin, epoch),
//  * wektor przesuniecia chmur liczony z wiatru (sin/cos, skala mapy, swiezosc
//    pogody) - jedyny "ciezki" rachunek na tym ekranie,
//  * ZAPIS DO GLOBALNEJ DIAGNOSTYKI (diag().radarFrame / radarFrameMin).
//
// Ten ostatni punkt byl najgorszy: funkcja rysujaca MUTOWALA globalny stan, ktory
// czyta panel WWW z innego rdzenia. Ekran rysuje sie takze do zrzutu BMP (webTask,
// drugi rdzen), wiec dwie sciezki rysowania pisaly na zmiane do tego samego pola.
// Teraz pisze je warstwa danych, raz na cykl, z jednego watku.
//
// UWAGA NA SEMANTYKE diag().radarFrame: pole opisuje "ktora klatka jest rysowana".
// Do v125 zapisywal je RYSUJACY, wiec przy ekranie radaru poza rotacja zostawala
// tam wartosc sprzed kilkunastu minut. Teraz liczy sie co cykl, czyli mowi "ktora
// klatka POSZLABY na ekran teraz". Dla diagnostyki animacji (a po to jest) to ta
// sama informacja, tylko zawsze swieza.
//
// Rozmiar: 32 B. Rastry (13 x 55 kB) zostaja w PSRAM, w RadarMap.cpp - model niesie
// WSKAZNIK do wybranej klatki, nie jej kopie.

struct RadarViewModel {
  int frames = 0;           // radarmap::count(), 0 = nie ma czego rysowac
  int frameIdx = 0;         // wybrana klatka animacji
  int32_t frameMin = 0;     // jej przesuniecie w minutach (ujemne = przeszlosc)
  uint32_t frameEpoch = 0;  // jej czas (0 = nieznany)

  // Przesuniecie PROBKOWANIA rastra w pikselach - interpolacja ruchu chmur miedzy
  // klatkami, policzona z wiatru. Zero = stoimy w miejscu (cisza, nieswieza pogoda
  // albo najnowsza klatka, ktora nie ma do czego "dojezdzac").
  int16_t shiftX = 0;
  int16_t shiftY = 0;

  // Raster wybranej klatki: rw*rh bajtow poziomu opadu (0 = sucho, 1..5 rosnaco),
  // w PSRAM. nullptr = brak. Bufory alokuje raz radarmap::begin() i nie zwalnia ich
  // az do nieudanego startu, wiec wskaznik przezyje klatke rysowania.
  const uint8_t* raster = nullptr;
  int16_t rw = 0;
  int16_t rh = 0;

  const char* error = "";   // radarmap::lastError(), do karty "brak danych"
  bool hasRain = false;     // czy w KTOREJKOLWIEK klatce jest opad (tytul V2)

  // Odczyt piksela rastra. Inline i w modelu, a nie wolanie radarmap::levelAt() z
  // petli rysujacej - i to nie jest kosmetyka: petla opadu robi do ~110 tys.
  // odczytow na klatke, a budzet calej klatki to 21 ms. Do v125 kazdy z nich byl
  // wywolaniem do innej jednostki kompilacji (z powtarzanym sprawdzaniem indeksu
  // klatki i wskaznika bufora); teraz to indeksowanie tablicy, ktore kompilator
  // widzi w calosci. Zakres poza rastrem zwraca 0 - dokladnie jak levelAt() - wiec
  // ujemne wspolrzedne po odjeciu shiftX/shiftY sa bezpieczne z definicji.
  uint8_t levelAt(int x, int y) const {
    if (raster == nullptr) return 0;
    if (x < 0 || x >= rw || y < 0 || y >= rh) return 0;
    return raster[y * rw + x];
  }
};
