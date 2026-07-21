#include "ThemeV3.h"

namespace tv3 {

using namespace grid;

// ---------------------------------------------------------------- swiezosc ----
void freshDot(TFT_eSPI& s, int cx, int cy, Fresh f) {
  switch (f) {
    case Fresh::OK:
      s.fillCircle(cx, cy, 3, col::OK);          // pelna kropka 7 px (r=3 => 7 px)
      break;
    case Fresh::STALE:
      s.drawCircle(cx, cy, 3, col::WARN);        // pusta kropka bursztynowa
      s.drawCircle(cx, cy, 2, col::WARN);
      break;
    case Fresh::UNKNOWN:
      s.drawFastHLine(cx - 3, cy, 7, col::MUTE); // myslnik, nigdy zero/kropka
      break;
  }
}

// -------------------------------------------------------------------- tlo ------
void sceneBg(TFT_eSPI& spr) {
  spr.fillRect(0, 0, W, H, col::BG);                 // cala jasna plaszczyzna
  spr.fillRect(0, 0, CTX_W, H, col::PANEL);          // ciemna kolumna kontekstu
}

void sceneBottom(TFT_eSPI& tft) {
  // Dolny pas 206..239 (poza sprite): lewa czesc ciemna, prawa jasna — spojnie z gora.
  tft.fillRect(0, 206, CTX_W, H - 206, col::PANEL);
  tft.fillRect(CTX_W, 206, W - CTX_W, H - 206, col::BG);
}

void moduleSep(TFT_eSPI& s, int y) {
  s.drawFastHLine(DATA_X + MARGIN, y, DATA_W - 2 * MARGIN, col::LINE);
}

int moduleLabel(TFT_eSPI& s, const char* label, int y) {
  // Plex11 wersalikami; y to linia bazowa etykiety.
  return DATA_L + plex::str(s, plex::f11(), label, DATA_L, y, col::SECOND);
}

// ------------------------------------------------------------- wskazniki -------
void bar(TFT_eSPI& s, int x, int y, int w, int h, float frac, uint16_t fill, uint16_t track) {
  if (frac < 0) frac = 0; if (frac > 1) frac = 1;
  s.fillRect(x, y, w, h, track);
  const int fw = static_cast<int>(frac * w + 0.5f);
  if (fw > 0) s.fillRect(x, y, fw, h, fill);
}

void scale5(TFT_eSPI& s, int x, int y, int w, int h, float pos01) {
  // Piec segmentow: zielony, zielono-zolty, zolty, pomaranczowy, czerwony.
  static const uint16_t seg[5] = {0x4CC9, 0x9CC9, col::PV, 0xEC00, col::GRID};
  const int sw = w / 5;
  for (int i = 0; i < 5; ++i) {
    s.fillRect(x + i * sw, y, (i == 4 ? w - 4 * sw : sw), h, seg[i]);
  }
  if (pos01 < 0) pos01 = 0; if (pos01 > 1) pos01 = 1;
  const int mx = x + static_cast<int>(pos01 * w + 0.5f);
  // Pionowy znacznik: ciemny slupek z jasna obwodka, czytelny na kazdym segmencie.
  s.fillRect(mx - 1, y - 2, 3, h + 4, col::PANEL);
  s.drawFastVLine(mx - 2, y - 2, h + 4, col::BG);
  s.drawFastVLine(mx + 2, y - 2, h + 4, col::BG);
}

// ============================================================= GLIFY POGODY ====
namespace wx {

namespace {
// Kategorie po WMO — jeden przelacznik na caly projekt.
enum Cat { CLEAR, PARTLY, CLOUDY, FOG, RAIN, SNOW, STORM };
Cat categorize(int wmo) {
  if (wmo <= 0) return CLEAR;
  if (wmo == 1 || wmo == 2) return PARTLY;
  if (wmo == 3) return CLOUDY;
  if (wmo == 45 || wmo == 48) return FOG;
  if (wmo >= 95) return STORM;
  if ((wmo >= 71 && wmo <= 77) || wmo == 85 || wmo == 86) return SNOW;
  return RAIN;  // mzawka, deszcz, przelotny — 51..67, 80..82
}

// Chmura z trzech kol + plaskiego spodu. cx,cy = srodek chmury, s = skala (~promien).
void cloud(TFT_eSPI& g, int cx, int cy, int s, uint16_t body, uint16_t edge) {
  const int r = s;
  g.fillCircle(cx - r, cy, r * 55 / 100, body);
  g.fillCircle(cx + r, cy, r * 62 / 100, body);
  g.fillCircle(cx, cy - r * 45 / 100, r * 80 / 100, body);
  g.fillRect(cx - r - r * 55 / 100, cy, 2 * r + r * 117 / 100, r * 62 / 100 + 1, body);
  (void)edge;
}

// Slonce: tarcza + promienie. Albo ksiezyc: sierp (tarcza minus przesunieta tarcza tla).
void sunOrMoon(TFT_eSPI& g, int cx, int cy, int r, bool night, uint16_t bg) {
  if (night) {
    g.fillCircle(cx, cy, r, col::MUTE);
    g.fillCircle(cx + r * 45 / 100, cy - r * 35 / 100, r, bg);  // wycina sierp
  } else {
    for (int a = 0; a < 8; ++a) {
      const float ang = a * 3.14159f / 4.f;
      const int x0 = cx + static_cast<int>((r + 2) * cosf(ang));
      const int y0 = cy + static_cast<int>((r + 2) * sinf(ang));
      const int x1 = cx + static_cast<int>((r + r * 70 / 100 + 3) * cosf(ang));
      const int y1 = cy + static_cast<int>((r + r * 70 / 100 + 3) * sinf(ang));
      g.drawLine(x0, y0, x1, y1, col::SUN);
    }
    g.fillCircle(cx, cy, r, col::SUN);
  }
}
}  // namespace

void glyph(TFT_eSPI& s, int wmoCode, bool night, int cx, int cy, int r, uint16_t onLight) {
  const uint16_t bg = onLight ? col::BG : col::PANEL;
  const uint16_t body = onLight ? col::SECOND : col::ONDARK_DIM;  // chmura czytelna na obu tlach
  const Cat c = categorize(wmoCode);
  const int sr = r;  // skala

  auto drops = [&](uint16_t cc) {
    for (int i = -1; i <= 1; ++i) {
      const int x = cx + i * sr * 55 / 100;
      const int y = cy + sr * 70 / 100;
      s.drawLine(x, y, x - sr * 20 / 100, y + sr * 55 / 100, cc);
      s.drawLine(x + 1, y, x + 1 - sr * 20 / 100, y + sr * 55 / 100, cc);
    }
  };
  auto flakes = [&](uint16_t cc) {
    for (int i = -1; i <= 1; ++i) {
      const int x = cx + i * sr * 55 / 100;
      const int y = cy + sr * 90 / 100;
      s.drawFastHLine(x - 2, y, 5, cc);
      s.drawFastVLine(x, y - 2, 5, cc);
      s.drawPixel(x - 1, y - 1, cc); s.drawPixel(x + 1, y + 1, cc);
      s.drawPixel(x + 1, y - 1, cc); s.drawPixel(x - 1, y + 1, cc);
    }
  };

  switch (c) {
    case CLEAR:
      sunOrMoon(s, cx, cy, sr * 80 / 100, night, bg);
      break;
    case PARTLY:
      sunOrMoon(s, cx + sr * 45 / 100, cy - sr * 45 / 100, sr * 55 / 100, night, bg);
      cloud(s, cx - sr * 10 / 100, cy + sr * 20 / 100, sr * 62 / 100, body, body);
      break;
    case CLOUDY:
      cloud(s, cx - sr * 20 / 100, cy - sr * 10 / 100, sr * 55 / 100, col::MUTE, col::MUTE);
      cloud(s, cx + sr * 10 / 100, cy + sr * 15 / 100, sr * 70 / 100, body, body);
      break;
    case FOG:
      cloud(s, cx, cy - sr * 25 / 100, sr * 62 / 100, body, body);
      for (int i = 0; i < 3; ++i)
        s.drawFastHLine(cx - sr, cy + sr * 40 / 100 + i * (sr * 30 / 100), 2 * sr, col::MUTE);
      break;
    case RAIN:
      cloud(s, cx, cy - sr * 20 / 100, sr * 70 / 100, body, body);
      drops(col::RAIN);
      break;
    case SNOW:
      cloud(s, cx, cy - sr * 20 / 100, sr * 70 / 100, body, body);
      flakes(onLight ? col::SECOND : col::ONDARK);
      break;
    case STORM:
      cloud(s, cx, cy - sr * 20 / 100, sr * 70 / 100, col::MUTE, col::MUTE);
      // blyskawica
      const int bx = cx, by = cy + sr * 35 / 100;
      s.fillTriangle(bx, by, bx - sr * 30 / 100, by + sr * 55 / 100,
                     bx + sr * 8 / 100, by + sr * 40 / 100, col::SUN);
      s.fillTriangle(bx + sr * 8 / 100, by + sr * 40 / 100, bx + sr * 30 / 100, by + sr * 30 / 100,
                     bx - sr * 5 / 100, by + sr * 95 / 100, col::SUN);
      break;
  }
}

}  // namespace wx
}  // namespace tv3
