// Testy dla RoomHistory — RoomHistory.h (historia temperatur z czujnikow BLE,
// okno ruchome 24h / 10 min, 6 pokoi).
#include "RoomHistory.h"

#include <gtest/gtest.h>

TEST(RoomHistoryPush, WritesScaledTemperatureAtHeadWhenPresent) {
  RoomHistory rh;
  rh.reset();  // ustawia caly bufor na NO_T i head=0

  rh.push(2, /*hasT=*/true, 21.7f);

  // t10 = temperatura * 10, obcieta do int16_t.
  EXPECT_EQ(rh.t10[2][rh.head], 217);
}

TEST(RoomHistoryPush, LeavesNoTWhenReadingAbsent) {
  RoomHistory rh;
  rh.reset();

  rh.push(3, /*hasT=*/false, 99.9f);  // wartosc ma byc zignorowana

  EXPECT_EQ(rh.t10[3][rh.head], RoomHistory::NO_T);
}

TEST(RoomHistoryPush, HandlesNegativeTemperature) {
  RoomHistory rh;
  rh.reset();

  rh.push(0, true, -5.3f);

  EXPECT_EQ(rh.t10[0][rh.head], -53);
}

TEST(RoomHistoryPush, IgnoresOutOfRangeRoomIndexWithoutCrashing) {
  RoomHistory rh;
  rh.reset();

  // Ani ujemny indeks, ani indeks >= ROOMS nie moze dotknac pamieci poza tablica.
  rh.push(-1, true, 10.f);
  rh.push(RoomHistory::ROOMS, true, 10.f);
  rh.push(RoomHistory::ROOMS + 5, true, 10.f);

  // Reszta bufora ma zostac nietknieta (NO_T wszedzie).
  for (int r = 0; r < RoomHistory::ROOMS; ++r) {
    EXPECT_EQ(rh.t10[r][rh.head], RoomHistory::NO_T) << "room " << r;
  }
}

TEST(RoomHistoryReset, ClearsAllRoomsToNoTAndZeroesHead) {
  RoomHistory rh;
  rh.head = 5;
  rh.push(1, true, 20.f);

  rh.reset();

  EXPECT_EQ(rh.head, 0);
  EXPECT_EQ(rh.lastSlot, 0u);
  for (int r = 0; r < RoomHistory::ROOMS; ++r) {
    for (int i = 0; i < RoomHistory::SLOTS; ++i) {
      EXPECT_EQ(rh.t10[r][i], RoomHistory::NO_T);
    }
  }
}

TEST(RoomHistoryClearSlot, ClearsOnlyGivenSlotAcrossAllRooms) {
  RoomHistory rh;
  rh.reset();
  for (int r = 0; r < RoomHistory::ROOMS; ++r) rh.t10[r][4] = 100 + r;
  for (int r = 0; r < RoomHistory::ROOMS; ++r) rh.t10[r][5] = 200 + r;

  rh.clearSlot(4);

  for (int r = 0; r < RoomHistory::ROOMS; ++r) {
    EXPECT_EQ(rh.t10[r][4], RoomHistory::NO_T) << "room " << r;
    EXPECT_EQ(rh.t10[r][5], 200 + r) << "room " << r;  // nietkniete
  }
}
