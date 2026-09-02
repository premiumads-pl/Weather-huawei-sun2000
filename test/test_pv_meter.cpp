// Testy dla pvMeterUpdate() i PvMeterBase — PvData.h, linie ~89-285.
//
// DETERMINIZM CZASU: pvMeterUpdate() woła localtime_r(&now, ...), czyli wynik
// zależy od strefy czasowej procesu. Żeby testy dawały ten sam wynik na
// laptopie dewelopera (dowolna TZ) i na runnerze CI (zwykle UTC), wymuszamy
// TZ=UTC na starcie tego pliku testowego (patrz ForceUtcTimezone poniżej) i
// budujemy znaczniki czasu funkcją epochUTC() opartą o timegm() — a nie o
// time(nullptr) ani o dosłowne literały epoch, które byłyby nieczytelne.
#include "PvData.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <ctime>

namespace {

// Wymusza UTC dla całego procesu testowego. Runda przez klasę + globalny
// obiekt, żeby zadziałało zanim pierwszy TEST() się wykona (kolejność
// inicjalizacji zmiennych globalnych w jednym pliku jest zdefiniowana, więc
// definicja niżej, przed include gtest_main, jest wystarczająca — ale dla
// pewności robimy to też w SetUpTestSuite każdej grupy testów pluginem GTest
// nie jest to konieczne: setenv w konstruktorze globalnym wystarczy).
struct ForceUtcTimezone {
  ForceUtcTimezone() {
    setenv("TZ", "UTC", 1);
    tzset();
  }
} gForceUtc;

// Buduje epoch (sekundy UTC) z pól kalendarzowych czytelnych w teście —
// timegm() interpretuje `struct tm` jako UTC niezależnie od TZ procesu,
// więc test jest odporny nawet gdyby ForceUtcTimezone nie zadziałało.
time_t epochUTC(int year, int month, int day, int hour, int minute, int second = 0) {
  struct tm tmv{};
  tmv.tm_year = year - 1900;
  tmv.tm_mon = month - 1;
  tmv.tm_mday = day;
  tmv.tm_hour = hour;
  tmv.tm_min = minute;
  tmv.tm_sec = second;
  return timegm(&tmv);
}

}  // namespace

// --- (a) czas sprzed NTP: baza sie nie rusza, zwraca NONE ------------------
TEST(PvMeterUpdate, BeforeNtpThresholdReturnsNoneAndLeavesBaseUntouched) {
  PvSnapshot s;
  s.meterExportKwh = 10.f;
  s.meterImportKwh = 20.f;
  PvMeterBase b;
  b.valid = true;
  b.year = 2023;
  b.yday = 5;
  b.importKwh = 1.f;
  b.exportKwh = 2.f;

  const time_t now = 1000000000;  // < 1700000000, czyli "przed NTP"
  const PvBaseEvent ev = pvMeterUpdate(s, b, now);

  EXPECT_EQ(ev, PvBaseEvent::NONE);
  EXPECT_TRUE(b.valid);
  EXPECT_EQ(b.year, 2023);
  EXPECT_EQ(b.yday, 5);
  EXPECT_FLOAT_EQ(b.importKwh, 1.f);
  EXPECT_FLOAT_EQ(b.exportKwh, 2.f);
  EXPECT_FALSE(s.meterTodayOk);
}

// --- (e) wartownik -1: nigdy nie dostalismy licznika => NONE, baza nietknieta
TEST(PvMeterUpdate, NegativeMeterSentinelReturnsNoneAndLeavesBaseUntouched) {
  PvSnapshot s;
  s.meterExportKwh = -1.f;  // sentinel "nigdy nie doszlo"
  s.meterImportKwh = 5.f;
  PvMeterBase b;
  b.valid = true;
  b.year = 2024;
  b.yday = 42;
  b.importKwh = 3.f;
  b.exportKwh = 4.f;

  const time_t now = epochUTC(2024, 6, 1, 12, 0);
  const PvBaseEvent ev = pvMeterUpdate(s, b, now);

  EXPECT_EQ(ev, PvBaseEvent::NONE);
  EXPECT_EQ(b.year, 2024);
  EXPECT_EQ(b.yday, 42);
  EXPECT_FLOAT_EQ(b.importKwh, 3.f);
  EXPECT_FLOAT_EQ(b.exportKwh, 4.f);
  EXPECT_FALSE(s.meterTodayOk);
}

