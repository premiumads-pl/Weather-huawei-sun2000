#pragma once

#include <cstdint>

// Dotyk pojemnosciowy na GPIO7 (kanal TOUCH7). Plytka nie ma zadnego przycisku,
// wiec wystarczy przylozyc palec do pinu albo doprowadzic z niego kawalek folii
// czy sruby — pojemnosc ciala wystarczy.
//
// UWAGA: w ESP32-S3 (inaczej niz w pierwszym ESP32) odczyt przy dotknieciu
// ROSNIE, a nie maleje. Progu nie da sie zaszyc na sztywno — zalezy od dlugosci
// przewodu i wilgotnosci — wiec liczymy go od linii bazowej, ktora dodatkowo
// powoli dryfuje razem z otoczeniem.

namespace touch {

void begin();

// Wolane co klatke. Zwraca true DOKLADNIE RAZ na dotkniecie (zbocze narastajace).
bool pressed();

// Do diagnostyki: surowy odczyt i aktualna linia bazowa.
uint32_t raw();
uint32_t baseline();

}  // namespace touch
