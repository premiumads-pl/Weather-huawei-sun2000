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
  uint32_t burnerHours = 0;
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
