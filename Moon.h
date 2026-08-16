#pragma once

#include <TFT_eSPI.h>

#include <cmath>
#include <ctime>

#include "Colors.h"

// Ksiezyc rysowany PROCEDURALNIE, a nie z bitmapy — bo jego ksztalt zmienia sie
// kazdej nocy. Wejsciem jest ZAWSZE czas UTC z systemu (time_t z time(nullptr)):
// time_t liczy sekundy od epoki bez strefy, a projekt nigdzie nie przesuwa go o
// strefe przed podaniem tutaj (WeatherUi.cpp, ThemeV3.cpp wolaja time(nullptr) wprost).
//
// DLACZEGO ZMIENILISMY MODEL (do v158 wlacznie bylo inaczej). Faza liczyla sie jako
// reszta z dzielenia czasu przez SREDNI miesiac synodyczny 29,530588853 d, odmierzany
// od stalego nowiu 6 stycznia 2000 (JD 2451550.1). To jest model LINIOWY, a rzeczywisty
// odstep miedzy kolejnymi nowiami waha sie od okolo 29,27 do 29,83 doby, bo orbita
// Ksiezyca jest eliptyczna i zaburzana przez Slonce. ZMIERZONE na 25 wzorcowych
// momentach roku 2026 (12 nowi + 13 pelni; AstroPixels / F. Espenak, "Phases of the
// Moon: 2001 to 2100", czasy UT, potwierdzone drugim zrodlem co do minuty): stary
// model mial blad SREDNI 6,63 h, a MAKSYMALNY 13,56 h (now 10 pazdziernika 2026).
// Czyli ekran potrafil pokazywac pelnie, gdy do pelni brakowalo ponad pol doby — a
// wlasciciel sprawdza to przez okno. Nowy model na tej samej probce: srednio 0,07 h,
// maksymalnie 0,15 h (9 minut), przy kryterium przyjecia 2 h.
//
// CO JEST TERAZ: ELONGACJA, czyli roznica dlugosci ekliptycznych Ksiezyca i Slonca,
// policzona z obcietego szeregu (Meeus, "Astronomical Algorithms", wyd. 2, tab. 47.A
// dla Ksiezyca i rozdz. 25 dla Slonca). Elongacja 0 stopni to z definicji now, 180 to
// pelnia, wiec faza = elongacja/360 — i ta sama liczba daje oswietlona czesc tarczy
// oraz kierunek (przybywa, gdy elongacja < 180).
//
// WKLAD POSZCZEGOLNYCH WYRAZOW. Elongacja rosnie srednio o 0,5079 st./h, wiec kazdy
// stopien amplitudy to okolo 1,97 h przesuniecia w czasie fazy:
//   rownanie srodka Ksiezyca      sin(M')       6,2888 st. = 12,38 h
//   rown. roczne + rown. srodka Slonca sin(M)   2,0997 st. =  4,13 h  (znak minus;
//        1,9146 pochodzi od Slonca, 0,1851 to rownanie roczne Ksiezyca)
//   ewekcja                       sin(2D-M')    1,2740 st. =  2,51 h
//   wariacja                      sin(2D)       0,6583 st. =  1,30 h
//   2. harmonika anomalii Ksiezyca sin(2M')     0,2136 st. =  0,42 h
//   wyraz szerokosciowy           sin(2F)       0,1143 st. =  0,23 h
//   dalsze 23 wyrazy              0,0588 st. i mniej
//
// ZBIEZNOSC OBCIECIA — zmierzona, nie oszacowana. Kolumna A: maksymalny blad czasu na
// 25 wzorcach 2026. Kolumna B: maksymalna roznica wzgledem pelnej serii liczona co
// 997 s przez CALY rok, czyli takze miedzy nowiem a pelnia, gdzie wzorcow nie ma.
//   liczba wyrazow:   4      5      6      8     10     14     20     25     29
//   A (wzorce):    4,28h  0,47h  0,42h  0,22h  0,21h  0,17h  0,14h  0,15h  0,15h
//   B (caly rok):  4,49h  0,79h  0,57h  0,35h  0,19h  0,12h  0,06h  0,02h    ---
// Skok miedzy 4 a 5 wyrazem jest tak duzy, bo piaty to wlasnie sin(M). Kryterium 2 h
// spelnia juz 5 wyrazow; bierzemy 29, bo reszta kosztuje 21 wywolan sinf raz na
// kwadrans (okolo 25 us) i 1,4 kB flasha, a scina blad miedzy fazami z 0,79 h do 0,02 h
// — a wlasnie miedzy nowiem a pelnia glif spedza wiekszosc czasu.
//
// CZEGO SWIADOMIE NIE LICZYMY, bo kazde z tego jest ponizej 0,01 st. = 70 s czasu:
//   * roznicy TT-UT (2026: okolo +69 s) — podajemy UTC prosto jako argument szeregu,
//   * nutacji w dlugosci — dziala na Ksiezyc i Slonce tak samo i SKRACA SIE w roznicy,
//   * aberracji rocznej Slonca (-0,0057 st.) i czasu biegu swiatla od Ksiezyca,
//   * poprawki mimosrodu E (Meeus 47.6): w 2026 E = 0,99933, wiec przy najwiekszym
//     wyrazie z M (1,9146 st.) blad wynosi 0,0013 st. = 9 s.
//
// Konwencja polkuli polnocnej: ksiezyc PRZYBYWAJACY jest oswietlony z PRAWEJ,
// UBYWAJACY z LEWEJ. W Gdyni wyglada dokladnie tak.

