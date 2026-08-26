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

  // (v181) SKUMULOWANA korzysc z fotowoltaiki od uruchomienia instalacji [PELNE PLN].
  // DRUGIE pole tego ladunku i pierwszy dowod, ze temat "dom/stan" mial byc
  // rozszerzalny (patrz naglowek pliku) — nowy temat ani trzecia subskrypcja nie byly
  // potrzebne. Zasila ekran ZWROT (cfg::VIEW_PAYBACK).
  //
  // NARASTAJACO, nie "dzisiaj": w odroznieniu od `zl` wyzej ta liczba NIGDY sie nie
  // zeruje i tylko rosnie. Licznik zycia instalacji trzyma Home Assistant, my go
  // wylacznie wyswietlamy.
  //
  // int32_t, a nie float: to sa PELNE ZLOTE (grosze przy kwocie rzedu 13 000 zl sa
  // szumem, a na ekranie i tak ich nie widac), a typ ze znakiem bierze sie stad, ze
  // roznica (cfg::PV_KOSZT_PLN - pvPln) ma prawo zejsc PONIZEJ ZERA w dniu splaty.
  // 4 B zamiast 2: uint16_t skonczylby sie na 65 535 zl, czyli — przy tempie ~334
  // zl/mies. — okolo roku 2039, a ta instalacja ma zyc dluzej niz to przepelnienie.
  int32_t pvPln = 0;
};

// Straznik budzetu, nie ozdobnik — ta sama rola, co static_assert przy AutoModel.
// Model zyje w DWOCH kopiach (odbiorczej w MqttClient.cpp i ekranowej uiCost
// w pogoda-gdynia.ino), wiec kazdy dolozony bajt kosztuje DWA bajty statycznego
// RAM-u przy barierze 76 000 B (tools/release.sh). (v181) Dzis 12 B — doszlo pvPln
// (4 B) i z zapasu, ktory prog 16 B mial dac "na jeszcze jedno-dwa pola", zostalo
// dokladnie JEDNO 4-bajtowe pole. Progu NIE PODNOSIMY razem z rozmiarem: straznik,
// ktory ustepuje przy kazdym dolozeniu, nie pilnuje juz niczego. Trzecie pole ma sie
// tu ZATRZYMAC i wymusic decyzje, a nie przejsc mimochodem.
static_assert(sizeof(CostModel) <= 16,
              "CostModel zyje w dwoch kopiach — powyzej 16 B zaczyna byc widoczny "
              "w budzecie statycznego RAM-u (bariera 76 000 B)");
// (v181) Drugi straznik, tym razem na TYP, nie na rozmiar: pvPln MUSI byc ze znakiem.
// Zamiana na uint32_t nie ruszylaby sizeof (wiec asercja wyzej by tego nie zlapala),
// a wywrocilaby ekran ZWROT w dniu splaty instalacji — (PV_KOSZT_PLN - pvPln) < 0
// przekrecilby sie w ~4 mld i pasek postepu spadlby ze 100% na 0%.
static_assert(static_cast<decltype(CostModel::pvPln)>(-1) < 0,
              "CostModel::pvPln musi byc typem ZE ZNAKIEM (roznica koszt-korzysc "
              "schodzi ponizej zera po splacie instalacji)");
