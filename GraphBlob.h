#pragma once

#include <cstdint>

// (v194) WYKRES MOCY LADOWANIA PRZEZYWA RESTART — utrwalenie bufora z OledPanel.cpp.
//
// OBJAW: po kazdym restarcie (a wiec po KAZDEJ aktualizacji OTA) dolny pas ekranu
// OLED zaczynal od zera i przez pierwsze minuty pokazywal "brak ładowania", chociaz
// auto ladowalo sie bez przerwy. Bufor probek siedzi w PSRAM, a PSRAM znika razem
// z zasilaniem — zbieranie bez utrwalania to ten sam stan, ktory przy dziennym logu
// gazu zostal opisany jako najgorszy z mozliwych: kod wyglada na dzialajacy, koszt
// pamieci jest placony, pozytku zero.
//
// MIEJSCE NIE BYLO PROBLEMEM I WARTO, ZEBY TO ZOSTALO ZAPISANE, bo pytanie brzmialo
// wprost "czy mamy tyle pamieci": ten blob kosztuje 7 wpisow NVS (2 narzutu + 5 na
// 136 B), a wolnych jest 4187 przy zajetosci 7,5%. To jest 0,17% dostepnej puli.
// Problemem bylo COS INNEGO — patrz akapit o zegarze nizej.
//
// CO SIE UTRWALA: same probki plus tyle stanu, ile trzeba, zeby po starcie
// rozstrzygnac JEDNO pytanie — czy sesja ladowania trwa dalej, czy sie skonczyla.
//
// DLACZEGO TO W OGOLE JEST UCZCIWE. Wykres jest SESYJNY, a nie zegarowy: bufor
// kasuje sie na poczatku kazdej nowej sesji (graphTick), kreski godzinowe stoja co
// 20 kolumn OD PRAWEJ, a nie o pelnych godzinach zegara. Probki opisuja wiec przebieg
// sesji, a nie odcinek doby — i pozostaja prawdziwe niezaleznie od tego, kiedy zostaly
// zebrane. Gdyby os byla zegarowa, odtworzenie bufora po przerwie wymagaloby wpisania
// czegos w miejsce brakujacych kolumn, a kazda taka wartosc bylaby zmyslona.
//
// ZEGAR JEST TU CALA TRUDNOSCIA, NIE PAMIEC. millis() zeruje sie przy restarcie, wiec
// znacznik czasu w millisekundach po starcie nie znaczy nic — stad epoch z NTP.
// Ale NTP dochodzi kilka sekund PO inicjalizacji panelu, wiec w chwili wczytywania
// blobu zegara zwykle JESZCZE NIE MA. Dlatego decyzja "kontynuujemy sesje czy nie"
// jest ODLOZONA do pierwszego przebiegu graphTick z waznym zegarem (gRestorePending
// w OledPanel.cpp), zamiast zapasc przy wczytaniu na podstawie czasu, ktorego nikt
// jeszcze nie zna. Bez tego odroczenia kazdy restart konczylby sie tak samo:
// gGraphHighMs == 0 przy starcie znaczy "cisza od zawsze", wiec pierwsza probka
// z moca powyzej progu uznalaby sie za POCZATEK NOWEJ SESJI i wyczyscilaby bufor,
// ktory wlasnie odtworzylismy. Zapis przezylby restart, a i tak nie zobaczylbys go
// na ekranie — i to jest dokladnie ten rodzaj bledu, ktory wyglada na dzialajacy.
struct GraphBlob {
  // Pole wersji jak przy "prof2", "gas2" i "mtr2". Zmiana ukladu MUSI isc razem
  // ze zmiana tej liczby albo klucza — inaczej stary blob wczyta sie jako nowy
  // i wykres pokaze nieprawde bez jednego ostrzezenia (notatka nvs-i-pamiec.md,
  // sekcja "Czego NIE robic").
  uint8_t ver = 1;
  uint8_t cnt = 0;        // ile probek jest w buforze (0..128)
  uint8_t max = 0;        // najwieksza probka sesji — skala pionowa wykresu
  uint8_t charging = 0;   // czy w chwili zapisu sesja trwala

  // CZAS ZEGAROWY ostatniej dopisanej probki (epoch, sekundy). 0 = zapisano, zanim
  // NTP doszedl — wtedy po starcie nie da sie policzyc przerwy i sesji NIE
  // kontynuujemy (bufor zostaje do pokazania, ale nastepne ladowanie zaczyna go
  // od nowa). Bezpieczny kierunek: gorzej doklejic nowe probki do przebiegu sprzed
  // wielu godzin, niz raz pokazac o jedna sesje za malo.
  uint32_t lastEpoch = 0;

  // Probki: kw * 10, przyciete do 0..255 — dokladnie to, co trzyma gGraph.
  uint8_t s[128] = {};
};

// Rozmiar wchodzi do tablicy kNvsBytes w Settings.cpp i musi sie z nia zgadzac.
// Asercja stoi TUTAJ, przy strukturze, zeby kompilacja padla w miejscu zmiany,
// a nie w tablicy oddalonej o plik — ta sama zasada, co przy pozostalych blobach.
static_assert(sizeof(GraphBlob) == 136,
              "GraphBlob zmienil rozmiar — popraw kNvsBytes[NVS_SLOT_GRAPH] "
              "w Settings.cpp I PODNIES pole ver, inaczej stary blob wczyta sie "
              "jako nowy i wykres pokaze nieprawde");
