#pragma once

#include <cstdint>
#include <cstring>

constexpr int FLIGHT_MAX = 6;  // ile samolotow pokazujemy na mapie i liscie

struct Flight {
  char callsign[10] = {};
  char type[6] = {};      // np. B738
  char route[12] = {};    // np. "WAW-GDN"
  char orig[5] = {};      // IATA
  char dest[5] = {};      // IATA
  float lat = 0.f;
  float lon = 0.f;
  int32_t altFt = 0;
  int16_t gs = 0;         // predkosc [kt]
  int16_t track = 0;      // kurs [deg]
  bool routeKnown = false;
};

struct FlightModel {
  Flight list[FLIGHT_MAX]{};
  int count = 0;   // ile w liscie
  int total = 0;   // ile w zasiegu mapy
  bool ready = false;
  char errorMsg[40] = {};
};

inline bool flightToGdansk(const Flight& f) {
  return f.routeKnown && strncmp(f.dest, "GDN", 3) == 0;
}

inline bool flightFromGdansk(const Flight& f) {
  return f.routeKnown && strncmp(f.orig, "GDN", 3) == 0;
}
