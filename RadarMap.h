#pragma once

#include <cstddef>
#include <cstdint>

#include "MapDataRadar.h"

// Animowana mapa opadow nad Zatoka Gdanska — 2 godziny wstecz, co 10 minut.
//
// RainViewer daje 13 klatek co 10 min (dokladnie 2 h wstecz) i — w darmowym API —
// ZERO klatek prognozy (sprawdzone: "nowcast": []). Wiec to, co pokazujemy, jest
// POMIAREM, nie przewidywaniem. Kierunek i predkosc frontu widac z samego ruchu.
// Do v109 bralismy co DRUGA klatke (7 z 13, co 20 min) — front przeskakiwal 48 px
// naraz (15% szerokosci ekranu) i wygladal na "latajacy". Teraz bierzemy KAZDA
// klatke, jaka RainViewer oddaje: sam skok bazowy spada o polowe, a reszte
// plynnosci dorzuca interpolacja wektorem wiatru w WeatherUi::drawViewRadar
// (potrzebuje gestszych klatek, zeby miec miedzy czym "dojezdzac").
//
// Geometria (v110, POSZERZENIE Z 111 NA ~300 KM — sprostowanie: ten komentarz
// do niedawna mowil o jednym kaflu zoom 7, co juz NIE JEST prawda): mapa to dzis
// gmapr (300 km szerokosci, LAT 53.8457-55.2943, LON 16.2756-20.9244), nie gmapw
// (ta zostaje wylacznie dla ekranu samolotow — patrz MapDataWide.h). Przy takiej
// rozpietosci okno NIE miesci sie w jednym kaflu RainViewera nawet na zoomie 6:
// pokrywa DWA kafle w poziomie i jeden w pionie (patrz RadarMap.cpp,
// computeGeometry() — zakres liczony DYNAMICZNIE z granic gmapr, nie zaszyty).
// Zoom 6 (nie 7): przy zoom 7 to samo okno siegaloby TRZECH kafli w poziomie
// (sprawdzone numerycznie), zoom 6 starcza na dwa — mniej pobran, a rozdzielczosc
// kafla i tak jest grubsza od potrzebnej (radar ma piksele grubsze niz nasza mapa).
// Zoom > 7 nie dziala: serwer zwraca obrazek "Zoom Level Not Supported".
//
// Pamiec: 13 klatek x 320x172 B (=55 040 B/klatka) = 715 520 B (~715 kB) w PSRAM.
// (Ten komentarz liczyl kiedys "7 klatek po 224x172 B = 270 kB" — 224 to szerokosc
// mapy SPRZED przejscia na szeroka gmapw; W ponizej to od dawna 320 (dzis z gmapr,
// tej samej wielkosci co gmapw), wiec przy okazji 7->13 liczba wraca do prawdy.)
// Do v50 nie do pomyslenia; dzis siedzi w PSRAM i nawet tego nie czuc.

