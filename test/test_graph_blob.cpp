// Testy dla GraphBlob — GraphBlob.h (trwalosc wykresu mocy ladowania OLED w NVS, v194).
#include "GraphBlob.h"

#include <gtest/gtest.h>

TEST(GraphBlobLayout, SizeMatchesNvsBudgetOf136Bytes) {
  // To samo, co static_assert w GraphBlob.h, ale w runtime GTest — jesli
  // ktos kiedys zmieni uklad struktury bez podniesienia `ver`/klucza NVS,
  // ten test (i static_assert obok niego) maja to zlapac przy kompilacji.
  EXPECT_EQ(sizeof(GraphBlob), 136u);
}

TEST(GraphBlobDefaults, FreshInstanceHasVersionOneAndIsOtherwiseZeroed) {
  GraphBlob g;

  EXPECT_EQ(g.ver, 1);
  EXPECT_EQ(g.cnt, 0);
  EXPECT_EQ(g.max, 0);
  EXPECT_EQ(g.charging, 0);
  EXPECT_EQ(g.lastEpoch, 0u);
  for (int i = 0; i < 128; ++i) {
    EXPECT_EQ(g.s[i], 0) << "index " << i;
  }
}
