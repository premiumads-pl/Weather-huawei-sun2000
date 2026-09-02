// Testy dla mapowania trybow ladowania — AutoData.h.
#include "AutoData.h"

#include <gtest/gtest.h>

#include <cstdio>

TEST(AutoModeMapping, RoundTripsForEveryValidIndex) {
  for (int i = 0; i < kAutoModeCount; ++i) {
    const char* mqtt = autoModeMqtt(i);
    EXPECT_EQ(autoModeIndex(mqtt), i) << "index " << i << " -> \"" << mqtt << "\"";
  }
}

TEST(AutoModeMapping, MqttStringsMatchKnownContract) {
  // Kontrakt z automatyka w garazu (komentarz AutoData.h linia ~124) — te
  // napisy ida po drucie i NIE WOLNO ich zmienic bez zmiany po drugiej stronie.
  EXPECT_STREQ(autoModeMqtt(0), "OFF");
  EXPECT_STREQ(autoModeMqtt(1), "PV");
  EXPECT_STREQ(autoModeMqtt(2), "PV+MIN");
  EXPECT_STREQ(autoModeMqtt(3), "MAX");
}

TEST(AutoModeMapping, UnknownIndexReturnsEmptyMqttString) {
  // Cialo autoModeMqtt(): default -> "" (pusty napis, nie nullptr).
  EXPECT_STREQ(autoModeMqtt(-1), "");
  EXPECT_STREQ(autoModeMqtt(kAutoModeCount), "");
  EXPECT_STREQ(autoModeMqtt(99), "");
}

TEST(AutoModeMapping, UnknownStringReturnsMinusOneSentinel) {
  // Cialo autoModeIndex() (linia ~152-156): petla po kAutoModeCount, brak
  // dopasowania -> return -1. To jest wartosc sentinel "nie znaleziono".
  EXPECT_EQ(autoModeIndex("nieistniejacy"), -1);
  EXPECT_EQ(autoModeIndex("off"), -1);  // wielkosc liter ma znaczenie (strcmp)
}

TEST(AutoModeMapping, NullOrEmptyStringReturnsMinusOne) {
  EXPECT_EQ(autoModeIndex(nullptr), -1);
  EXPECT_EQ(autoModeIndex(""), -1);
}

TEST(AutoModeLabel, KnownIndexesReturnPolishLabels) {
  EXPECT_STREQ(autoModeLabel(0), "STOP");
  EXPECT_STREQ(autoModeLabel(1), "TYLKO SŁOŃCE");
  EXPECT_STREQ(autoModeLabel(2), "SŁOŃCE + MIN.");
  EXPECT_STREQ(autoModeLabel(3), "CAŁA NAPRZÓD");
}

TEST(AutoModeLabel, UnknownIndexReturnsDash) {
  EXPECT_STREQ(autoModeLabel(-1), "-");
  EXPECT_STREQ(autoModeLabel(kAutoModeCount), "-");
}

TEST(AutoStateLabel, KnownTechnicalStatesMapToPolishLabels) {
  AutoModel a;

  std::snprintf(a.state, sizeof(a.state), "%s", "laduje");
  EXPECT_STREQ(autoStateLabel(a), "ładuje");

  std::snprintf(a.state, sizeof(a.state), "%s", "czeka");
  EXPECT_STREQ(autoStateLabel(a), "czeka");

  std::snprintf(a.state, sizeof(a.state), "%s", "stoi");
  EXPECT_STREQ(autoStateLabel(a), "postój");

  std::snprintf(a.state, sizeof(a.state), "%s", "spi");
  EXPECT_STREQ(autoStateLabel(a), "śpi");

  std::snprintf(a.state, sizeof(a.state), "%s", "brak");
  EXPECT_STREQ(autoStateLabel(a), "brak kabla");
}

TEST(AutoStateLabel, UnknownStateReturnsDash) {
  AutoModel a;
  std::snprintf(a.state, sizeof(a.state), "%s", "cokolwiek");
  EXPECT_STREQ(autoStateLabel(a), "-");
}