namespace radarmap {

// -120, -110, -100, -90, -80, -70, -60, -50, -40, -30, -20, -10, 0 min (co 10 min)
constexpr int FRAMES = 13;
constexpr int W = gmapr::MAP_W;
constexpr int H = gmapr::MAP_H;

struct Frame {
  uint32_t epoch = 0;       // czas klatki
  int32_t offsetMin = 0;    // minuty wzgledem "teraz" (ujemne = przeszlosc)
  bool valid = false;
};

bool begin();               // alokacja buforow w PSRAM (raz, w setup())

// Wolane z netTask przy KAZDYM obiegu, PRZED decyzja o pobieraniu klatek.
// Gdy bufory stoja — jeden odczyt bool i wyjscie, wiec kosztuje tyle co nic.
// Gdy nie weszly przy starcie, ponawia alokacje w tle: pierwsza proba minute po
// nieudanym begin(), potem odstep podwaja sie do 10 minut, przez 20 prob lacznie.
// Do v145 nieudany begin() skazywal cala sesje na tryb zastepczy "pomiar punktowy"
// az do recznego restartu — a alokacja padala zwykle nie z braku PSRAM, tylko przez
// chwilowe zajecie/fragmentacje w trakcie rozruchu. Po udanej probie sam ustawia
// wantsFetch(), zeby mapa nie czekala pusta do najblizszego cyklu (10 min).
bool ensureReady();

bool fetch();               // pobiera komplet klatek (wolane z netTask)

int count();                             // ile klatek jest gotowych
const Frame& frame(int i);
uint8_t levelAt(int i, int x, int y);    // 0 = brak opadu, 1..5 rosnaco

// Caly raster klatki `i` na raz: W*H bajtow poziomu, nullptr gdy klatki nie ma.
// Po co obok levelAt(): petla rysujaca opad robi do ~110 tys. odczytow na klatke
// przy budzecie 21 ms, a levelAt() to wywolanie do innej jednostki kompilacji,
// ktore przy KAZDYM pikselu od nowa sprawdza indeks klatki i wskaznik bufora.
// Model ekranu (RadarData.h) bierze ten wskaznik RAZ i indeksuje tablice sam.
//
// DLACZEGO WOLNO CZYTAC TO BEZ MUTEXA (rdzen 1 rysuje, rdzen 0 pobiera i alokuje):
// bo raz opublikowany wskaznik bufora NIE JEST ZWALNIANY NIGDY — nie ma w module
// zadnej sciezki, ktora by to robila. Alokacja (RadarMap.cpp, allocateBuffers())
// pracuje na wskaznikach LOKALNYCH i podstawia do gFrames/gTile dopiero KOMPLET,
// pod gMx; nieudana proba zwalnia wylacznie to, co sama zaalokowala, i nie dotyka
// tego, co juz opublikowane. Zmiana jest wiec zawsze jednokierunkowa i jednorazowa:
// nullptr -> wazny adres, i tam zostaje do konca sesji. Gorzej niz "nieaktualny"
// ten wskaznik byc nie moze, a nieaktualny nullptr rysowanie i tak obsluguje
// (RadarViewModel::levelAt zwraca 0).
// Uwaga: ponawianie alokacji w tle (ensureReady()) tej gwarancji NIE oslabia —
// przeciwnie, to wlasnie ono wymusilo powyzsza dyscypline. Wczesniej powod byl
// slabszy ("bufory alokuje raz begin(), a zwalnia je releaseAll() na sciezce
// nieudanego startu") i przestalby obowiazywac przy pierwszej probie ponowienia.
const uint8_t* raster(int i);
uint32_t updatedAt();                    // millis() ostatniego udanego pobrania
const char* lastError();

// --- stan alokacji buforow: do /api/diag ("radar_map") i sekcji "Zdrowie urzadzenia" ---
// Czy stoi KOMPLET buforow, czyli czy animowana mapa w ogole dziala. Zalozyc je moze
// begin() przy starcie ALBO pozniejsze ensureReady() — raz ustawione true juz nie
// wraca na false, bo opublikowanych buforow nic nie zwalnia (patrz raster() wyzej).
// To NIE jest to samo, co count() > 0: count() mowi tylko, czy DOSZLY dane — jest 0
// takze przez pierwsze sekundy po udanym starcie, zanim przejdzie pierwszy fetch().
// Odwrotnie tez: przy nieudanej alokacji count() zostaje 0, dopoki bufory nie wejda,
// i z samego count() nie odroznisz "jeszcze nie pobrano" od "nie ma z czego rysowac".
// Panel do tej pory zgadywal stan mapy z ESP.getPsramSize() > 0 i przez to klamal
// dokladnie w jedynym ciekawym przypadku: PSRAM jest, a alokacja i tak padla.
bool ready();

// Ile razy probowano zaalokowac bufory — liczy sie takze proba przerwana od razu na
// !psramFound(). Nie jest to juz zawsze 1: po nieudanym starcie ensureReady() ponawia
// alokacje w tle, wiec liczba mowi, ile podejsc bylo naprawde. Razem z next_try_s
// w /api/diag daje pelny obraz: ile razy juz probowano i kiedy kolejna proba.
int allocTries();

// Ile sekund zostalo do kolejnej proby alokacji. 0 = bufory stoja (nie ma czego
// probowac) albo termin wlasnie minal i proba pojdzie w najblizszym obiegu netTask.
// -1 = juz nie probujemy: albo na pokladzie nie ma PSRAM, albo wyczerpal sie limit
// prob i pomoze dopiero restart. Wylacznie do diagnostyki (/api/diag, panel).
int32_t nextTrySec();

// Ile bajtow PSRAM trzymaja bufory (klatki + kafelek roboczy); 0 gdy !ready().
size_t bufferBytes();

// Czy w KTOREJKOLWIEK klatce jest opad. Ekran radaru bez deszczu to pusta mapa,
// wiec rotacja go wtedy pomija — a pasek postepu zaznacza go innym kolorem.
bool hasRain();

// Symulacja: sztuczny front przechodzacy z zachodu na wschod. Do obejrzenia,
// jak wyglada wizualizacja, gdy akurat nie pada. Wlaczana z panelu.
void setDemo(bool on);
bool demo();

// Po wylaczeniu symulacji trzeba NATYCHMIAST sciagnac prawdziwe klatki — inaczej
// ekran wisi na "Pobieram mapę opadów" az do najblizszego cyklu (10 minut).
bool wantsFetch();

}  // namespace radarmap
