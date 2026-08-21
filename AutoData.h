#pragma once

#include <cstdint>
#include <cstring>

// Stan samochodu (Tesla) — JEDYNE dane w tym projekcie, ktore PRZYCHODZA po MQTT,
// zamiast byc przez nas odpytane. Publikuje je Home Assistant na temat
// <prefix>/auto/stan mniej wiecej co 15 s; MqttClient.cpp to subskrybuje i parsuje.
//
// DLACZEGO MQTT, A NIE WLASNY KLIENT HTTP: do auta nie da sie zapytac wprost —
// dostep idzie przez chmure Tesli z tokenem i limitem zapytan, a Home Assistant ma
// juz te integracje utrzymana i wybudza auto wtedy, kiedy trzeba. Powielanie tego
// w firmwarze oznaczaloby drugi token do odnawiania i drugi licznik limitu.
//
// ROZMIAR MA ZNACZENIE. Bariera statycznego RAM-u calego programu to 76 000 B
// (tools/release.sh), a ten model zyje w DWOCH kopiach: odbiorczej (MqttClient.cpp,
// pisze ja netTask) i ekranowej (uiAuto w pogoda-gdynia.ino, czyta rdzen rysujacy).
// Dlatego teksty stoja w krotkich char[], a nie w String: String to 12 B naglowka
// w statyku PLUS blok na stercie na kazda kopie, czyli wiecej pamieci i fragmentacja
// za przechowanie szesciu liter. Cala struktura ma 44 B (2 x 44 = 88 B budzetu).
struct AutoModel {
  // millis() ODBIORU wiadomosci (0 = nie przyszla ani jedna od uruchomienia).
  // Znacznik jest tu, a nie w diag(), bo to on rozstrzyga o POMIJANIU ekranu w
  // rotacji (cfg::AUTO_STALE_MS) — czyli nalezy do danych, nie do diagnostyki.
  // Wiek liczymy ZE ZNAKIEM na int32 (idiom freshMs/okAgeS z WeatherUiV3.cpp):
  // millis() przekreca sie po ~49 dniach pracy, a pisze go INNY watek niz czyta.
  uint32_t atMs = 0;

  float kw = 0.f;        // biezaca moc ladowania [kW]
  float addedKwh = 0.f;  // energia dodana w BIEZACEJ sesji [kWh]
  float sunKwh = 0.f;    // dzis oddane do auta ZE SLONCA [kWh]
  float gridKwh = 0.f;   // dzis oddane do auta Z SIECI [kWh]

  int16_t rangeKm = 0;   // zasieg [km]
  uint8_t soc = 0;       // naladowanie [%] 0..100
  uint8_t limitPct = 0;  // docelowe naladowanie [%]
  uint8_t amps = 0;      // zadany prad [A]
  bool cable = false;    // kabel podpiety

  // Tryb ladowania — kontrakt z automatyka w garazu: dokladnie jeden z
  // "OFF" / "PV" / "PV+MIN" / "MAX". 8 B, bo najdluzszy ma 6 znakow + NUL, a 8
  // trzyma strukture wyrownana bez dziury.
  char mode[8] = {};
  // Stan techniczny — dokladnie jeden z "laduje" / "czeka" / "stoi" / "spi" /
  // "brak". CELOWO BEZ POLSKICH ZNAKOW: to pole idzie po drucie i sluzy do
  // porownan, a nie do wyswietlenia. Napis dla czlowieka robi autoStateLabel().
  char state[8] = {};

  // Czy pole `state` niesie dokladnie ten stan (porownanie bez ryzyka literowki
  // w dwoch miejscach naraz).
  bool stateIs(const char* s) const { return strcmp(state, s) == 0; }
  bool modeIs(const char* m) const { return strcmp(mode, m) == 0; }
};

// Straznik budzetu, nie ozdobnik. Ten model istnieje w DWOCH kopiach (odbiorczej
// w MqttClient.cpp i ekranowej uiAuto w pogoda-gdynia.ino), wiec kazdy dolozony bajt
// kosztuje DWA bajty statycznego RAM-u przy barierze 76 000 B (tools/release.sh).
// Zmierzone po dodaniu tego ekranu: caly program urosl o 136 B, czyli dokladnie
// 2 x 44 B modelu + bufor tematu MQTT + uchwyt mutexu. Prog 80 B daje zapas na
// jeszcze jedno-dwa pola; przekroczenie go ma ZATRZYMAC kompilacje, a nie po cichu
// zjesc margines wydania.
static_assert(sizeof(AutoModel) < 80,
              "AutoModel zyje w dwoch kopiach — powyzej 80 B zaczyna byc widoczny "
              "w budzecie statycznego RAM-u (bariera 76 000 B)");

// Techniczny `state` -> napis dla czlowieka. Zwraca literal z flasha, wiec nie
// kosztuje ani bajta RAM-u i nie trzeba go nigdzie kopiowac.
//
// DLACZEGO MAPOWANIE, A NIE POLSKI NAPIS PROSTO Z HOME ASSISTANTA: napis na ekranie
// jest DECYZJA UKLADU — musi sie zmiescic w kolumnie szerokiej na 78 px i moze sie
// zmienic razem z ukladem. Gdyby przychodzil gotowy z automatyki, kazda zmiana slowa
// po tamtej stronie potrafilaby po cichu rozwalic ekran (dluzszy napis nachodzi na
// sasiednia kolumne, a pltxt::drawString POMIJA PO CICHU glify spoza fontu, wiec
// polski znak w nowym slowie po prostu zniknalby z wyrazu). Tu lista jest zamknieta
// i policzona.
inline const char* autoStateLabel(const AutoModel& a) {
  if (a.stateIs("laduje")) return "ładuje";
  if (a.stateIs("czeka")) return "czeka";
  if (a.stateIs("stoi")) return "postój";
  if (a.stateIs("spi")) return "śpi";
  if (a.stateIs("brak")) return "brak kabla";
  return "-";   // nieznany stan: kreska, a nie zgadywanie
}
