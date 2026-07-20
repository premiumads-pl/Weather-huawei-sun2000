#pragma once

#include <cstddef>

#include "AirData.h"

// Nazwa slowna indeksu ARMAAG (1..6) — "" gdy index <= 0 (brak danych). Wspolna dla
// ekranu (WeatherUi::drawViewAir) i /api/diag (Portal.cpp), zeby obie strony mowily
// dokladnie to samo slowo dla tej samej liczby. Pelna tabela progow — patrz AirClient.cpp.
const char* airIndexName(int index);

class AirClient {
 public:
  bool fetch(AirModel& out);

 private:
  bool parsePayload(const char* json, std::size_t len, AirModel& out) const;
};
