// Testy dla Format.h — fmt1/fmt2, polski przecinek dziesietny zamiast kropki.
#include "Format.h"

#include <gtest/gtest.h>

#include <cstring>

TEST(Fmt1, OneDecimalPlaceWithPolishComma) {
  char buf[32];
  fmt1(buf, sizeof(buf), 3.14f);
  EXPECT_STREQ(buf, "3,1");
}

TEST(Fmt1, IntegerValueKeepsOneDecimalZero) {
  char buf[32];
  fmt1(buf, sizeof(buf), 5.0f);
  EXPECT_STREQ(buf, "5,0");
}

TEST(Fmt1, NegativeValue) {
  char buf[32];
  fmt1(buf, sizeof(buf), -3.14f);
  EXPECT_STREQ(buf, "-3,1");
}

TEST(Fmt1, Zero) {
  char buf[32];
  fmt1(buf, sizeof(buf), 0.f);
  EXPECT_STREQ(buf, "0,0");
}

TEST(Fmt2, TwoDecimalPlacesAlwaysShownEvenWithTrailingZero) {
  char buf[32];
  fmt2(buf, sizeof(buf), 4.8f);
  // WAZNE (patrz komentarz Format.h): kwoty ZAWSZE maja dwa miejsca po
  // przecinku, nawet z koncowym zerem -- "4,80", nie "4,8".
  EXPECT_STREQ(buf, "4,80");
}

TEST(Fmt2, NegativeValue) {
  char buf[32];
  fmt2(buf, sizeof(buf), -1.5f);
  EXPECT_STREQ(buf, "-1,50");
}

TEST(Fmt2, Zero) {
  char buf[32];
  fmt2(buf, sizeof(buf), 0.f);
  EXPECT_STREQ(buf, "0,00");
}

TEST(Fmt2, RoundsToTwoDecimals) {
  char buf[32];
  fmt2(buf, sizeof(buf), 1.005f);  // w binarnym float 1.005 jest ciut ponizej -> "1.00" albo "1.01"
  // Nie zgadujemy kierunku zaokraglenia binarnego, sprawdzamy tylko KSZTALT wyjscia:
  // dokladnie jeden przecinek, dokladnie dwie cyfry po nim.
  const char* comma = nullptr;
  for (const char* p = buf; *p; ++p) {
    if (*p == ',') { comma = p; break; }
  }
  ASSERT_NE(comma, nullptr);
  EXPECT_EQ(std::strlen(comma + 1), 2u);
}
