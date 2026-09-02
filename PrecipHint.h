#pragma once

#include <cstdint>

#include "Config.h"

// (Blok G, runda 2) Właściciel: "wyświetlacz mówi sucho, a za oknem pada". Kod nie
// był zepsuty — odpowiadał na INNE pytanie. Napis OPAD na ekranie głównym czytał
// wyłącznie prognozę (WX_HOURS = +1h..+12h, patrz WeatherData.h), która z definicji
// nie ma jak zobaczyć deszczu padającego W TEJ CHWILI — a urządzenie RÓWNOLEGLE ma
// świeży odczyt radaru (RadarClient.h), mówiący dokładnie to, czego szuka patrzący
// przez okno. Ta funkcja rozstrzyga, KTÓRE z dwóch źródeł (obserwacja radaru kontra
// prognoza) ma prawo głosu — czysta logika, zero rysowania i zero formatowania
// napisu (to zostaje w WeatherUiV3.cpp), więc da się ją przetestować na hoście bez
// żadnej zależności od Arduino/TFT.
enum class PrecipHintKind {
  kRadarNow,      // radar realnie widzi opad TERAZ (świeża klatka, poziom > 0)
  kForecastPeak,  // brak świeżej obserwacji radaru — prognoza ze szczytem > progu
  kDry,           // brak świeżej obserwacji radaru I prognoza poniżej progu
};

// PRÓG ŚWIEŻOŚCI RADARU: cfg::RADAR_STALE_MS (15 min) — TEN SAM próg, którego ekran
// RADAR już używa do własnego wskaźnika świeżości (2,5 x kadencja pobierania 5 min,
// pełne uzasadnienie przy stałej w Config.h). Jedno źródło prawdy zamiast nowej,
// osobno wymyślonej liczby: opad sprzed 10 minut nadal jest sensowną odpowiedzią na
// "czy pada" (RainViewer daje klatki co ~10 min), opad sprzed godziny już nie —
// 15 minut siedzi wygodnie w tym przedziale.
//
// `radarTrueAgeS` MUSI być CAŁKOWITYM, bieżącym wiekiem klatki, nie samym
// RadarSnapshot::ageSec: ageSec to wiek klatki W CHWILI POBRANIA i nie rusza się
// między pobraniami. Sam w sobie by mylił: klatka mogła być świeża godzinę temu,
// przy ostatnim udanym pobraniu, i dziś wciąż niosłaby tę samą liczbę, choć realnie
// już nie jest aktualna. Wywołujący dokłada więc czas od ostatniego udanego
// pobrania (np. `w.radarAgeSec + okAgeS(diag().radarOkAt)` w WeatherUiV3.cpp).
inline PrecipHintKind choosePrecipHint(bool radarValid, uint8_t radarLevel,
                                        uint32_t radarTrueAgeS, int bestProb,
                                        int bestHour) {
  if (radarValid && radarLevel > 0 &&
      radarTrueAgeS <= (cfg::RADAR_STALE_MS / 1000)) {
    return PrecipHintKind::kRadarNow;
  }
  if (bestProb >= 20 && bestHour >= 0) {
    return PrecipHintKind::kForecastPeak;
  }
  return PrecipHintKind::kDry;
}
