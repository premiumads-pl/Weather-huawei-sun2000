// Testy dla wxTimeToMinutes() i pvMayBeAsleep() — WeatherData.h, linie ~67-117.
// (Brief mowil o "PvSleep.h", ale taki plik nie istnieje — obie funkcje
// mieszkaja w WeatherData.h, patrz komentarz nad pvMayBeAsleep, linie 90-98.)
#include "WeatherData.h"

#include <gtest/gtest.h>

// --------------------------- wxTimeToMinutes -------------------------------

TEST(WxTimeToMinutes, ParsesValidTime) {
  EXPECT_EQ(wxTimeToMinutes("04:32"), 4 * 60 + 32);
  EXPECT_EQ(wxTimeToMinutes("00:00"), 0);
  EXPECT_EQ(wxTimeToMinutes("23:59"), 23 * 60 + 59);
}

TEST(WxTimeToMinutes, EmptyOrNullReturnsMinusOne) {
  EXPECT_EQ(wxTimeToMinutes(""), -1);
  EXPECT_EQ(wxTimeToMinutes(nullptr), -1);
}

TEST(WxTimeToMinutes, MalformedStringReturnsMinusOne) {
  EXPECT_EQ(wxTimeToMinutes("abc"), -1);     // brak ':' na pozycji 2
  EXPECT_EQ(wxTimeToMinutes("1234"), -1);    // brak ':'
}

TEST(WxTimeToMinutes, OutOfRangeHourOrMinuteReturnsMinusOne) {
  EXPECT_EQ(wxTimeToMinutes("24:00"), -1);  // godzina spoza 0..23
  EXPECT_EQ(wxTimeToMinutes("23:60"), -1);  // minuta spoza 0..59
}

// --------------------------- pvMayBeAsleep ----------------------------------
// Okno snu = [zachod+30min, wschod+30min], z zawijaniem przez polnoc.

TEST(PvMayBeAsleep, MiddleOfNightIsAsleep) {
  // zachod 20:00, wschod 05:00, teraz 02:00 -> srodek nocy.
  EXPECT_TRUE(pvMayBeAsleep("05:00", "20:00", 2 * 60));
}

TEST(PvMayBeAsleep, MiddayIsNotAsleep) {
  EXPECT_FALSE(pvMayBeAsleep("05:00", "20:00", 12 * 60));
}

TEST(PvMayBeAsleep, ExactBoundaryJustBeforeWindowStartIsAwake) {
  // start okna = zachod(20:00=1200) + 30 = 1230. Zachod+29 = 1229 -> jeszcze PRZED oknem.
  EXPECT_FALSE(pvMayBeAsleep("05:00", "20:00", 1200 + 29));
}

TEST(PvMayBeAsleep, ExactBoundaryJustAfterWindowStartIsAsleep) {
  // Zachod+31 = 1231 >= start(1230) -> juz W oknie.
  EXPECT_TRUE(pvMayBeAsleep("05:00", "20:00", 1200 + 31));
}

TEST(PvMayBeAsleep, MissingSunriseReturnsFalse) {
  EXPECT_FALSE(pvMayBeAsleep("", "20:00", 100));
}

TEST(PvMayBeAsleep, MissingSunsetReturnsFalse) {
  EXPECT_FALSE(pvMayBeAsleep("05:00", "", 100));
}

TEST(PvMayBeAsleep, MissingNowMinutesReturnsFalse) {
  EXPECT_FALSE(pvMayBeAsleep("05:00", "20:00", -1));
}

// --- galaz "start < end": margines przenosi poczatek okna ZA polnoc --------
// Zachod bardzo pozny (23:50=1430): start=(1430+30)%1440=20 (00:20), a
// wschod 05:00 -> end=(300+30)%1440=330 (05:30). start(20) < end(330), wiec
// funkcja bierze branch "Zachod tak pozny ze margines przeniosl poczatek
// okna za polnoc" (WeatherData.h, linia ~111-113): okno NIE zawija sie przez
// polnoc, tylko jest zwyklym przedzialem [20, 330).
TEST(PvMayBeAsleep, LateSunsetPushesWindowStartPastMidnight_InsideWindow) {
  EXPECT_TRUE(pvMayBeAsleep("05:00", "23:50", 25));    // 00:25, wewnatrz [20,330)
}

TEST(PvMayBeAsleep, LateSunsetPushesWindowStartPastMidnight_BeforeWindow) {
  EXPECT_FALSE(pvMayBeAsleep("05:00", "23:50", 10));   // 00:10, przed startem okna (20)
}

TEST(PvMayBeAsleep, LateSunsetPushesWindowStartPastMidnight_AfterWindow) {
  EXPECT_FALSE(pvMayBeAsleep("05:00", "23:50", 400));  // po koncu okna (330)
}
