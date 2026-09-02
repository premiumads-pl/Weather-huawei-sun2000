// Test dla Bloku G (runda 2): choosePrecipHint() w PrecipHint.h — czysta funkcja
// wyboru miedzy "radar widzi opad teraz" a "dotychczasowa logika prognozy".
// Wlacza PRAWDZIWY naglowek produkcyjny (nie kopie) — cfg::RADAR_STALE_MS jest
// wciagane wprost z Config.h.
#include <gtest/gtest.h>

#include "../PrecipHint.h"

namespace {
constexpr uint32_t kStaleS = cfg::RADAR_STALE_MS / 1000;  // 900 s = 15 min
}  // namespace

// Przypadki wprost z briefu.

TEST(ChoosePrecipHint, NoRadarLevelWithHighForecastIsForecastPeak) {
  // radar 0 (brak opadu na radarze) + prognoza 85% -> prognoza
  EXPECT_EQ(choosePrecipHint(/*radarValid=*/true, /*radarLevel=*/0,
                             /*radarTrueAgeS=*/60, /*bestProb=*/85, /*bestHour=*/22),
            PrecipHintKind::kForecastPeak);
}

TEST(ChoosePrecipHint, FreshRadarWithRainIsRadarNow) {
  // radar 2 (deszcz), swiezy (60 s) -> obserwacja
  EXPECT_EQ(choosePrecipHint(/*radarValid=*/true, /*radarLevel=*/2,
                             /*radarTrueAgeS=*/60, /*bestProb=*/0, /*bestHour=*/-1),
            PrecipHintKind::kRadarNow);
}

TEST(ChoosePrecipHint, HourOldRadarWithRainFallsBackToForecast) {
  // radar 2, ale klatka sprzed godziny (3600 s, znacznie ponad prog 900 s) -> prognoza
  EXPECT_EQ(choosePrecipHint(/*radarValid=*/true, /*radarLevel=*/2,
                             /*radarTrueAgeS=*/3600, /*bestProb=*/85, /*bestHour=*/22),
            PrecipHintKind::kForecastPeak);
}

TEST(ChoosePrecipHint, InvalidRadarFallsBackToForecast) {
  // radar nigdy nie pobrany (valid=false) -> prognoza, niezaleznie od level/age
  EXPECT_EQ(choosePrecipHint(/*radarValid=*/false, /*radarLevel=*/2,
                             /*radarTrueAgeS=*/60, /*bestProb=*/85, /*bestHour=*/22),
            PrecipHintKind::kForecastPeak);
}

TEST(ChoosePrecipHint, NoRadarWithLowForecastIsDry) {
  // radar 0 + prognoza 15% (ponizej progu 20%) -> dotychczasowe "sucho"
  EXPECT_EQ(choosePrecipHint(/*radarValid=*/true, /*radarLevel=*/0,
                             /*radarTrueAgeS=*/60, /*bestProb=*/15, /*bestHour=*/22),
            PrecipHintKind::kDry);
}

// Przypadki brzegowe, dopisane przy wdrożeniu.

TEST(ChoosePrecipHint, RadarExactlyAtStaleThresholdIsStillRadarNow) {
  EXPECT_EQ(choosePrecipHint(true, 1, kStaleS, 0, -1), PrecipHintKind::kRadarNow);
}

TEST(ChoosePrecipHint, RadarOneSecondPastStaleThresholdFallsBack) {
  EXPECT_EQ(choosePrecipHint(true, 1, kStaleS + 1, 85, 22),
            PrecipHintKind::kForecastPeak);
}

TEST(ChoosePrecipHint, RadarTakesPriorityEvenWhenForecastAlsoPredictsRain) {
  // Radar swiezy I opad, a prognoza TEZ ma wysoki szczyt -> radar wygrywa (patrz
  // brief: radar widzi TERAZNIEJSZOSC i ma glos pierwszenstwa).
  EXPECT_EQ(choosePrecipHint(true, 3, 60, 90, 18), PrecipHintKind::kRadarNow);
}

TEST(ChoosePrecipHint, InvalidRadarWithLowForecastIsDry) {
  EXPECT_EQ(choosePrecipHint(false, 0, 0, 10, -1), PrecipHintKind::kDry);
}

TEST(ChoosePrecipHint, ZeroAgeFreshRadarIsRadarNow) {
  // radarTrueAgeS == 0 (pobrane w tej samej sekundzie) nie ma prawa wypasc jako
  // "za stare" - graniczny przypadek przy starcie liczenia wieku.
  EXPECT_EQ(choosePrecipHint(true, 1, 0, 0, -1), PrecipHintKind::kRadarNow);
}
