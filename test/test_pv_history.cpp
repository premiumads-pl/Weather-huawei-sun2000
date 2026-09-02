// Testy dla PvHistory — PvData.h, linia ~300 (profil dobowy PV, 144 sloty po 10 min).
#include "PvData.h"

#include <gtest/gtest.h>

TEST(PvHistoryPush, WritesIntoCorrectSlot) {
  PvHistory h;
  h.reset(10);

  // hour=1, minute=23 -> slot = (60+23)/10 = 8
  h.push(10, 1, 23, 555, 111);

  EXPECT_EQ(h.watts[8], 555);
  EXPECT_EQ(h.load[8], 111);
  EXPECT_TRUE(h.filled[8]);
  // Sasiednie sloty pozostaja puste.
  EXPECT_FALSE(h.filled[7]);
  EXPECT_FALSE(h.filled[9]);
}

TEST(PvHistoryPush, DifferentDayTriggersResetBeforeWriting) {
  PvHistory h;
  h.reset(10);
  h.push(10, 5, 0, 1000, 500);  // slot 30, dzien 10
  ASSERT_TRUE(h.filled[30]);

  // Nowy dzien -> stary slot ma zniknac (reset), a nowa probka wejsc na wlasciwy slot.
  h.push(11, 0, 0, 42, 7);  // slot 0, dzien 11

  EXPECT_EQ(h.day, 11);
  EXPECT_FALSE(h.filled[30]);  // wyczyszczone przez reset()
  EXPECT_TRUE(h.filled[0]);
  EXPECT_EQ(h.watts[0], 42);
  EXPECT_EQ(h.load[0], 7);
}

TEST(PvHistoryPush, OutOfRangeSlotIsIgnoredWithoutCrash) {
  PvHistory h;
  h.reset(10);
  // hour=24 -> (24*60+0)/10 = 144, poza SLOTS(144) -> push ma nic nie zrobic.
  h.push(10, 24, 0, 999, 999);
  for (int i = 0; i < PvHistory::SLOTS; ++i) {
    EXPECT_FALSE(h.filled[i]) << "slot " << i;
  }
}

TEST(PvHistoryReset, ClearsAllSlotsAndSetsDay) {
  PvHistory h;
  h.push(3, 10, 0, 100, 50);  // dzien 3, jakikolwiek slot
  h.reset(7);

  EXPECT_EQ(h.day, 7);
  for (int i = 0; i < PvHistory::SLOTS; ++i) {
    EXPECT_EQ(h.watts[i], 0);
    EXPECT_EQ(h.load[i], 0);
    EXPECT_FALSE(h.filled[i]);
  }
}

TEST(PvHistoryClampW, NegativeClampsToZero) {
  EXPECT_EQ(PvHistory::clampW(-1), 0u);
  EXPECT_EQ(PvHistory::clampW(-100000), 0u);
}

TEST(PvHistoryClampW, OverflowClampsTo65535) {
  EXPECT_EQ(PvHistory::clampW(65536), 65535u);
  EXPECT_EQ(PvHistory::clampW(1000000), 65535u);
}

TEST(PvHistoryClampW, InRangeValuePassesThrough) {
  EXPECT_EQ(PvHistory::clampW(0), 0u);
  EXPECT_EQ(PvHistory::clampW(65535), 65535u);
  EXPECT_EQ(PvHistory::clampW(1234), 1234u);
}

TEST(PvHistoryPeak, ReturnsMaxAcrossBothSeriesIgnoringUnfilled) {
  PvHistory h;
  h.reset(1);
  h.push(1, 0, 0, 100, 900);   // load wiekszy niz watts
  h.push(1, 0, 10, 2000, 50);  // watts wiekszy niz load
  EXPECT_EQ(h.peak(), 2000u);
}

TEST(PvHistoryPeak, EmptyHistoryPeaksAtZero) {
  PvHistory h;
  h.reset(1);
  EXPECT_EQ(h.peak(), 0u);
}