TEST(PvMeterUpdate, NegativeImportMeterAlsoReturnsNone) {
  PvSnapshot s;
  s.meterExportKwh = 5.f;
  s.meterImportKwh = -1.f;
  PvMeterBase b;  // b.valid == false domyslnie

  const time_t now = epochUTC(2024, 6, 1, 12, 0);
  const PvBaseEvent ev = pvMeterUpdate(s, b, now);

  EXPECT_EQ(ev, PvBaseEvent::NONE);
  EXPECT_FALSE(b.valid);
}

// --- (b) pierwsze wywolanie z b.valid==false => SET_FIRST -------------------
TEST(PvMeterUpdate, FirstCallWithInvalidBaseReturnsSetFirst) {
  PvSnapshot s;
  s.meterExportKwh = 10.f;
  s.meterImportKwh = 20.f;
  PvMeterBase b;  // valid == false domyslnie

  const time_t now = epochUTC(2024, 6, 1, 8, 0);
  const PvBaseEvent ev = pvMeterUpdate(s, b, now);

  EXPECT_EQ(ev, PvBaseEvent::SET_FIRST);
  EXPECT_TRUE(b.valid);
  // SET_FIRST nigdy nie jest "pelna" (patrz komentarz w PvData.h, linia ~246).
  EXPECT_FALSE(b.full);
  EXPECT_EQ(b.event, static_cast<uint8_t>(PvBaseEvent::SET_FIRST));
}

// --- (d) licznik cofnal sie w tej samej dobie => WENT_BACK ------------------
TEST(PvMeterUpdate, MeterGoingBackwardsWithinSameDayReturnsWentBack) {
  const time_t now = epochUTC(2024, 6, 1, 14, 0);
  struct tm tmv{};
  localtime_r(&now, &tmv);

  PvSnapshot s;
  s.meterImportKwh = 90.f;  // mniej niz baza (100) -> cofniecie
  s.meterExportKwh = 60.f;  // wiecej niz baza (50) -> samo w sobie by nie wywolalo WENT_BACK

  PvMeterBase b;
  b.valid = true;
  b.year = tmv.tm_year + 1900;
  b.yday = tmv.tm_yday;  // TA SAMA doba co `now`, zeby nie zlapac ROLLED
  b.importKwh = 100.f;
  b.exportKwh = 50.f;

  const PvBaseEvent ev = pvMeterUpdate(s, b, now);

  EXPECT_EQ(ev, PvBaseEvent::WENT_BACK);
  // WENT_BACK przestawia baze na biezaca wartosc odczytu.
  EXPECT_FLOAT_EQ(b.importKwh, 90.f);
  EXPECT_FLOAT_EQ(b.exportKwh, 60.f);
  // WENT_BACK nigdy nie jest "pelna" (tylko ROLLED moze byc).
  EXPECT_FALSE(b.full);
  EXPECT_EQ(b.event, static_cast<uint8_t>(PvBaseEvent::WENT_BACK));
}

