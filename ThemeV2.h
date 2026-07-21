#pragma once

#include <TFT_eSPI.h>

#include <cstddef>
#include <cstdint>

#include "AirData.h"
#include "Colors.h"  // C565() — ta sama konwersja RGB888->RGB565, co paleta V1 (col::)

// Prymitywy wygladu V2 ("SCENA") — v119. Retro 8-bit, spojny z VIEW_RETRO
// (patrz WeatherUi.cpp, sekcja "WIDOK 0: RETRO"), ale WLASNA paleta i WLASNY
// uklad (patrz cfg::V2_* w Config.h). Uzywane przez WeatherUi::paintFrame/
// drawView (HUD, pasek segmentow) i przez kazdy drawView*V2 (tytul, karty, tlo).
//
// Ten plik NIE zna WeatherUi (poza jednym wyjatkiem: hudSegments wola publiczna
// statyczna WeatherUi::viewSkipped z WeatherUi.cpp, zeby "pomijany ekran" znaczyl
// DOKLADNIE to samo na pasku V1 i V2 — patrz komentarz przy tej metodzie w
// WeatherUi.h). Poza tym prymitywy sa czystymi funkcjami: dostaja gotowe stringi
// i liczby, nic same nie licza z modeli danych — to zostaje po stronie
// WeatherUi.cpp (drawView*V2), tak jak V1 trzyma logike w drawView*, a nie w
// plStr/gl/bigStr.
namespace themev2 {

// Paleta z mockupu projektanta (RGB888 -> RGB565 przez C565() z Colors.h,
// dokladnie jak przy col:: w Colors.h). Trzecia, niezalezna paleta obok col::
// (V1) i rcol:: (RETRO, w WeatherUi.cpp) — mieszanie ktorejkolwiek pary
// zacieraloby granice miedzy trzema stylami ekranow.
namespace col2 {
constexpr uint16_t BG       = C565(10, 14, 26);    // tlo ekranu / HUD
constexpr uint16_t CARD     = C565(22, 32, 58);    // tlo karty
constexpr uint16_t CARD_TOP = C565(42, 58, 90);    // jasny pasek 2px u gory karty
constexpr uint16_t TEXT     = C565(248, 248, 248); // tekst glowny (liczby)
constexpr uint16_t LABEL    = C565(240, 192, 32);  // etykiety / nazwy
constexpr uint16_t VALUE2   = C565(64, 208, 224);  // wartosci dodatkowe
constexpr uint16_t WARN     = C565(255, 144, 64);  // ostrzezenie
constexpr uint16_t DIM      = C565(120, 140, 170); // tekst przygaszony
}  // namespace col2

// ---- tekst RetroFontu -----------------------------------------------------
// WeatherUi.cpp ma juz identyczna trojke (retroChar/retroStr/retroStrShadowed)
// w retroSection RETRO — CELOWO nie wolamy jej stad. Zadanie zabrania ruszac
// drawViewRetro, a te trzy funkcje siedza w jego anonimowej przestrzeni nazw w
// TYM SAMYM pliku; zeby ich uzyc, trzeba by dotknac WeatherUi.cpp w miejscu,
// ktorego nie wolno ruszac. Kopia jest krotka (dane fontu — retrofont::GLYPHS —
// zostaja WSPOLNE, w RetroFont.h; duplikuje sie tylko petla rysujaca), a koszt
// to kilkaset bajtow flasha, nie warte ryzyka.

// Szerokosc napisu w RetroFoncie: KAZDY znak (nawet spoza zestawu — patrz
// retrofont::FIRST_CHARS) przesuwa kursor o 9*scale (8 px glifu + 1 px odstepu),
// wiec szerokosc to zwykla arytmetyka, bez petli po tablicy glifow.
int textWidth(const char* s, int scale);

void text(TFT_eSPI& spr, int x, int y, int scale, uint16_t color, const char* s);

// Cien: czarna kopia przesunieta o `scale` w prawo/w dol, rysowana PRZED
// wlasciwym tekstem — bez tego jasne litery gina na gradiencie nieba (patrz
// sceneBackground). Ten sam trik co retroStrShadowed w RETRO.
void textShadowed(TFT_eSPI& spr, int x, int y, int scale, uint16_t color, const char* s);

// UTF-8 (polskie znaki, dowolna wielkosc liter) -> WIELKIE ASCII z zestawu,
// ktory RetroFont potrafi narysowac (patrz retrofont::FIRST_CHARS — brak
// malych liter i polskich znakow). Potrzebne dla tekstu spoza naszej kontroli:
// nazwa miasta i nazwy pokoi BLE sa wolnym tekstem z panelu WWW. Niezalezna
// kopia retroAscii() z WeatherUi.cpp z tego samego powodu co wyzej.
void foldAscii(char* dst, size_t dstSize, const char* src);

// ---- struktura ekranu V2 --------------------------------------------------

// HUD gorny, y=0..27 (cfg::V2_HUD_H): miasto (bialo, lewo), data (zolto,
// srodek), godzina (cyan, prawo). `city` to surowy UTF-8 z ustawien —
// hudTop() sam go fałduje przez foldAscii(). Data/godzina czytane z
// time(nullptr) TU, w srodku (jak WeatherUi::drawHeader/drawViewRetro) — to
// zegar scienny, nie stan animacji, wiec nie idzie przez nowMs (patrz
// uzasadnienie w tamtych funkcjach: kosmetyczny rozjazd o sekunde miedzy
// paskami zrzutu nikomu nie szkodzi, w odroznieniu od millis()).
void hudTop(TFT_eSPI& spr, int ox, const char* city);

// Pasek segmentow rotacji, y=29..32 (cfg::V2_SEG_Y/V2_SEG_H — TA SAMA
// geometria co pasek V1 cfg::PROG_Y/PROG_H, inny styl). Jeden segment na
// ekran (`viewCount` = cfg::VIEW_COUNT): aktywny wypelnia sie na `frac`
// (0..1, policzone przez wolajacego jak w WeatherUi::drawProgress), odwiedzone
// swieca stalym jasnym tonem, pomijane (WeatherUi::viewSkipped) dostaja
// przygaszony pomaranczowy zamiast neutralnego toru. `air` idzie DALEJ do
// viewSkipped — patrz komentarz przy tej metodzie w WeatherUi.h.
void hudSegments(TFT_eSPI& spr, int ox, int curView, int viewCount, float frac,
                 const AirModel* air);

// Wiersz tytulu, y=36..51 (cfg::V2_TITLE_Y/V2_TITLE_H): nazwa ekranu (zolto,
// skala 2), obok opis/licznik (bialo, skala 1), przy prawej krawedzi maly tag
// (przygaszony, skala 1). `info`/`tag` moga byc nullptr albo "" — wtedy sie
// nie rysuja. `info` NIE rysuje sie w ogole, gdy nie miesci sie miedzy
// tytulem a tagiem (zamiast na sile scinac) — tytul i tag maja pierwszenstwo.
void titleRow(TFT_eSPI& spr, int ox, const char* name, const char* info = nullptr,
             const char* tag = nullptr);

// Karta danych — podstawowy budulec tresci. Prostokat w kolorze tla karty z
// jasnym paskiem 2px na gornej krawedzi; etykieta zolta (skala 2), wartosc
// glowna (skala 3, KOLOR wg `valueColor` — domyslnie biel z mockupu, ale np.
// temperatura chce wlasnej skali barw jak w V1), wartosc dodatkowa cyan
// (skala 2, opcjonalna — nullptr/"" pomija ja), `warn` zmienia jej kolor na
// pomaranczowy. `value`, gdy nie miesci sie w karcie przy skali 3, SAM schodzi
// na skale 2 (ten sam wzorzec "najpierw mniejszy font, dopiero potem tnij" co
// w WeatherUi::drawNetInfo dla SSID) — wiec szerokosc karty nie musi byc
// idealnie wymierzona pod kazda mozliwa wartosc.
void card(TFT_eSPI& spr, int x, int y, int w, int h, const char* label, const char* value,
         const char* value2 = nullptr, bool warn = false, uint16_t valueColor = col2::TEXT);

// Sylwetka (dom/miasto/panel PV...) jako lista prostokatow, wspolrzedne
// WZGLEDEM `ox` (jak wszystko inne w drawView*) — sceneBackground dodaje ox
// sama, wolajacy podaje x=0 dla lewej krawedzi ekranu.
struct SceneRect {
  int16_t x, y, w, h;
};

// Tlo sceniczne — dla ekranow "obrazkowych" (TERAZ, W DOMU, SAMOLOTY, RADAR,
// 5 DNI, GODZINY, FOTOWOLTAIKA, patrz BRIEF-PROJEKTANT-UI.md). Niebo w pasach
// od ciemnego granatu (u gory) do jasniejszego tonu z palety (przy horyzoncie),
// KWANTYZOWANE (v & 0xF0 na kazdym kanale) — to CELOWY "retro banding" rodem z
// 8-bit grafiki (dokladnie ten sam trik co niebo w WeatherUi::drawViewRetro),
// NIE oszczednosc i nie blad zaokraglenia. Pod linia horyzontu plaski "grunt"
// w kolorze karty, na nim opcjonalna ciemna sylwetka. Rysuje sie PRZED kartami
// tresci — widoczna tam, gdzie karty jej nie zaslaniaja.
void sceneBackground(TFT_eSPI& spr, int ox, int top, int bottom, int horizonY,
                     const SceneRect* silhouette = nullptr, int nSilhouette = 0);

// Tlo plaskie — dla ekranow gestych od danych (PIEC, POWIETRZE, PAMIEC, RUCH,
// STATYSTYKI): jednolity kolor, zero nieba. Same karty niosa tresc.
void flatBackground(TFT_eSPI& spr, int ox, int top, int bottom);

}  // namespace themev2
