// Testy dla CostModel::sellGr — CostData.h, pole uint16_t (grosze), linia ~99.
//
// POMINIETE: test granic przycinania ("SellGrBounds"). CostData.h zostal
// przeczytany w calosci (146 linii) i NIE ZAWIERA zadnej inline funkcji/metody,
// ktora przycina (clamp) wartosc sellGr przy konstrukcji z groszy/zlotych —
// jest to zwykle pole struktury, bez logiki. Komentarz nad polem (linie 73-98)
// mowi wprost, ze wartosc "jest juz po korekcie" i przychodzi GOTOWA z Home
// Assistanta przez MqttClient.cpp (ktory PARSUJE JSON z MQTT i NIE jest
// host-compilowalny — zalezy od PubSubClient/WiFi/ArduinoJson). Ewentualne
// przycinanie zyloby wiec wylacznie w MqttClient.cpp, poza zasiegiem testow
// hosta. Zamiast zmyslonego testu granic, ponizej sa testy na sama STRUKTURE:
// wartosc domyslna i to, ze typowe/graniczne kwoty miesza sie w uint16_t bez
// utraty precyzji.
#include "CostData.h"

#include <gtest/gtest.h>

TEST(CostModelSellGr, DefaultsToZero) {
  CostModel c;
  EXPECT_EQ(c.sellGr, 0u);
}

TEST(CostModelSellGr, HoldsTypicalDailyAmountWithoutPrecisionLoss) {
  CostModel c;
  c.sellGr = 4187;  // 41,87 zl
  EXPECT_EQ(c.sellGr, 4187u);
}

TEST(CostModelSellGr, HoldsMaximumUint16WithoutOverflow) {
  CostModel c;
  // Komentarz w naglowku: sufit fizyczny ok. 41 zl/dobe (4100 gr) przy
  // najwyzszej stawce RCEm z faktur, z ponad 15-krotnym zapasem do 65535.
  c.sellGr = 65535;
  EXPECT_EQ(c.sellGr, 65535u);
}

TEST(CostModelSellGr, ZeroIsValidValueNotASentinel) {
  // W odroznieniu od np. PvSnapshot::meterExportKwh (-1.f jako wartownik),
  // sellGr nie ma wartownika: 0 to zwykly, prawdziwy "brak sprzedazy dzis".
  CostModel c;
  c.sellGr = 0;
  EXPECT_EQ(c.sellGr, 0u);
}
