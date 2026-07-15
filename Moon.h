#pragma once

#include <TFT_eSPI.h>

#include <cmath>
#include <ctime>

#include "Colors.h"

// Ksiezyc rysowany PROCEDURALNIE, a nie z bitmapy — bo jego ksztalt zmienia sie
// kazdej nocy. Faza liczona z kalendarza: miesiac synodyczny ma 29,530588853 dnia,
// a punktem odniesienia jest now z 6 stycznia 2000, 18:14 UTC (JD 2451550.1).
//
// Konwencja polkuli polnocnej: ksiezyc PRZYBYWAJACY jest oswietlony z PRAWEJ,
// UBYWAJACY z LEWEJ. W Gdyni wyglada dokladnie tak.

namespace moon {

// 0.0 = now, 0.25 = pierwsza kwadra, 0.5 = pelnia, 0.75 = ostatnia kwadra
inline float phase(time_t t) {
  if (t < 1700000000) return 0.5f;   // brak czasu — rysujemy pelnie zamiast nonsensu

  const double jd = static_cast<double>(t) / 86400.0 + 2440587.5;
  const double p = fmod((jd - 2451550.1) / 29.530588853, 1.0);
  return static_cast<float>(p < 0 ? p + 1.0 : p);
}

// Jaka czesc tarczy jest oswietlona (0..1)
inline float illum(float p) {
  return 0.5f * (1.f - cosf(2.f * static_cast<float>(M_PI) * p));
}

// Nazwy sa RZECZOWNIKOWE i samodzielne. Bylo "Przybywa" / "Ubywa" — czasownik bez
// podmiotu, ktory pod ikona wygladal na urwany w pol zdania ("Czyste niebo /
// Przybywa" — czego przybywa?). Reszta faz byla rzeczownikami ("Nów", "Pełnia"),
// wiec te dwie wylamywaly sie tez ze wzoru.
// Sierp = ponizej kwadry, garb = powyzej — tak sie te fazy nazywaja po polsku.
// Zmierzone w PLF14: najszersze slowo to "przybywa" (60 px) przy limicie 116 px
// na slowo (drawWeatherDesc lamie tylko przy spacji).
inline const char* name(float p) {
  if (p < 0.03f || p > 0.97f) return "Nów";
  if (p < 0.22f) return "Sierp przybywa";
  if (p < 0.28f) return "Pierwsza kwadra";
  if (p < 0.47f) return "Garb przybywa";
  if (p < 0.53f) return "Pełnia";
  if (p < 0.72f) return "Garb ubywa";
  if (p < 0.78f) return "Ostatnia kwadra";
  return "Sierp ubywa";
}

// Terminator (granica swiatla i cienia) to elipsa. Dla kazdego wiersza tarczy
// polowa szerokosci to w = sqrt(r^2 - y^2), a granica lezy na x = w * (1 - 2k),
// gdzie k to oswietlona czesc. Stad: k=0 -> granica na prawej krawedzi (now),
// k=0.5 -> przez srodek (kwadra), k=1 -> na lewej krawedzi (pelnia).
inline void draw(TFT_eSPI& s, int cx, int cy, int r, float p) {
  if (r < 3) return;

  const uint16_t lit = C565(248, 244, 220);   // tarcza oswietlona
  const uint16_t dark = C565(46, 54, 74);     // czesc zacieniona — widoczna, ale ciemna
  const uint16_t rim = C565(96, 108, 134);    // obrys, zeby now nie znikal calkiem

  const float k = illum(p);
  const bool waxing = p < 0.5f;

  for (int y = -r; y <= r; ++y) {
    const int w = static_cast<int>(sqrtf(static_cast<float>(r * r - y * y)));
    if (w <= 0) continue;

    s.drawFastHLine(cx - w, cy + y, 2 * w + 1, dark);

    const int xt = static_cast<int>(lroundf(w * (1.f - 2.f * k)));
    if (waxing) {
      if (xt <= w) s.drawFastHLine(cx + xt, cy + y, w - xt + 1, lit);
    } else {
      if (-xt >= -w) s.drawFastHLine(cx - w, cy + y, w - xt + 1, lit);
    }
  }
  s.drawCircle(cx, cy, r, rim);
}

}  // namespace moon
