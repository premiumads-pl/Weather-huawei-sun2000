#pragma once

// ============================================================================
//  MOTYW V3 "PASMOWY" — prymitywy wspolne dla wszystkich ekranow
// ============================================================================
// Projekt "Pasmowy" (docs/design-v3/): jasne tlo, uklad DWUKOLUMNOWY — ciemna
// kolumna kontekstu po lewej (zegar/data/temperatura/glif pogody) + jasna kolumna
// danych po prawej (do 3 modulow rozdzielanych cienka linia, nigdy ramkami).
//
// Ten plik to JEDYNE zrodlo palety, siatki i prymitywow V3. Ekrany (WeatherUiV3.cpp)
// nie znaja hexow ani wspolrzednych kolumn — sklejaja gotowe klocki stad. Dzieki
// temu zmiana odstepu czy koloru to jedna linijka tutaj, a nie osiem ekranow.
//
// WAZNE — GEOMETRIA: V3 rysuje PELNE 320x240. Kolumna danych ma trzy sloty; dolny
// (POWIETRZE na glownym, os na wykresach) siega y=206..239, czyli w strefe, ktora
// V1/V2 oddaja stopce PV. Dlatego render() rysuje dolny pas V3 wprost na TFT (jak
// footer RETRO), a nie w sprite 206 px. Patrz sceneBottom() i WeatherUi::render.

#include <TFT_eSPI.h>

#include "PlexText.h"

