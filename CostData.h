#pragma once

#include <cstdint>

// (v180) Koszt energii KUPIONEJ Z SIECI od polnocy — druga (po AutoData.h) porcja
// danych, ktore w tym projekcie PRZYCHODZA po MQTT, zamiast byc przez nas odpytane.
// Publikuje je Home Assistant na temat <prefix>/dom/stan co 60 s, BEZ retained;
// MqttClient.cpp to subskrybuje i parsuje.
//
// DLACZEGO NIE LICZYMY TEGO SAMI: firmware zna moc chwilowa z sieci (PvClient czyta
// rejestry miernika), ale NIE zna STAWKI — w taryfie G12w cena zalezy od strefy,
// czyli od godziny i dnia tygodnia, a same strefy zmieniaja sie razem z umowa.
// Policzenie kosztu tutaj oznaczaloby wpisanie cennika do firmware'u tylko-OTA
// i pilnowanie go przy kazdej zmianie taryfy. Home Assistant sumuje to minuta po
// minucie i ma cennik w JEDNYM miejscu — my dostajemy gotowa liczbe.
//
// TEMAT NAZYWA SIE "dom/stan", A NIE "koszt": to jest stan domu, a `zl` jest jego
// PIERWSZYM polem. Kolejne (np. koszt oddania, licznik wody) maja dochodzic tutaj,
// bez nowego tematu i bez trzeciej subskrypcji — dlatego parser w MqttClient.cpp
// czyta pola po nazwie i nie wymaga kompletu.
struct CostModel {
  // millis() ODBIORU wiadomosci (0 = nie przyszla ani jedna od uruchomienia).
  // DOKLADNIE ten sam idiom, co AutoModel::atMs: swiezosc liczy freshMs()
  // z WeatherUiV3.cpp, czyli roznica ZE ZNAKIEM na int32 — millis() przekreca sie
  // po ~49 dniach pracy, a pisze go INNY watek (netTask) niz ten, ktory czyta
  // (petla rysowania). Prog: cfg::COST_STALE_MS.
  uint32_t atMs = 0;

  // Koszt energii pobranej z sieci OD POLNOCY [PLN]. Zeruje sie o polnocy PO
  // STRONIE Home Assistanta — my tej granicy nie znamy, wiec jej nie zgadujemy
  // i nigdy sami nie zerujemy tego pola.
  float zl = 0.f;
};

// Straznik budzetu, nie ozdobnik — ta sama rola, co static_assert przy AutoModel.
// Model zyje w DWOCH kopiach (odbiorczej w MqttClient.cpp i ekranowej uiCost
// w pogoda-gdynia.ino), wiec kazdy dolozony bajt kosztuje DWA bajty statycznego
// RAM-u przy barierze 76 000 B (tools/release.sh). Dzis 8 B; prog 16 B daje miejsce
// na jeszcze jedno-dwa pola z rozszerzalnego ladunku i ZATRZYMA kompilacje, zanim
// ktos po cichu wstawi tu String albo tablice napisow.
static_assert(sizeof(CostModel) <= 16,
              "CostModel zyje w dwoch kopiach — powyzej 16 B zaczyna byc widoczny "
              "w budzecie statycznego RAM-u (bariera 76 000 B)");