namespace moon {

namespace detail {

// Sprowadzenie kata do [0,360). Musi byc w DOUBLE: srednie dlugosci narastaja o
// 477 tysiecy stopni na stulecie, wiec w roku 2026 argument fmod ma rzad 1,2e5 stopnia.
inline double wrap360(double x) {
  x = fmod(x, 360.0);
  return x < 0.0 ? x + 360.0 : x;
}

// Elongacja w stopniach, zakres [0,360). 0 = now, 180 = pelnia.
//
// DOUBLE vs FLOAT — decyzja policzona, nie z przyzwyczajenia. ESP32-S3 ma FPU tylko
// dla float; double idzie przez emulacje programowa. Dlatego double stoi WYLACZNIE
// w dwoch pierwszych krokach i nigdzie dalej:
//  1. w przeliczeniu time_t na T. Sam argument jest nie do uniesienia dla float:
//     time_t ma dzis rzad 1,8e9 s, a float ma 24 bity mantysy, czyli ulp = 128 s —
//     zegar "skakalby" o dwie minuty niezaleznie od reszty rachunku.
//  2. w czterech srednich katach i ich redukcji mod 360. Przy T = 0,27 (rok 2026)
//     iloczyn 477198,8675 * T ma rzad 1,3e5 stopnia; w float ulp wynosi tam 0,0078 st.,
//     a sam blad reprezentacji T dokłada podobnie. ZMIERZONE: wariant liczony CALKOWICIE
//     w float rozjezdza sie z double o 0,0370 st., czyli 4,4 MINUTY czasu wpisane na
//     stale w model; obecny (double na argumentach) trzyma sie double w granicach
//     0,000193 st. = 1,4 s. Oba mieszcza sie w kryterium 2 h, ale te dwa kroki kosztuja
//     razem okolo 4 tys. cykli raz na kwadrans, wiec nie ma za co placic bledem.
// KOSZT, policzony z disasemblacji build_verify (nie zmierzony na sprzecie — urzadzenia
// nie ma pod reka): elongationDeg to 527 instrukcji i 55 wywolan posrednich — 29 x sinf,
// 21 x procedury emulacji double (__muldf3 x9, __adddf3 x5, __subdf3 x4, __divdf3 x2,
// __truncdfsf2 x4, __floatdidf x1) i 4 x wrap360, z ktorych kazdy wola fmod. Przy
// typowych kosztach tych procedur na Xtensa LX7 (mnozenie ~110 c., dodawanie ~90 c.,
// dzielenie ~380 c., fmod ~200 c., sinf ~125 c.) daje to okolo 8,5 tys. cykli, czyli
// ~35 us przy 240 MHz. Raz na 900 s to 0,04 us na sekunde pracy — kilkaset razy MNIEJ
// niz stary, tani model liczony po kilka razy w kazdej klatce (20 klatek/s).
// Dalej (29 sinusow i suma) wszystko jest w FLOAT: najwiekszy wyraz ma 6,29 st., ulp
// float przy tej wartosci to 5e-7 st. = 0,004 s czasu — sto tysiecy razy ponizej progu.
// Argumenty sinusa sa juz sprowadzone do [0,4pi), wiec nie ma utraty precyzji na
// redukcji zakresu wewnatrz sinf().
inline float elongationDeg(time_t t) {
  // JD = t/86400 + 2440587,5;  T = (JD - 2451545,0)/36525.  2440587,5 - 2451545,0 = -10957,5.
  const double T = (static_cast<double>(t) / 86400.0 - 10957.5) / 36525.0;
  const double T2 = T * T;

  // Cztery srednie katy (Meeus 47.2-47.5). Wyrazy przy T^3/T^4 pominiete: do roku 2050
  // (T < 0,5) najwiekszy z nich to T^3/69699 < 2e-6 st. = 0,01 s czasu.
  const float dD  = static_cast<float>(wrap360(297.8501921 + 445267.1114034 * T - 0.0018819 * T2));
  const float dM  = static_cast<float>(wrap360(357.5291092 +  35999.0502909 * T - 0.0001536 * T2));
  const float dMp = static_cast<float>(wrap360(134.9633964 + 477198.8675055 * T + 0.0087414 * T2));
  const float dF  = static_cast<float>(wrap360( 93.2720950 + 483202.0175233 * T - 0.0036539 * T2));

  constexpr float kRad = 0.0174532925f;
  const float D = dD * kRad;    // srednia elongacja
  const float M = dM * kRad;    // anomalia srednia Slonca
  const float P = dMp * kRad;   // anomalia srednia Ksiezyca (u Meeusa M')
  const float F = dF * kRad;    // argument szerokosci Ksiezyca

  // Elongacja = (L' + suma_ksiezycowa) - (L0 + rownanie_srodka_Slonca), a D = L' - L0
  // z definicji, wiec zostaje D + suma_ksiezycowa - rownanie_srodka_Slonca.
  // Wyraz sin(M) jest zlozony z DWOCH zrodel: rownania rocznego Ksiezyca (-0,185116)
  // i rownania srodka Slonca (-1,914602) — razem -2,099718.
  float e = dD
      + 6.288774f  * sinf(P)
      + 1.274027f  * sinf(2.f * D - P)
      + 0.658314f  * sinf(2.f * D)
      + 0.213618f  * sinf(2.f * P)
      - 2.099718f  * sinf(M)
      - 0.114332f  * sinf(2.f * F)
      + 0.058793f  * sinf(2.f * D - 2.f * P)
      + 0.057066f  * sinf(2.f * D - M - P)
      + 0.053322f  * sinf(2.f * D + P)
      + 0.045758f  * sinf(2.f * D - M)
      - 0.040923f  * sinf(P - M)
      - 0.034720f  * sinf(D)
      - 0.030383f  * sinf(P + M)
      - 0.019993f  * sinf(2.f * M)            // rownanie srodka Slonca, 2. harmonika
      + 0.015327f  * sinf(2.f * D - 2.f * F)
      - 0.012528f  * sinf(P + 2.f * F)
      + 0.010980f  * sinf(P - 2.f * F)
      + 0.010675f  * sinf(4.f * D - P)
      + 0.010034f  * sinf(3.f * P)
      + 0.008548f  * sinf(4.f * D - 2.f * P)
      - 0.007888f  * sinf(2.f * D + M - P)
      - 0.006766f  * sinf(2.f * D + M)
      - 0.005163f  * sinf(D - P)
      + 0.004987f  * sinf(D + M)
      + 0.004036f  * sinf(2.f * D - M + P)
      + 0.003994f  * sinf(2.f * D + 2.f * P)
      + 0.003861f  * sinf(4.f * D)
      + 0.003665f  * sinf(2.f * D - 3.f * P)
      - 0.000289f  * sinf(3.f * M);           // rownanie srodka Slonca, 3. harmonika

  e = fmodf(e, 360.f);
  return e < 0.f ? e + 360.f : e;
}

}  // namespace detail

// 0.0 = now, 0.25 = pierwsza kwadra, 0.5 = pelnia, 0.75 = ostatnia kwadra.
//
// BUFOROWANIE. To jest sciezka RYSOWANIA KLATKI: loop() wola WeatherUi::render() co
// ~50 ms, a phase() siedzi w drawNow() i drawHours() (WeatherUi.cpp) oraz w sunOrMoon()
// motywu V3 — bez bufora szereg liczylby sie 20 razy na sekunde po nic. Faza zmienia
// sie o 1% na 7,1 h, wiec 15 minut (900 s) to zapas rzedu 30x: przez ten czas
// terminator na najwiekszej tarczy w projekcie (r = 19 px) przesuwa sie o 0,06 px.
// Roznica liczona jest BEZWZGLEDNIE, bo skok NTP potrafi cofnac zegar i inaczej bufor
// zamarzlby na zawsze. Statyki w funkcji inline maja jedna instancje na caly program
// (i sa inicjowane stalymi, wiec bez straznika __cxa_guard). Ekran maluje zawsze rdzen
// 1, ale zrzut przez /api/screen moze wejsc z zadania serwera — wyscig jest tu
// nieszkodliwy: 32-bitowy odczyt float na Xtensa jest niepodzielny, wiec najgorsze co
// sie stanie to jedna klatka z faza sprzed kwadransa.
inline float phase(time_t t) {
  if (t < 1700000000) return 0.5f;   // brak czasu — rysujemy pelnie zamiast nonsensu

  static time_t cachedAt = 0;
  static float cachedPhase = 0.5f;

  const time_t age = (t >= cachedAt) ? (t - cachedAt) : (cachedAt - t);
  if (age >= 900) {
    cachedPhase = detail::elongationDeg(t) * (1.f / 360.f);
    cachedAt = t;
  }
  return cachedPhase;
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
//
// Kolory sa PARAMETRAMI, bo ten sam ksiezyc rysuje sie na roznych tlach: V1/V2 maja
// granatowa noc, a motyw V3 stawia go raz na ciemnej kolumnie kontekstu, raz na jasnym
// module PRAD — kremowa tarcza na jasnym tle bylaby niewidoczna. Wartosci DOMYSLNE sa
// dokladnie tymi, ktore staly tu wczesniej na sztywno, wiec wszystkie dotychczasowe
// wywolania (WeatherUi.cpp, WeatherIcons.h) rysuja piksel w piksel to samo, co przedtem.
// `dark` warto podac rowne kolorowi TLA — wtedy czesc zacieniona znika w tle, a nów
// zostaje samym obrysem `rim` (bez obrysu wygladalby na dziure po bledzie rysowania).
inline void draw(TFT_eSPI& s, int cx, int cy, int r, float p,
                 uint16_t lit = C565(248, 244, 220),    // tarcza oswietlona
                 uint16_t dark = C565(46, 54, 74),      // czesc zacieniona
                 uint16_t rim = C565(96, 108, 134)) {   // obrys, zeby now nie znikal calkiem
  if (r < 3) return;

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
