// Test regresyjny dla POPRAWKI w Bloku B (P1-1).
//
// Dwie różne rzeczy, dwa różne podejścia:
//  - backoff MQTT (gLastTryAt/gBackoffMs) żyje wyłącznie w MqttClient.cpp, który
//    zależy od PubSubClient/WiFi i nie kompiluje się na hoście — wzorzec arytmetyczny
//    jest więc odtworzony tu jako ŚWIADOMY, KONIECZNY duplikat (nie da się inaczej).
//  - świeżość (isFresh) ma dziś realne, host-kompilowalne źródło w Freshness.h —
//    ten plik WŁĄCZA je wprost i testuje PRAWDZIWĄ funkcję produkcyjną, żadnej kopii.
//    (poprawka po przeglądzie: pierwsza wersja tego pliku duplikowała też isFresh()
//    lokalnie, co znaczyło, że poprawka w Freshness.h nigdy by tego testu nie zabarwiła
//    na czerwono — patrz historia commitów).
#include <gtest/gtest.h>

#include <cstdint>

#include "../Freshness.h"

namespace {

// Skopiowany wzorzec naprawy z MqttClient.cpp (P1-1/Blok B) — MUSI zostać duplikatem,
// bo MqttClient.cpp nie jest host-compilowalny (PubSubClient/WiFi).
inline bool backoffElapsed(uint32_t now, uint32_t lastTryAt, uint32_t backoffMs) {
  return (now - lastTryAt) >= backoffMs;  // odejmowanie BEZ ZNAKU, zawija sie poprawnie
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
// isFresh() — TESTUJE PRAWDZIWY KOD PRODUKCYJNY z Freshness.h, nie kopie.
//
// (poprawka po przeglądzie) Pierwsza wersja tego testu duplikowała formułę isFresh()
// SPRZED poprawki (rzutowanie na int32_t) i asercją `EXPECT_TRUE` na danych sprzed
// 30 dni UTRWALAŁA błąd zamiast go łapać — brief żądał testu "dane sprzed miesiąca
// NIE są świeże", a powstała jego dokładna odwrotność. Po poprawce w Freshness.h
// (wąskie okno tolerancji ~2 s na wyścig odczytu między rdzeniami, zamiast
// blankietowego "cały wiek > 2^31 ms to świeże") test poniżej sprawdza both kierunki
// naprawdę: dane sprzed miesiąca są stare, a stempel kilkaset ms w przyszłości
// (typowy wyścig odczytu) nadal liczy się jako świeży.
// ---------------------------------------------------------------------------
TEST(IsFreshPattern, DataFromThirtyDaysAgoIsCorrectlyStale) {
  const uint32_t now = 40UL * 24UL * 3600UL * 1000UL;   // 40 dni w ms = 3 456 000 000
  const uint32_t okAt = 10UL * 24UL * 3600UL * 1000UL;  // 10 dni w ms =   864 000 000
  const uint32_t staleMs = 40000;                       // 40 s -- dowolny sensowny prog

  // Wiek = 2 592 000 000 ms (30 dni) — daleko poza jakimkolwiek sensownym staleMs
  // I daleko poza oknem tolerancji na wyscig odczytu (blisko UINT32_MAX). Musi
  // wyjsc jako NIEswieze.
  EXPECT_FALSE(isFresh(now, okAt, staleMs));
}

TEST(IsFreshPattern, DataOlderThanThresholdIsCorrectlyStaleInNormalWindow) {
  // Dane sprzed 5 minut, prog swiezosci 1 minuta -- normalny przypadek (bez
  // zadnego przepelnienia arytmetyki), poprawnie wykryty jako NIEswiezy.
  const uint32_t now = 10UL * 60UL * 1000UL;                // t = 10 min w ms
  const uint32_t okAt = now - 5UL * 60UL * 1000UL;          // 5 min wczesniej
  const uint32_t staleMs = 60UL * 1000UL;                   // prog: 1 minuta

  EXPECT_FALSE(isFresh(now, okAt, staleMs));
}

TEST(IsFreshPattern, DataWithinStaleThresholdIsFresh) {
  const uint32_t now = 10UL * 60UL * 1000UL;
  const uint32_t okAt = now - 30UL * 1000UL;   // 30 s wczesniej
  const uint32_t staleMs = 60UL * 1000UL;      // prog: 1 minuta
  EXPECT_TRUE(isFresh(now, okAt, staleMs));
}

TEST(IsFreshPattern, NeverReceivedIsNeverFresh) {
  EXPECT_FALSE(isFresh(/*now=*/123456, /*okAt=*/0, /*staleMs=*/60000));
}

// Znacznik kilkaset ms W PRZYSZLOSCI wzgledem `now` — realny wyscig odczytu miedzy
// rdzeniami (netTask pisze stempel, watek rysujacy lapie wlasne `now` chwile wczesniej
// albo pozniej). `now - okAt` zawija sie do wartosci bliskiej UINT32_MAX; okno
// tolerancji (kFreshFutureSkewToleranceMs = 2 s) ma to zlapac i zwrocic "swieze".
TEST(IsFreshPattern, TimestampSlightlyInFutureFromCrossCoreRaceIsFresh) {
  const uint32_t now = 1000000;
  const uint32_t okAt = now + 500;  // 500 ms "w przyszlosci" wzgledem now
  EXPECT_TRUE(isFresh(now, okAt, /*staleMs=*/60000));
}

// Kontrast z powyzszym: przyszlosc WIEKSZA niz okno tolerancji (np. zegar realnie sie
// rozjechal, a nie zwykly wyscig o kilkaset ms) NIE jest juz uznawana za swieza.
TEST(IsFreshPattern, TimestampFarInFutureBeyondToleranceIsNotFresh) {
  const uint32_t now = 1000000;
  const uint32_t okAt = now + 60000;  // 60 s "w przyszlosci" -- poza oknem tolerancji
  EXPECT_FALSE(isFresh(now, okAt, /*staleMs=*/60000));
}
