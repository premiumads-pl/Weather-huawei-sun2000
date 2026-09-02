// Testy dla AirHistory — AirHistory.h (historia jakosci powietrza, 7 dni,
// srednia dobowa PM2.5/PM10 + maksimum indeksu ARMAAG).
//
// UWAGA: plik deklaruje `extern AirHistory gAirHistory;` ale NIE definiuje go
// (definicja zyje w pogoda-gdynia.ino). Testy tworza WLASNE instancje AirHistory
// na stosie i nigdy nie odwoluja sie do globalu — inaczej linker host'owy nie
// mialby czym rozwiazac symbolu gAirHistory.
#include "AirHistory.h"

#include <gtest/gtest.h>

// --- roundPm: zaokraglanie do najblizszej liczby calkowitej, clamp do [0,32767] ---

TEST(AirHistoryRoundPm, RoundsToNearest) {
  EXPECT_EQ(AirHistory::roundPm(3.4f), 3);
  EXPECT_EQ(AirHistory::roundPm(3.6f), 4);
  EXPECT_EQ(AirHistory::roundPm(3.5f), 4);  // lroundf: polowki od zera
}

TEST(AirHistoryRoundPm, NegativeClampsToZero) {
  EXPECT_EQ(AirHistory::roundPm(-2.f), 0);
  EXPECT_EQ(AirHistory::roundPm(-0.1f), 0);
}

TEST(AirHistoryRoundPm, OverflowClampsTo32767) {
  EXPECT_EQ(AirHistory::roundPm(40000.f), 32767);
}

TEST(AirHistoryRoundPm, ZeroStaysZero) {
  EXPECT_EQ(AirHistory::roundPm(0.f), 0);
}

// --- pelny cykl: advance -> push -> freezeCurrent (wewn.) -> advance -> reset ---

TEST(AirHistoryCycle, PushImmediatelyFreezesTodaysSlot) {
  AirHistory ah;
  ah.reset();

  const uint32_t day1 = 20000UL * 86400UL;  // dowolny "dzien" >= progu NTP
  ASSERT_TRUE(ah.advance(day1));

  ah.push(10.f, 20.f, 3);  // jedna probka: PM2.5=10, PM10=20, indeks=3

  // push() wola freezeCurrent() wewnatrz, wiec "dzis" (head) jest widoczne od razu.
  EXPECT_EQ(ah.pm25At(AirHistory::DAYS - 1), 10);
  EXPECT_EQ(ah.pm10At(AirHistory::DAYS - 1), 20);
  EXPECT_EQ(ah.idxAt(AirHistory::DAYS - 1), 3);
}

TEST(AirHistoryCycle, AverageOfMultipleSamplesAndMaxIndex) {
  AirHistory ah;
  ah.reset();
  const uint32_t day1 = 20000UL * 86400UL;
  ASSERT_TRUE(ah.advance(day1));

  ah.push(10.f, 20.f, 2);
  ah.push(20.f, 30.f, 5);  // najwyzszy indeks dnia
  ah.push(30.f, 10.f, 1);

  // Srednia PM2.5 = (10+20+30)/3 = 20; PM10 = (20+30+10)/3 = 20.
  EXPECT_EQ(ah.pm25At(AirHistory::DAYS - 1), 20);
  EXPECT_EQ(ah.pm10At(AirHistory::DAYS - 1), 20);
  EXPECT_EQ(ah.idxAt(AirHistory::DAYS - 1), 5);  // maksimum, nie ostatnia probka
}

TEST(AirHistoryCycle, AdvanceToNextDayFreezesYesterdayAndClearsToday) {
  AirHistory ah;
  ah.reset();
  const uint32_t day1 = 20000UL * 86400UL;
  ASSERT_TRUE(ah.advance(day1));
  ah.push(10.f, 20.f, 3);

  const uint32_t day2 = day1 + 86400UL;
  ASSERT_TRUE(ah.advance(day2));

  // "Dzis" (day2) jeszcze bez probek -> dziura.
  EXPECT_EQ(ah.pm25At(AirHistory::DAYS - 1), AirHistory::NO_V);
  EXPECT_EQ(ah.idxAt(AirHistory::DAYS - 1), AirHistory::NO_IDX);
  // "Wczoraj" (day1) zostaje zamrozone z akumulatorow sprzed przewiniecia.
  EXPECT_EQ(ah.pm25At(AirHistory::DAYS - 2), 10);
  EXPECT_EQ(ah.pm10At(AirHistory::DAYS - 2), 20);
  EXPECT_EQ(ah.idxAt(AirHistory::DAYS - 2), 3);
}

TEST(AirHistoryAdvance, BeforeNtpThresholdReturnsFalseAndDoesNothing) {
  AirHistory ah;
  ah.reset();
  EXPECT_FALSE(ah.advance(100));  // 100 << 1700000000
  EXPECT_EQ(ah.lastDay, 0u);
}

TEST(AirHistoryAdvance, GapLongerThanWeekResetsWholeHistory) {
  AirHistory ah;
  ah.reset();
  const uint32_t day1 = 20000UL * 86400UL;
  ASSERT_TRUE(ah.advance(day1));
  ah.push(10.f, 20.f, 3);

  const uint32_t muchLater = day1 + 30UL * 86400UL;  // 30 dni przerwy > DAYS(7)
  ASSERT_TRUE(ah.advance(muchLater));

  for (int i = 0; i < AirHistory::DAYS; ++i) {
    EXPECT_EQ(ah.pm25At(i), AirHistory::NO_V) << "i=" << i;
  }
}

TEST(AirHistoryPush, DoesNothingBeforeFirstAdvance) {
  AirHistory ah;
  ah.reset();  // lastDay == 0, advance() nigdy nie wolane
  ah.push(10.f, 20.f, 3);
  // Brak advance() -> push() ma nic nie robic (patrz "if (lastDay == 0) return;").
  EXPECT_EQ(ah.accN, 0u);
}
