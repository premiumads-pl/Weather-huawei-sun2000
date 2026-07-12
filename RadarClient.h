#pragma once

#include <cstdint>

// Radar opadowy (RainViewer) — realny obraz z radarów meteo, w przeciwieństwie
// do Open-Meteo, które jest modelem prognostycznym i potrafi przegapić lokalną
// ulewę.
//
// Jak to działa:
//   1. api.rainviewer.com/public/weather-maps.json -> ścieżka do najnowszej klatki
//   2. tilecache.rainviewer.com/<klatka>/256/7/<x>/<y>/0/0_0.png  (zoom 7 = ~710 m/px;
//      zoomy >= 8 zwracają placeholder "Zoom Level Not Supported")
//   3. dekodujemy PNG, czytamy piksele wokół naszych współrzędnych
//   4. kolor -> natężenie (paleta RainViewer to gradient: beż -> niebieski -> żółty
//      -> pomarańczowy -> czerwony)

struct RadarSnapshot {
  uint8_t level = 0;       // 0 = brak, 1..5 = od mżawki do ulewy
  uint32_t ageSec = 0;     // wiek klatki radarowej
  bool valid = false;
  char errorMsg[40] = {};
};

const char* radarLabel(uint8_t level);

// Dekoder PNG potrzebuje spójnego bloku ~46 kB. Kiedyś bufor ekranu (132 kB) tak
// kruszył stertę, że największy wolny kawałek miał ~35 kB — radar musiał więc prosić
// UI o oddanie bufora, a ekran na czas dekodowania zamierał. Od czasu rysowania
// w dwóch pasach (bufor 66 kB) miejsca starcza i ta proteza zniknęła: radar dekoduje
// w tle, nie zatrzymując obrazu.

class RadarClient {
 public:
  bool fetch(RadarSnapshot& out);
};
