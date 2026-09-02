// Test dla kPaybackHist — PaybackHist.h. To jest test na DANE, nie na kod:
// tablica jest opisana jako SKUMULOWANA (kazda wartosc >= poprzedniej), wiec
// blednie dopisany, malejacy punkt ma tu zostac zlapany.
#include "PaybackHist.h"

#include <gtest/gtest.h>

TEST(PaybackHist, HasAtLeastTwoPoints) {
  EXPECT_GE(kPaybackHistN, 2);
}

TEST(PaybackHist, IsMonotonicallyNonDecreasing) {
  for (int i = 1; i < kPaybackHistN; ++i) {
    EXPECT_GE(kPaybackHist[i], kPaybackHist[i - 1])
        << "kPaybackHist[" << i << "]=" << kPaybackHist[i]
        << " < kPaybackHist[" << (i - 1) << "]=" << kPaybackHist[i - 1]
        << " -- skumulowana historia nie moze maleic";
  }
}

TEST(PaybackHist, MonthZeroIsValidCalendarMonth) {
  EXPECT_GE(kPaybackHistMonth0, 1);
  EXPECT_LE(kPaybackHistMonth0, 12);
}
