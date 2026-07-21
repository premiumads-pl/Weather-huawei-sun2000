#pragma once

// Fonty motywu V3 "Pasmowy" — IBM Plex Sans Condensed (zastepnik: DejaVu Sans
// Condensed, patrz tools/gen_fonts.py). Ten sam format i ta sama sciezka rysowania,
// co PlText.h — tu tylko akcesory do szesciu rozmiarow. Nie duplikujemy logiki:
// drawString/stringWidth itd. bierzemy z pltxt::.

#include "PlText.h"
#include "Plex10.h"
#include "Plex11.h"
#include "Plex13.h"
#include "Plex20.h"
#include "Plex24.h"
#include "Plex52.h"

namespace plex {

inline pltxt::FontSet f10() { return {Plex10Bitmaps, Plex10Glyphs, Plex10Codepoints, Plex10Count, Plex10Ascent}; }
inline pltxt::FontSet f11() { return {Plex11Bitmaps, Plex11Glyphs, Plex11Codepoints, Plex11Count, Plex11Ascent}; }
inline pltxt::FontSet f13() { return {Plex13Bitmaps, Plex13Glyphs, Plex13Codepoints, Plex13Count, Plex13Ascent}; }
inline pltxt::FontSet f20() { return {Plex20Bitmaps, Plex20Glyphs, Plex20Codepoints, Plex20Count, Plex20Ascent}; }
inline pltxt::FontSet f24() { return {Plex24Bitmaps, Plex24Glyphs, Plex24Codepoints, Plex24Count, Plex24Ascent}; }
inline pltxt::FontSet f52() { return {Plex52Bitmaps, Plex52Glyphs, Plex52Codepoints, Plex52Count, Plex52Ascent}; }

// Skroty rysowania na linii bazowej (y = dol liter). Zwracaja szerokosc.
inline int str(TFT_eSPI& s, const pltxt::FontSet& f, const char* t, int x, int baseline,
               uint16_t color) {
  return pltxt::drawString(s, f, t, x, baseline, color, color);  // bg==color => tlo tekstu przezroczyste
}

inline int width(const pltxt::FontSet& f, const char* t) { return pltxt::stringWidth(f, t); }

inline int strRight(TFT_eSPI& s, const pltxt::FontSet& f, const char* t, int right, int baseline,
                    uint16_t color) {
  return pltxt::drawString(s, f, t, right - pltxt::stringWidth(f, t), baseline, color, color);
}

inline int strCenter(TFT_eSPI& s, const pltxt::FontSet& f, const char* t, int cx, int baseline,
                     uint16_t color) {
  return pltxt::drawString(s, f, t, cx - pltxt::stringWidth(f, t) / 2, baseline, color, color);
}

}  // namespace plex
