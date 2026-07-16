#pragma once

#include <Arduino.h>

#include <cstdint>

// Piec Viessmann (Vitodens 050-W) przez chmurowe API ViCare.
//
// DLACZEGO CHMURA, skoro falownik czytamy lokalnie po Modbusie?
// Bo Viessmann nie zostawia wyboru. Skan portow 192.168.0.31 (moduł WiFi_SA0041):
// urzadzenie odpowiada na ping, ale NIE NASLUCHUJE na zadnym porcie 1-1024 ani na
// typowych wysokich. Modul laczy sie wylacznie na zewnatrz, do chmury. Jedyna
// lokalna alternatywa to OpenTherm — ale ten przejmuje sterowanie ogrzewaniem,
// wiec to zupelnie inna decyzja niz wyswietlanie danych.
//
// TOKENY:
//   access  — 60 minut, odnawiany automatycznie
//   refresh — 180 dni, potem trzeba raz kliknac autoryzacje w przegladarce
// Refresh token siedzi w NVS i NIGDY nie trafia do repo ani do /api/state.

namespace vi {

struct Model {
  // ciepła woda
  float dhwTempC = 0.f;
  float dhwTargetC = 0.f;
  char dhwMode[12] = {};        // comfort | eco | off

  // kocioł
  float supplyTempC = 0.f;
  bool burnerActive = false;
  int modulationPct = 0;

  // LICZNIKI KUMULACYJNE PALNIKA — jedyne dane o piecu ODPORNE NA ALIASING.
  //
  // Odpytujemy piec co 3 minuty (limit Viessmanna: 120/10 min, 1450/dobe), a cykl
  // grzania CWU trwa czasem krocej. Cykl, ktory zaczal sie i skonczyl MIEDZY dwoma
  // odpytami, dla burnerActive/modulationPct nie istnial — i wlasnie dlatego licznik
  // gazu pokazuje 1,1 m3 na dobe przy modulacji "caly czas zero". Fotowoltaika idzie
  // co 30 s, wiec trafia; piec nie ma szans.
  // Licznik kumulacyjny tego problemu nie ma z definicji: roznica miedzy dwoma
  // odpytami mowi, ile palnik chodzil, nawet jesli ANI RAZU nie zlapalismy go w akcji.
  //
  // hours JEST FLOATEM, i to nie kosmetyka. Do v98 stalo tu uint32_t i parser robil
  // static_cast<uint32_t>(v) — dwuminutowe palenie to 0,033 h, czyli po obcieciu
  // dokladnie zero. Licznik odporny na aliasing byl zaokraglany do bezuzytecznosci.
  float burnerHours = 0.f;
  uint32_t burnerStarts = 0;

  // obieg grzewczy
  char circuitMode[12] = {};    // heating | standby
  float circuitTargetC = 0.f;   // to ustawia sie recznie zima

  // zużycie (dziś)
  float gasDhwM3 = 0.f;
  float gasHeatM3 = 0.f;
  float heatDhwKwh = 0.f;
  float heatHeatKwh = 0.f;
  float powerKwh = 0.f;         // prąd zżarty przez sam piec

  int wifiRssi = 0;

  // "Przyszlo z API" osobno od "wynosi zero".
  // ArduinoJson po cichu oddaje 0 dla brakujacego pola, a Viessmann potrafi nie
  // przyslac "value" wcale: feature wylaczony (isEnabled/isReady), czujnik
  // notConnected, nowy uklad pol. Bez tych flag "CWU 0,0 C" bylo nieodroznialne
  // od zamarznietego bojlera. Dokladnie ta sama lekcja co ble::Sensor::hasTemp/
  // hasHum — tam komentarz mowi wprost: "Bez tych flag rysowalismy 0.0 C, dopoki
  // nie doszla ramka z temperatura — czyli klamalismy".
  // UI ma pokazac "—" tam, gdzie flaga jest false, a nie zero.
  bool hasDhwTemp = false;
  bool hasDhwTarget = false;
  bool hasSupplyTemp = false;
  bool hasBurnerState = false;
  bool hasModulation = false;
  // Bez tych flag "hours = 0,0" znaczy naraz "cecha heating.burners.0.statistics
  // nie przyszla" i "palnik ma zero przepracowanych godzin". Przy pomiarze
  // rozdzielczosci licznika to jest roznica miedzy "API nie daje" a "nie rusza sie".
  //
  // DWIE FLAGI, NIE JEDNA: `hours` i `starts` to dwie osobne wlasciwosci tej samej
  // cechy i moga przyjsc niezaleznie. Do v99 stala tu jedna flaga, podnoszona na OR
  // przez obie — co podkopywalo jedyny cel tych pol: gdyby doszlo samo `starts`,
  // diagnostyka pokazalaby "statystyki sa" przy burner_hours = 0,0, czyli "licznik
  // godzin stoi" zamiast "`hours` nie ma w odpowiedzi". Dokladnie ta konfuzja, dla
  // ktorej flaga powstala. W /api/diag: has_hours i has_starts, osobno.
  bool hasBurnerHours = false;
  bool hasBurnerStarts = false;
  bool hasCircuitTarget = false;
  bool hasGas = false;
  bool hasHeat = false;
  bool hasWifiRssi = false;

  bool online = false;
  bool valid = false;
  uint32_t okAt = 0;
  char err[56] = {};
};

// --- autoryzacja (jednorazowa, z panelu WWW) ---
// Zwraca URL, ktory uzytkownik otwiera w przegladarce. Generuje przy okazji
// code_verifier (PKCE) i trzyma go do czasu powrotu kodu.
String authUrl(const char* clientId, const char* redirectUri);

// Kod z redirecta -> tokeny. Kod zyje 20 SEKUND, wiec ESP musi go dostac od razu
// (stad redirect prosto na panel, a nie na localhost).
bool exchangeCode(const char* code, const char* redirectUri, char* errOut, size_t errLen);

// Ile dni zostalo do wygasniecia refresh tokena (180 dni od autoryzacji).
int daysLeft();

// --- sterowanie ---
// UWAGA: to ZMIENIA ustawienia ogrzewania w domu. Wolane tylko jawnie, nigdy
// automatycznie — automat krzywej grzewczej dostanie osobny wlacznik i tryb suchy.
// Zakres wymuszany przez piec: 2..80 C, krok 1.
bool setCircuitTemp(int celsius, char* errOut, size_t errLen);

// Ostatnio odczytana nastawa obiegu (do porownania "wyliczam X, piec ma Y").
float circuitTarget();

// --- dane ---
bool fetch(Model& out);        // wolane z netTask
void forget();                 // kasuje tokeny

}  // namespace vi
