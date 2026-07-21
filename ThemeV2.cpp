#include "ThemeV2.h"

#include <cstdio>
#include <cstring>
#include <ctime>

#include "Config.h"
#include "RetroFont.h"
#include "WeatherUi.h"  // WeatherUi::viewSkipped (statyczna, publiczna — patrz WeatherUi.h)

namespace themev2 {

namespace {

// ---- rysowanie glifow RetroFontu (kopia petli z WeatherUi.cpp — patrz uzasadnienie
// w ThemeV2.h przy komentarzu "tekst RetroFontu") ---------------------------------

void drawGlyph(TFT_eSPI& s, char c, int x, int y, int scale, uint16_t color) {
  const int idx = retrofont::index(c);
  if (idx < 0) return;  // znak spoza zestawu — po prostu pomijamy (jak w RETRO)
  for (int row = 0; row < 8; ++row) {
    const uint8_t bits = pgm_read_byte(&retrofont::GLYPHS[idx][row]);
    if (bits == 0) continue;
    int col = 0;
    while (col < 8) {
      if (!(bits & (0x80 >> col))) {
        ++col;
        continue;
      }
      int end = col;
      while (end < 8 && (bits & (0x80 >> end))) ++end;
      s.fillRect(x + col * scale, y + row * scale, (end - col) * scale, scale, color);
      col = end;
    }
  }
}

// Mieszanie kanalu 0-255 liniowo — do kwantyzowanego gradientu nieba.
uint8_t mixChan(int a, int b, float t) {
  return static_cast<uint8_t>(a + static_cast<int>((b - a) * t));
}

}  // namespace

int textWidth(const char* s, int scale) {
  if (!s) return 0;
  return static_cast<int>(strlen(s)) * 9 * scale;
}

void text(TFT_eSPI& spr, int x, int y, int scale, uint16_t color, const char* s) {
  if (!s) return;
  int cx = x;
  for (const char* p = s; *p; ++p) {
    drawGlyph(spr, *p, cx, y, scale, color);
    cx += 9 * scale;
  }
}

void textShadowed(TFT_eSPI& spr, int x, int y, int scale, uint16_t color, const char* s) {
  text(spr, x + scale, y + scale, scale, C565(0, 0, 0), s);
  text(spr, x, y, scale, color, s);
}

// Tabela identyczna znaczeniowo z retroAscii() w WeatherUi.cpp (te same punkty
// kodowe UTF-8 dla polskich liter) — niezalezna implementacja, patrz ThemeV2.h.
void foldAscii(char* dst, size_t dstSize, const char* src) {
  if (!dst || dstSize == 0) return;
  size_t o = 0;
  auto p = reinterpret_cast<const unsigned char*>(src ? src : "");
  while (*p != 0 && o + 1 < dstSize) {
    char rep = 0;
    if (p[0] == 0xC4 && p[1] != 0) {
      switch (p[1]) {
        case 0x84: case 0x85: rep = 'A'; break;  // Ą ą
        case 0x86: case 0x87: rep = 'C'; break;  // Ć ć
        case 0x98: case 0x99: rep = 'E'; break;  // Ę ę
      }
      if (rep) dst[o++] = rep;
      p += 2;
      continue;
    }
    if (p[0] == 0xC5 && p[1] != 0) {
      switch (p[1]) {
        case 0x81: case 0x82: rep = 'L'; break;  // Ł ł
        case 0x83: case 0x84: rep = 'N'; break;  // Ń ń
        case 0x9A: case 0x9B: rep = 'S'; break;  // Ś ś
        case 0xB9: case 0xBA: rep = 'Z'; break;  // Ź ź
        case 0xBB: case 0xBC: rep = 'Z'; break;  // Ż ż
      }
      if (rep) dst[o++] = rep;
      p += 2;
      continue;
    }
    if (p[0] == 0xC3 && p[1] != 0) {
      if (p[1] == 0x93 || p[1] == 0xB3) rep = 'O';  // Ó ó
      if (rep) dst[o++] = rep;
      p += 2;
      continue;
    }
    char c = static_cast<char>(*p);
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    dst[o++] = c;
    ++p;
  }
  dst[o] = '\0';
}

void hudTop(TFT_eSPI& spr, int ox, const char* city) {
  spr.fillRect(ox, 0, cfg::SCREEN_W, cfg::V2_HUD_H, col2::BG);
  spr.drawFastHLine(ox, cfg::V2_HUD_H - 1, cfg::SCREEN_W, col2::CARD_TOP);

  char cityBuf[40];
  foldAscii(cityBuf, sizeof(cityBuf), city);

  char dateBuf[8] = "--- --";
  char timeBuf[6] = "--:--";
  const time_t now = time(nullptr);
  if (now >= 1700000000) {
    struct tm tmv{};
    localtime_r(&now, &tmv);
    // Miesiace ASCII-safe (RetroFont nie zna Z) — WLASNA tabela, nie kMon z
    // WeatherUi.cpp (ta ma "paź" z ogonkowym z, ktorego font nie narysuje).
    static const char* const kMon[12] = {"STY", "LUT", "MAR", "KWI", "MAJ", "CZE",
                                         "LIP", "SIE", "WRZ", "PAZ", "LIS", "GRU"};
    snprintf(dateBuf, sizeof(dateBuf), "%d %s", tmv.tm_mday, kMon[tmv.tm_mon % 12]);
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
  }

  // Miasto ZAWSZE na skali 3 (tak jak w opisie zadania) — data/godzina schodza
  // na skale 2 tylko, gdy komplet NIE miesci sie w 320 px. Realny krok fontu to
  // 9*scale (8 px glifu + 1 px odstepu), NIE 8*scale — przy skali 3 to 27 px/znak,
  // a nie 24 (uproszczenie z opisu zadania pomijalo ten 1 px odstepu). Liczymy
  // wiec naprawde, zamiast zakladac z gory, ze "GDYNIA" = 144 px.
  // UKLAD SEKWENCYJNY, NIE CENTROWANY. Pierwsza wersja centrowala date w calym
  // ekranie i sprawdzala kolizje TYLKO PRZED zejsciem na mniejsza skale — a po
  // zejsciu juz nie. Efekt na urzadzeniu: "GDYNIA" (skala 3 = 162 px, konczy sie
  // na x=170) nachodzilo na date wysrodkowana od x=106. Zweryfikowane zrzutem.
  //
  // Teraz: godzina zawsze przy prawej krawedzi, miasto przy lewej, a data dostaje
  // to, co REALNIE zostalo miedzy nimi — i dopiero gdy tam nie wchodzi, calosc
  // (razem z miastem) schodzi o stopien nizej. Miasto jest najwazniejsze, ale
  // czytelne miasto nachodzace na date nie jest czytelne wcale.
  int cityScale = 3, scale = 3;
  int cityW = 0, dateW = 0, timeW = 0, gapL = 0;
  for (int attempt = 0; attempt < 3; ++attempt) {
    cityW = textWidth(cityBuf, cityScale);
    dateW = textWidth(dateBuf, scale);
    timeW = textWidth(timeBuf, scale);
    gapL = ox + 8 + cityW;                                   // koniec miasta
    const int timeX = ox + cfg::SCREEN_W - 8 - timeW;        // poczatek godziny
    if (gapL + 8 + dateW + 8 <= timeX) break;                // miesci sie
    if (attempt == 0) scale = 2;                             // najpierw data/godzina
    else cityScale = 2;                                      // potem takze miasto
  }
  const int timeX = ox + cfg::SCREEN_W - 8 - timeW;
  // Data wysrodkowana w REALNIE dostepnej przerwie, nie w calym ekranie.
  int dateX = gapL + 8 + ((timeX - 8) - (gapL + 8) - dateW) / 2;
  if (dateX < gapL + 8) dateX = gapL + 8;
  const int cityY = (cityScale == 3) ? 2 : 6;
  const int dateY = (scale == 3) ? 2 : 6;

  textShadowed(spr, ox + 8, cityY, cityScale, col2::TEXT, cityBuf);
  textShadowed(spr, dateX, dateY, scale, col2::LABEL, dateBuf);
  textShadowed(spr, timeX, dateY, scale, col2::VALUE2, timeBuf);
}

void hudSegments(TFT_eSPI& spr, int ox, int curView, int viewCount, float frac,
                 const AirModel* air) {
  spr.fillRect(ox, cfg::V2_SEG_Y, cfg::SCREEN_W, cfg::V2_SEG_H, col2::BG);
  if (viewCount <= 0) return;
  if (frac < 0.f) frac = 0.f;
  if (frac > 1.f) frac = 1.f;

  const int segW = cfg::SCREEN_W / viewCount;
  for (int i = 0; i < viewCount; ++i) {
    const int x = ox + i * segW;
    const int w = (i == viewCount - 1) ? (cfg::SCREEN_W - i * segW) : (segW - 2);

    // Warunki "pomijany" ZYJA WYLACZNIE w WeatherUi::viewSkipped (radar bez
    // opadu, dom bez czujnikow, piec bez autoryzacji, powietrze bez danych) —
    // patrz komentarz przy tej metodzie w WeatherUi.h. Gdyby powtorzyc je tu
    // osobno, prędzej czy pozniej rozjadą się od paska V1 (drawProgress).
    if (WeatherUi::viewSkipped(i, air)) {
      const uint16_t dim = C565(mixChan(10, 255, 0.28f), mixChan(14, 144, 0.28f),
                                mixChan(26, 64, 0.28f));
      spr.fillRect(x, cfg::V2_SEG_Y, w, cfg::V2_SEG_H, dim);
      continue;
    }

    spr.fillRect(x, cfg::V2_SEG_Y, w, cfg::V2_SEG_H, col2::CARD);
    if (i < curView) {
      spr.fillRect(x, cfg::V2_SEG_Y, w, cfg::V2_SEG_H, col2::CARD_TOP);
    } else if (i == curView) {
      const int fw = static_cast<int>(w * frac);
      if (fw > 0) spr.fillRect(x, cfg::V2_SEG_Y, fw, cfg::V2_SEG_H, col2::VALUE2);
    }
  }
}

void titleRow(TFT_eSPI& spr, int ox, const char* name, const char* info, const char* tag) {
  spr.fillRect(ox, cfg::V2_TITLE_Y, cfg::SCREEN_W, cfg::V2_TITLE_H, col2::BG);

  int x = ox + 8;
  if (name && name[0]) {
    text(spr, x, cfg::V2_TITLE_Y, 2, col2::LABEL, name);
    x += textWidth(name, 2) + 10;
  }

  int tagX = ox + cfg::SCREEN_W;
  if (tag && tag[0]) {
    const int tagW = textWidth(tag, 1);
    tagX = ox + cfg::SCREEN_W - 8 - tagW;
    text(spr, tagX, cfg::V2_TITLE_Y + 4, 1, col2::DIM, tag);
  }

  if (info && info[0]) {
    const int avail = tagX - 8 - x;
    // Info jest DRUGORZEDNE: gdy nie miesci sie miedzy tytulem a tagiem, po
    // prostu go pomijamy zamiast na sile scinac — tytul ekranu sam w sobie
    // juz mowi, na czym stoimy.
    if (avail > 0 && textWidth(info, 1) <= avail) {
      text(spr, x, cfg::V2_TITLE_Y + 4, 1, col2::TEXT, info);
    }
  }
}

void card(TFT_eSPI& spr, int x, int y, int w, int h, const char* label, const char* value,
         const char* value2, bool warn, uint16_t valueColor) {
  spr.fillRect(x, y, w, h, col2::CARD);
  spr.fillRect(x, y, w, 2, col2::CARD_TOP);

  constexpr int pad = 6;
  // Etykiety w V1 sa krotkie z definicji (patrz kViewNames — max 12 znakow),
  // ale karty V2 bywaja waskie (siatka 3-4 w rzedzie), a RetroFont nie ma
  // proporcjonalnej szerokosci — "CISNIENIE" na skali 2 to 162 px, wiecej niz
  // niejedna karta. Ten sam wzorzec co przy `value` nizej: najpierw pelna
  // skala, dopiero gdy nie miesci sie — mniejsza, NIGDY ciecie napisu.
  if (label && label[0]) {
    int lscale = 2;
    if (textWidth(label, lscale) > w - 2 * pad) lscale = 1;
    text(spr, x + pad, y + 4, lscale, col2::LABEL, label);
  }
  if (value && value[0]) {
    int scale = 3;
    if (textWidth(value, scale) > w - 2 * pad) scale = 2;
    if (textWidth(value, scale) > w - 2 * pad) scale = 1;
    text(spr, x + pad, y + 22, scale, valueColor, value);
  }
  if (value2 && value2[0]) {
    int vscale = 2;
    if (textWidth(value2, vscale) > w - 2 * pad) vscale = 1;
    text(spr, x + pad, y + 48, vscale, warn ? col2::WARN : col2::VALUE2, value2);
  }
}

void sceneBackground(TFT_eSPI& spr, int ox, int top, int bottom, int horizonY,
                     const SceneRect* silhouette, int nSilhouette) {
  const int hy = horizonY < top ? top : (horizonY > bottom ? bottom : horizonY);

  // Niebo: pasy od tla HUD (10,14,26 — najciemniejszy ton palety) do gornej
  // krawedzi karty (42,58,90 — najjasniejszy neutralny ton palety) przy
  // horyzoncie. Oba konce gradientu to kolory JUZ ISTNIEJACE w palecie
  // mockupu (col2::BG/col2::CARD_TOP) — mockup nie podawal osobnych barw
  // nieba, wiec zamiast wymyslac nowe, uzywamy tych samych dwoch tonow, ktore
  // juz nosza znaczenie "tlo" i "jasna krawedz". `& 0xF0` na kazdym kanale to
  // CELOWY retro-banding (pasy zamiast plynnego przejscia) — ten sam trik, co
  // niebo w WeatherUi::drawViewRetro, NIE blad zaokraglenia.
  for (int y = top; y < hy; ++y) {
    const float f = (hy > top) ? static_cast<float>(y - top) / static_cast<float>(hy - top) : 0.f;
    const uint8_t r = mixChan(10, 42, f) & 0xF0;
    const uint8_t g = mixChan(14, 58, f) & 0xF0;
    const uint8_t b = mixChan(26, 90, f) & 0xF0;
    spr.drawFastHLine(ox, y, cfg::SCREEN_W, C565(r, g, b));
  }

  spr.drawFastHLine(ox, hy, cfg::SCREEN_W, col2::CARD_TOP);  // linia horyzontu
  if (hy + 1 < bottom) {
    spr.fillRect(ox, hy + 1, cfg::SCREEN_W, bottom - hy - 1, col2::CARD);  // grunt/woda
  }

  for (int i = 0; i < nSilhouette; ++i) {
    const SceneRect& r = silhouette[i];
    spr.fillRect(ox + r.x, r.y, r.w, r.h, col2::BG);
  }
}

void flatBackground(TFT_eSPI& spr, int ox, int top, int bottom) {
  spr.fillRect(ox, top, cfg::SCREEN_W, bottom - top, col2::BG);
}

}  // namespace themev2
