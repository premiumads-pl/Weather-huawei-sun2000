#pragma once

#include <cstdint>

// Nasluch czujnikow Xiaomi (LYWSD03MMC "Mi Temperature and Humidity Monitor 2")
// przez rozglaszanie BLE. NIE laczymy sie z czujnikiem i nie parujemy — czujnik
// sam nadaje ramke co kilka/kilkanascie sekund, wiec wystarczy pasywny nasluch.
//
// Formaty ramek (service data), ktore rozpoznajemy:
//   0x181A, 15 B  — firmware pvvx (custom), otwarty tekst
//   0x181A, 13 B  — firmware ATC (custom), otwarty tekst
//   0xFE95        — MiBeacon (fabryczny); czesto ZASZYFROWANY -> potrzebny bindkey
//
// Nieznane ramki trafiaja do logu w postaci szesnastkowej, zeby dalo sie je
// rozpoznac ZDALNIE (urzadzenie wisi na scianie, bez USB).

namespace ble {

constexpr int MAX_SENSORS = 4;

struct Sensor {
  char mac[18] = {};
  float tempC = 0.f;
  float humidity = 0.f;
  int batteryPct = 0;
  int batteryMv = 0;
  int rssi = 0;
  uint32_t seenAt = 0;  // millis() ostatniej ramki
  bool valid = false;
};

void begin();

// Pasywny nasluch przez `seconds` sekund. Wolane z netTask — blokuje.
void scan(int seconds);

int count();
Sensor get(int i);

bool ready();
const char* lastError();

}  // namespace ble
