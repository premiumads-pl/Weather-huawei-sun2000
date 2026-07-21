#pragma once

#include <cstddef>
#include <cstdint>

// Nasluch czujnikow Xiaomi (LYWSD03MMC "Mi Temperature and Humidity Monitor 2")
// przez rozglaszanie BLE. NIE laczymy sie z czujnikiem i nie parujemy — czujnik
// sam nadaje ramke co kilka/kilkanascie sekund, wiec wystarczy pasywny nasluch.
//
// Formaty ramek (service data), ktore rozpoznajemy:
//   0x181A, 15 B  — firmware pvvx (custom), otwarty tekst
//   0x181A, 13 B  — firmware ATC (custom), otwarty tekst
//   0xFE95        — MiBeacon (fabryczny Xiaomi); ZASZYFROWANY -> potrzebny bindkey
//   0xFDCD        — Qingping (cgllc.sensor_ht.qpg1); OTWARTY TEKST, bez klucza
//
// Nieznane ramki trafiaja do logu w postaci szesnastkowej, zeby dalo sie je
// rozpoznac ZDALNIE (urzadzenie wisi na scianie, bez USB).

namespace ble {

// 8, nie 4: uzytkownik ma 2 czujniki i dokłada kolejne 2, a obce nadajniki w bloku
// tez potrafia zajac slot. Kazdy slot to ~60 B — nie ma po co oszczedzac.
constexpr int MAX_SENSORS = 8;

struct Sensor {
  char mac[18] = {};
  float tempC = 0.f;
  float humidity = 0.f;
  int batteryPct = 0;
  int batteryMv = 0;
  // DWA zrodla, DWA pomiary. Trzymanie jednego "rssi" bylo bledem: bramka i wlasne
  // radio nadpisywaly sie nawzajem, wiec /api/ble pokazywalo raz jedno, raz drugie —
  // a narzedzie do pomiaru zasiegu porownywalo Shelly sam ze soba.
  int rssiOwn = 0;       // z naszego radia (0 = nie slyszymy)
  int rssiGw = 0;        // z bramki
  uint32_t ownAt = 0;
  uint32_t gwAt = 0;

  int rssi = 0;          // zrodlo, ktore dalo ostatni odczyt — do zgodnosci
  uint32_t seenAt = 0;   // millis() ostatniej ramki
  bool valid = false;    // mamy JAKIKOLWIEK odczyt
  // Czujnik nadaje temperature i wilgotnosc w OSOBNYCH ramkach. Bez tych flag
  // rysowalismy 0.0 C, dopoki nie doszla ramka z temperatura — czyli klamalismy.
  bool hasTemp = false;
  bool hasHum = false;
  bool encrypted = false;
  bool needsKey = false;  // widzimy czujnik, ale brakuje bindkeya (albo jest zly)
  bool viaGw = false;     // odczyt przyszedl przez bramke, nie z wlasnego radia
};

void begin();

// Pasywny nasluch przez `seconds` sekund. Wolane z netTask — blokuje.
void scan(int seconds);

int count();
Sensor get(int i);

// TRWALY wskaznik na adres czujnika `i` ("" poza zakresem). Po co osobno, skoro
// get() zwraca cala strukture z polem mac: bo get() oddaje KOPIE, ktora ginie
// razem z lokalna zmienna. Model ekranu W DOMU (RoomData.h) trzyma nazwe pokoju
// jako wskaznik i dla czujnika bez wpisu w ustawieniach musi pokazac MAC - a taki
// wskaznik musi przezyc klatke rysowania.
//
// Bezpieczne bez mutexu i to nie jest niedopatrzenie: slot dostaje adres RAZ, w
// chwili zalozenia (slotFor), a kazdy pozniejszy zapis wpisuje ten SAM ciag (slot
// jest dopasowywany po strcmp z tego adresu). Zawartosc jest wiec stala, a tablica
// gSensors[] jest statyczna - nie realokuje sie i nie znika.
const char* macOf(int i);

bool ready();
bool scanning();  // trwa nasluch — inni maja nie brac duzych blokow

// Wstrzykniecie surowej ramki MiBeacon (0xFE95) z BRAMKI — np. Shelly stojacego
// bliżej czujnika. Bramka NIE odszyfrowuje niczego: przekazuje szyfrogram, a klucze
// zostaja tutaj, w NVS. Przetwarzanie jest identyczne jak dla wlasnego radia.
bool feedRaw(const char* mac, const uint8_t* data, size_t len, int rssi);

// Ramka Qingping (0xFDCD) z bramki — inny format niz MiBeacon, wiec osobna droga.
bool feedRawQingping(const char* mac, const uint8_t* data, size_t len, int rssi);
const char* lastError();

}  // namespace ble
