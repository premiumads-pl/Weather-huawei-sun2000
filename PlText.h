#pragma once

#include <TFT_eSPI.h>
#include <pgmspace.h>

#include "PlFont14.h"
#include "PlFont18.h"

namespace pltxt {

struct FontSet {
  const uint8_t* bitmaps;
  const GFXglyph* glyphs;
  const uint16_t* codepoints;
  uint8_t count;
  int ascent;
};

inline FontSet font14() {
  return {PlFont14Bitmaps, PlFont14Glyphs, PlFont14Codepoints, PlFont14Count, PlFont14Ascent};
}

inline FontSet font18() {
  return {PlFont18Bitmaps, PlFont18Glyphs, PlFont18Codepoints, PlFont18Count, PlFont18Ascent};
}

inline int decodeUtf8(const char*& p) {
  const uint8_t c = static_cast<uint8_t>(*p++);
  if (c < 0x80) {
    return static_cast<int>(c);
  }
  if ((c & 0xE0) == 0xC0) {
    const uint8_t c2 = static_cast<uint8_t>(*p++);
    return ((c & 0x1F) << 6) | (c2 & 0x3F);
  }
  if ((c & 0xF0) == 0xE0) {
    const uint8_t c2 = static_cast<uint8_t>(*p++);
    const uint8_t c3 = static_cast<uint8_t>(*p++);
    return ((c & 0x0F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);
  }
  return '?';
}

inline int glyphIndex(const FontSet& font, int cp) {
  for (uint8_t i = 0; i < font.count; ++i) {
    if (pgm_read_word(&font.codepoints[i]) == static_cast<uint16_t>(cp)) {
      return i;
    }
  }
  return -1;
}

inline void drawGlyph(TFT_eSprite& s, const FontSet& font, int x, int baseline, int idx,
                      uint16_t color, uint16_t bg) {
  if (idx < 0) {
    return;
  }
  GFXglyph glyph;
  memcpy_P(&glyph, &font.glyphs[idx], sizeof(GFXglyph));
  const uint8_t* bitmap = font.bitmaps + glyph.bitmapOffset;
  const uint8_t w = glyph.width;
  const uint8_t h = glyph.height;
  const uint8_t rowBytes = (w + 7) / 8;
  const int top = baseline + glyph.yOffset;
  for (uint8_t yy = 0; yy < h; ++yy) {
    for (uint8_t xx = 0; xx < w; ++xx) {
      const uint8_t bit =
          pgm_read_byte(&bitmap[yy * rowBytes + (xx >> 3)]) & (0x80 >> (xx & 7));
      const int px = x + glyph.xOffset + xx;
      const int py = top + yy;
      if (bit) {
        s.drawPixel(px, py, color);
      } else if (bg != color) {
        s.drawPixel(px, py, bg);
      }
    }
  }
}

// y = linia bazowa (dolna krawedz liter, standard GFX).
inline int drawString(TFT_eSprite& s, const FontSet& font, const char* text, int x, int y,
                      uint16_t color, uint16_t bg) {
  const int baseline = y;
  int cx = x;
  const char* p = text;
  while (*p) {
    const int cp = decodeUtf8(p);
    const int idx = glyphIndex(font, cp);
    if (idx < 0) {
      continue;
    }
    GFXglyph glyph;
    memcpy_P(&glyph, &font.glyphs[idx], sizeof(GFXglyph));
    drawGlyph(s, font, cx, baseline, idx, color, bg);
    cx += glyph.xAdvance;
  }
  return cx - x;
}

inline int stringWidth(const FontSet& font, const char* text) {
  int w = 0;
  const char* p = text;
  while (*p) {
    const int cp = decodeUtf8(p);
    const int idx = glyphIndex(font, cp);
    if (idx < 0) {
      continue;
    }
    GFXglyph glyph;
    memcpy_P(&glyph, &font.glyphs[idx], sizeof(GFXglyph));
    w += glyph.xAdvance;
  }
  return w;
}

inline int drawString(TFT_eSprite& s, const char* text, int x, int y, uint16_t color,
                      uint16_t bg) {
  return drawString(s, font14(), text, x, y, color, bg);
}

inline int drawStringLg(TFT_eSprite& s, const char* text, int x, int y, uint16_t color,
                        uint16_t bg) {
  return drawString(s, font18(), text, x, y, color, bg);
}

inline int stringWidth(const char* text) {
  return stringWidth(font14(), text);
}

inline int drawStringRight(TFT_eSprite& s, const FontSet& font, const char* text, int right, int y,
                           uint16_t color, uint16_t bg) {
  const int w = stringWidth(font, text);
  return drawString(s, font, text, right - w, y, color, bg);
}

// Etykiety wykresu — font GLCD (rowna linia bazowa, ASCII).
inline void drawChartLabel(TFT_eSprite& s, const char* text, int x, int y, uint16_t color,
                           uint16_t bg) {
  s.setTextSize(1);
  s.setTextColor(color, bg);
  s.drawString(text, x, y);
}

inline int chartLabelWidth(TFT_eSprite& s, const char* text) {
  s.setTextSize(1);
  return s.textWidth(text);
}

}  // namespace pltxt