namespace tv3 {

// --- PALETA (RGB565, przeliczona ze specyfikacji, plaskie wypelnienia) --------
namespace col {
constexpr uint16_t BG      = 0xF7BE;  // #F4F4F0 tlo jasne
constexpr uint16_t PANEL   = 0x18E3;  // #1A1C1E ciemna kolumna / tekst na jasnym
// KONTRAST: wartosci ponizej sa CIEMNIEJSZE niz w oryginalnej specyfikacji projektu.
// Powod: na fizycznym ST7789 w lazience jasne tlo ma niebieski odcien i myje szarosci
// — etykiety w #6C6F6A (kontrast 4,6) na zywo znikaly. Podbite do #33362F (11,1) /
// #5A5D54 (6,1). Estetyka odrobine mniej "papierowa", ale wlasciciel patrzy z 2 m na
// zaparowany, tani panel: czytelnosc bije elegancje (zasada z briefu).
constexpr uint16_t SECOND  = 0x31A5;  // #33362F etykiety (bylo #6C6F6A) — mocny kontrast
constexpr uint16_t MUTE    = 0x5AEA;  // #5A5D54 wyciszony (bylo #9A9C96)
constexpr uint16_t LINE    = 0xBDF6;  // #BCBFB6 linie rozdzielajace (bylo #D9DCD6) — widoczne
constexpr uint16_t RAIN    = 0x2318;  // #2563C4 opad (najsilniejszy)
constexpr uint16_t RAIN2   = 0x5C7A;  // #5B8DD0
constexpr uint16_t RAIN3   = 0x9DDB;  // #9DB8DD
constexpr uint16_t RAIN4   = 0xBE3B;  // #B8C6DD (najslabszy)
constexpr uint16_t OK      = 0x4CC9;  // #4D9A4D produkcja / stan OK / swieze
constexpr uint16_t SELF    = 0x3BD8;  // #3D78C4 zuzycie wlasne
constexpr uint16_t GRID    = 0xC247;  // #C04A3A z sieci / alarm
constexpr uint16_t SUN     = 0xF587;  // #F2B13A slonce
constexpr uint16_t PV      = 0xE545;  // #E0A92E modulacja / slupki PV
constexpr uint16_t WARN    = 0xBC83;  // #B8901F nieswieze / uwaga
constexpr uint16_t WARNBG  = 0xF738;  // #F3E4C2 tlo plakietki uwagi
constexpr uint16_t ACCENT  = 0x7D5C;  // #7FA8E0 akcent na ciemnym
// Radar
constexpr uint16_t SEA     = 0x08A3;  // #0E141F morze
constexpr uint16_t LAND    = 0x29A8;  // #2B3444 lad
constexpr uint16_t COAST   = 0x5B50;  // #5A6A82 brzeg
constexpr uint16_t BORDER  = 0x532F;  // #57647C granice
// Tekst NA ciemnej kolumnie (jasny)
constexpr uint16_t ONDARK      = 0xF7BE;  // = BG, glowny tekst na panelu
constexpr uint16_t ONDARK_DIM  = 0xAD55;  // przygaszony na ciemnym
}  // namespace col

// --- SIATKA -------------------------------------------------------------------
namespace grid {
constexpr int W = 320, H = 240;
// KOLUMNA KONTEKSTU zwezona ze 136 do 120 px (wlasciciel: "odrobine za szeroka",
// przez co w kolumnie danych nachodzil tekst PV i plakietka POWIETRZE zakrywala
// etykiete). MARGINESY sciete z 12 do 7 — na 320 px kazdy piksel jest cenny, a
// projektowe marginesy zjadaly 2x12=24 px szerokosci. Zwezenie CTX_W dotyka TYLKO
// ekranu glownego/nocnego i planszy alertu (reszta ekranow liczy od MARGIN, nie
// DATA_L), wiec jest bezpieczne. Kolumna danych rosnie ze 160 do 186 px uzytecznych.
constexpr int CTX_W = 120;          // ciemna kolumna kontekstu (bylo 136)
constexpr int DATA_X = CTX_W;       // start jasnej kolumny danych
constexpr int DATA_W = W - CTX_W;   // 200
constexpr int MARGIN = 7;           // margines w kolumnie danych (bylo 12)
constexpr int MARGIN_CTX = 8;       // margines w kolumnie kontekstu (bylo 10)
constexpr int DATA_L = DATA_X + MARGIN;      // lewy tekst danych = 127
constexpr int DATA_R = W - MARGIN;           // prawy tekst danych = 313
}  // namespace grid

// --- SWIEZOSC DANYCH (system trojstanowy ze specyfikacji) ---------------------
enum class Fresh : uint8_t { OK, STALE, UNKNOWN };
// Kropka stanu: pelna zielona (swieze) / pusta bursztynowa (nieswieze) / brak (—).
void freshDot(TFT_eSPI& s, int cx, int cy, Fresh f);

// --- TLO SCENY ----------------------------------------------------------------
// Rysuje jasne tlo + ciemna kolumne kontekstu do y=205 (obszar sprite). NIE rysuje
// dolnego pasa — ten idzie osobno przez sceneBottom() wprost na TFT.
void sceneBg(TFT_eSPI& spr);
// Dolny pas 206..239: dla kolumny kontekstu przedluza ciemne tlo, dla danych jasne.
void sceneBottom(TFT_eSPI& tft);

// Cienka linia rozdzielajaca moduly w kolumnie danych (pelna szerokosc kolumny).
void moduleSep(TFT_eSPI& s, int y);

// Etykieta modulu: WERSALIKI Plex11 w kolorze SECOND, od lewej kolumny danych.
// Zwraca x tuz za etykieta (do dostawienia dopisku typu "za 3 h").
int moduleLabel(TFT_eSPI& s, const char* label, int y);

// --- GLIFY POGODY (prymitywy, 0 kB flasha) ------------------------------------
// Rysowane kolami/prostokatami/liniami, skalowane rozmiarem `r` (promien bazowy).
// Kod pogody = WMO (Open-Meteo). `night` zmienia slonce w ksiezyc.
namespace wx {
// cx,cy = srodek; r = polowa nominalnej wysokosci glifu (np. 28 = ~56 px).
void glyph(TFT_eSPI& s, int wmoCode, bool night, int cx, int cy, int r,
           uint16_t onLight);   // onLight: czy rysowac na jasnym tle (dobor odcieni)
}  // namespace wx

// --- WSKAZNIKI ----------------------------------------------------------------
// Poziomy pasek proporcji (produkcja/pobor itp.): tlo LINE + wypelnienie kolorem.
void bar(TFT_eSPI& s, int x, int y, int w, int h, float frac, uint16_t fill, uint16_t track);
// Skala jakosci: 5 segmentow zielony->czerwony z pionowym znacznikiem pozycji.
void scale5(TFT_eSPI& s, int x, int y, int w, int h, float pos01);

}  // namespace tv3
