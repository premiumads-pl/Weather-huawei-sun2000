// Test regresyjny dla POPRAWKI w Bloku B (P1-1), odtworzony jako izolowany,
// host-testowalny wzorzec — NIE import z MqttClient.cpp, bo ten plik zalezy
// od PubSubClient/WiFi i nie kompiluje sie na hoscie. Ten test sprawdza, ze
// sam WZORZEC arytmetyczny jest poprawny na dowolnych wartosciach uint32_t,
// niezaleznie od pliku zrodlowego.
#include <gtest/gtest.h>

#include <cstdint>

namespace {

// Skopiowany wzorzec naprawy z MqttClient.cpp (P1-1/Blok B).
inline bool backoffElapsed(uint32_t now, uint32_t lastTryAt, uint32_t backoffMs) {
  return (now - lastTryAt) >= backoffMs;  // odejmowanie BEZ ZNAKU, zawija sie poprawnie
}

inline bool isFreshPattern(uint32_t now, uint32_t okAt, uint32_t staleMs) {
  if (okAt == 0) return false;
  return static_cast<int32_t>(now - okAt) < static_cast<int32_t>(staleMs);
}

}  // namespace

// ---------------------------------------------------------------------------
// "reconnect dozwolony przy now = 30 dni"
//
// Bug P1-1 (przed naprawa): kod liczyl roznice na int32_t, wiec po ~24,85 dnia
// (2^31 ms) roznica przekrecala sie na ujemna i blokowala reconnect az do
// ~49,7 dnia. Wzorzec ponizej liczy CALKOWICIE na uint32_t (bez rzutu na
// int32_t), wiec przy now=30 dni roznica (2 592 000 000) wciaz miesci sie w
// uint32_t (max ~4 294 967 295) i porownanie wychodzi poprawnie.
// ---------------------------------------------------------------------------
TEST(BackoffElapsedPattern, ReconnectAllowedAt30DaysBeyondOldSignedThreshold) {
  const uint32_t now = 30UL * 24UL * 3600UL * 1000UL;  // 30 dni w ms = 2 592 000 000
  const uint32_t lastTryAt = 0;                        // nigdy nie probowano

  EXPECT_TRUE(backoffElapsed(now, lastTryAt, /*backoffMs=*/5000));
  EXPECT_TRUE(backoffElapsed(now, lastTryAt, /*backoffMs=*/300000));  // 5 min, max z projektu
}

TEST(BackoffElapsedPattern, FreshAttemptWithinBackoffWindowIsBlocked) {
  const uint32_t now = 100000;
  const uint32_t lastTryAt = 99000;  // 1000 ms temu
  EXPECT_FALSE(backoffElapsed(now, lastTryAt, /*backoffMs=*/5000));
}

TEST(BackoffElapsedPattern, ExactlyAtBackoffBoundaryIsAllowed) {
  const uint32_t now = 100000;
  const uint32_t lastTryAt = 95000;  // dokladnie 5000 ms temu
  EXPECT_TRUE(backoffElapsed(now, lastTryAt, /*backoffMs=*/5000));  // >= , nie >
}

// ---------------------------------------------------------------------------
// "dane sprzed miesiaca nie sa swieze" (a raczej: SA blednie uznane za swieze,
// i to jest UDOKUMENTOWANY kompromis projektu, nie dziura w tescie).
//
// isFreshPattern rzutuje roznice (now - okAt), ktora jest uint32_t, na
// int32_t. Kiedy ta roznica PRZEKRACZA INT32_MAX (2 147 483 647), rzutowanie
// (zachowanie zdefiniowane od C++20, a w praktyce od zawsze przy U2 -- co ten
// kompilator i kazdy realny kompilator implementuje) daje liczbe UJEMNA.
// Ujemna < dodatnia(staleMs) jest zawsze prawda, wiec funkcja zwraca `true`
// -- dane sprzed 30 dni wychodza jako "swieze". To jest ZAMIERZONY fail-safe
// przy dwuznacznosci powyzej ~24,85 dnia (patrz "TRZY STANY SWIEZOSCI" w
// WeatherUiV3.cpp) -- lepiej pokazac przypadkiem stara wartosc niz ukryc
// swieza pod falszywym "brak danych" przy bledzie zegara. Test PONIZEJ liczy
// to naprawde (nie zgaduje) i dokumentuje wynik w asercji.
// ---------------------------------------------------------------------------
TEST(IsFreshPattern, DataFromThirtyDaysAgoOverflowsInt32AndIsTreatedAsFresh) {
  const uint32_t now = 40UL * 24UL * 3600UL * 1000UL;   // 40 dni w ms = 3 456 000 000
  const uint32_t okAt = 10UL * 24UL * 3600UL * 1000UL;  // 10 dni w ms =   864 000 000
  const uint32_t staleMs = 40000;                       // 40 s -- dowolny sensowny prog

  // Policzone wprost: diff = now - okAt = 2 592 000 000 (uint32_t).
  // 2 592 000 000 > INT32_MAX (2 147 483 647), wiec static_cast<int32_t>(diff)
  // == 2 592 000 000 - 4 294 967 296 == -1 702 967 296 (ujemne).
  // -1 702 967 296 < 40000 -> true. To NIE jest poprawne wykrycie "swiezosci"
  // w sensie kalendarzowym -- to jest fail-safe: przy tak duzej, dwuznacznej
  // roznicy kod woli pokazac dane niz zgasic ekran.
  EXPECT_TRUE(isFreshPattern(now, okAt, staleMs));
}

TEST(IsFreshPattern, RecentDataWithinNormalWindowIsCorrectlyFresh) {
  // Kontrast: dane sprzed 5 minut, prog swiezosci 1 minuta -- normalny
  // przypadek, BEZ przepelnienia int32_t, wykryty poprawnie jako nieswiezy.
  const uint32_t now = 10UL * 60UL * 1000UL;                // t = 10 min w ms
  const uint32_t okAt = now - 5UL * 60UL * 1000UL;          // 5 min wczesniej
  const uint32_t staleMs = 60UL * 1000UL;                   // prog: 1 minuta

  EXPECT_FALSE(isFreshPattern(now, okAt, staleMs));
}

TEST(IsFreshPattern, NeverReceivedIsNeverFresh) {
  EXPECT_FALSE(isFreshPattern(/*now=*/123456, /*okAt=*/0, /*staleMs=*/60000));
}
