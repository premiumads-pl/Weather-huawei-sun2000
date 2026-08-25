#pragma once

#include <cstdint>

#include "AutoData.h"

// Panel OLED 0,96" (SSD1306/SSD1315, 128x64, I2C) z czterema przyciskami — WYBOR
// TRYBU LADOWANIA AUTA. To DRUGIE URZADZENIE na tej samej plytce, a nie nowy widok
// glownego ekranu: ma wlasny sterownik, wlasny rytm rysowania i wlasne przyciski.
// Numerow widokow (cfg::VIEW_*) nie dotyka i dotykac nie ma po co.
//
// TRZY RZECZY, KTORE RZADZA CALYM TYM MODULEM
// -------------------------------------------
// 1. WLASNEGO BUFORA OBRAZU NIE MA. Pelna klatka SSD1306 to 128 x 64 / 8 = 1024 B,
//    a do bariery statycznego RAM-u (76 000 B, tools/release.sh) zostalo po v174
//    1816 B. Trzymanie tego kilobajta zjadloby 56% calego zapasu wydania.
//    Dlatego jedziemy w TRYBIE STRONICOWYM: bufor to JEDNA strona, 128 B, i osiem
//    przebiegow sklada pelna klatke. Z tego samego powodu odpada U8g2 w trybie
//    pelnego bufora i Adafruit_SSD1306 — obie alokuja ten kilobajt statycznie.
// 2. RYSOWANIE NIE MOZE ZATRZYMAC GLOWNEGO EKRANU. Cala klatka to 1 kB po I2C,
//    czyli przy 400 kHz okolo 25 ms — POLOWA klatki ST7789. Dlatego step() robi
//    najwyzej JEDNA strone na obieg petli (~3 ms) i TYLKO wtedy, gdy tresc sie
//    zmienila. Osiem kolejnych obiegow sklada nowa klatke; przy braku zmian koszt
//    to sam odczyt czterech pinow.
// 3. PANEL NIE UDAJE, ZE COS USTAWIL. Kropka "tryb aktywny" pochodzi WYLACZNIE
//    z pola `tryb` w <prefix>/auto/stan. Zatwierdzenie publikuje polecenie i czeka;
//    bez potwierdzenia w cfg::OLED_CONFIRM_MS menu pisze o tym wprost.
namespace oled {

// Wykrywa modul po I2C (ACK pod 0x3C, potem 0x3D) i konfiguruje przyciski.
// BRAK MODULU NIE JEST BLEDEM: wlasciciel wgra firmware, zanim cokolwiek podlaczy.
// Nieudane wykrycie WYLACZA panel na cale zycie programu — step() wychodzi wtedy
// pierwsza linia, wiec nie ma ani ponawiania co petle, ani wiszacych transakcji I2C.
// Slad zostaje w dzienniku i w /api/diag (sekcja "oled").
void begin();

// Jeden obieg: odczyt przyciskow, obsluga stanu, najwyzej JEDNA strona obrazu.
// Wolac z petli rysowania, po zlozeniu klatki glownego ekranu.
void step(const AutoModel& a, uint32_t now);

// --- podglad dla /api/diag (zero nowych pol w Diag, wiec zero bajtow w .bss) ---
bool present();            // czy modul odpowiedzial przy starcie
uint8_t address();         // 0x3C / 0x3D / 0 gdy nie ma
const char* screenName();  // "spoczynek" / "menu" / "test"
uint32_t pagesSent();      // ile stron poszlo na I2C od uruchomienia
uint32_t i2cErrors();      // ile transakcji NIE doszlo (urwany przewod, zly styk)
uint32_t lastStepUs();     // ile trwal ostatni obieg step() [us]
uint8_t buttonMask();      // bity 0..3 = K1..K4 wcisniete (po debounce)
const char* sentMode();    // ostatnio WYSLANY tryb ("" = nic nie wysylano)

}  // namespace oled
