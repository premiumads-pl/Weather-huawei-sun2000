#pragma once

#include "FlightData.h"

class FlightClient {
 public:
  bool fetch(FlightModel& out);

 private:
  // Cache tras — trasa dla danego znaku wywolawczego sie nie zmienia,
  // wiec pytamy o nia raz i trzymamy w RAM.
  struct RouteEntry {
    char callsign[10];
    char route[12];
    char orig[5];
    char dest[5];
    bool known;   // czy API cokolwiek zwrocilo
    bool used;
  };
  static constexpr int CACHE_N = 28;
  RouteEntry cache_[CACHE_N]{};
  int cacheNext_ = 0;

  const RouteEntry* findCached(const char* callsign) const;
  bool lookupRoute(const char* callsign, RouteEntry& out);
};