// --- (c) zmiana doby (ROLLED), wariant DALEKO od polnocy => full == false ---
TEST(PvMeterUpdate, RolledFarFromMidnightIsNotFull) {
  // Baza z dnia poprzedniego, bez "ostatniego udanego odczytu" bliskiego
  // polnocy (havePrev == false, bo lastEpoch == 0) — jedyny kandydat na baze
  // to biezacy odczyt o 09:00, czyli 540 minut po polnocy: daleko za progiem
  // PV_BASE_FULL_MIN (30 min).
  const time_t prevDay = epochUTC(2024, 6, 1, 12, 0);
  struct tm prevTm{};
  localtime_r(&prevDay, &prevTm);

  PvMeterBase b;
  b.valid = true;
  b.year = prevTm.tm_year + 1900;
  b.yday = prevTm.tm_yday;
  b.importKwh = 10.f;
  b.exportKwh = 5.f;
  // lastEpoch celowo 0 -> havePrev == false w pvMeterUpdate().

  PvSnapshot s;
  s.meterImportKwh = 25.f;
  s.meterExportKwh = 15.f;

  const time_t now = epochUTC(2024, 6, 2, 9, 0);  // nastepna doba, 09:00
  const PvBaseEvent ev = pvMeterUpdate(s, b, now);

  EXPECT_EQ(ev, PvBaseEvent::ROLLED);
  EXPECT_FALSE(b.full);
  EXPECT_TRUE(s.meterTodayOk == false);  // baza niepelna -> "dzis" nie jest pokazywane
}

// --- (c) zmiana doby (ROLLED), wariant BLISKO polnocy z uzyciem odczytu ----
//         SPRZED polnocy (v169 — wybor kandydata blizszego polnocy) ---------
TEST(PvMeterUpdate, RolledPicksPreMidnightReadingWhenCloserAndMarksFull) {
  // Krok 1: odczyt o 23:59 dnia D (1 minuta przed polnoca) — normalny cykl,
  // ktory NIE zmienia bazy (ta sama doba co juz zapisana), ale zapamietuje
  // "ostatni udany odczyt" (b.lastEpoch/lastImportKwh/lastExportKwh).
  const time_t day1_2359 = epochUTC(2024, 6, 1, 23, 59);
  struct tm day1Tm{};
  localtime_r(&day1_2359, &day1Tm);

  PvMeterBase b;
  b.valid = true;
  b.year = day1Tm.tm_year + 1900;
  b.yday = day1Tm.tm_yday;
  b.importKwh = 100.f;
  b.exportKwh = 50.f;

  PvSnapshot s1;
  s1.meterImportKwh = 100.f;  // bez zmiany -> nie WENT_BACK
  s1.meterExportKwh = 50.f;

  const PvBaseEvent ev1 = pvMeterUpdate(s1, b, day1_2359);
  ASSERT_EQ(ev1, PvBaseEvent::NONE);
  ASSERT_EQ(b.lastEpoch, static_cast<uint32_t>(day1_2359));

  // Krok 2: odczyt o 00:20 dnia D+1 (20 minut po polnocy). Kandydat "biezacy"
  // (offset +20) jest DALEJ od polnocy niz kandydat "sprzed polnocy" z kroku 1
  // (offset -1), wiec baza MA wziac ten sprzed polnocy.
  const time_t day2_0020 = epochUTC(2024, 6, 2, 0, 20);
  PvSnapshot s2;
  s2.meterImportKwh = 100.5f;
  s2.meterExportKwh = 50.2f;

  const PvBaseEvent ev2 = pvMeterUpdate(s2, b, day2_0020);

  EXPECT_EQ(ev2, PvBaseEvent::ROLLED);
  // Kandydat sprzed polnocy: offset -1 min, minuta zapisana jako 1440 + (-1) = 1439.
  EXPECT_EQ(b.offsetMin, -1);
  EXPECT_EQ(b.minute, 1439);
  // Baza powinna przejac WARTOSCI z odczytu sprzed polnocy (100 / 50), nie
  // z biezacego (100.5 / 50.2).
  EXPECT_FLOAT_EQ(b.importKwh, 100.f);
  EXPECT_FLOAT_EQ(b.exportKwh, 50.f);
  // Odleglosc |-1| <= PV_BASE_FULL_MIN(30) -> baza PELNA.
  EXPECT_TRUE(b.full);
  EXPECT_TRUE(s2.meterTodayOk);
  EXPECT_NEAR(s2.meterTodayImportKwh, 100.5f - 100.f, 1e-4f);
  EXPECT_NEAR(s2.meterTodayExportKwh, 50.2f - 50.f, 1e-4f);
}
