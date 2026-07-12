#pragma once

#include <cstdint>
#include <ctime>

constexpr int WX_HOURS = 12;  // +1h ... +12h
constexpr int WX_DAYS = 5;    // jutro ... +5 dni

struct WeatherSnapshot {
  float tempC = 0.f;
  float feelsC = 0.f;
  int humidity = 0;
  int cloudCover = 0;
  float windKmh = 0.f;
  int windDir = 0;
  int weatherCode = 0;
  float pressureHpa = 0.f;
  float precipMm = 0.f;
  int precipProb = 0;
  float uvIndex = 0.f;
  bool isDay = true;
  bool valid = false;
};

struct HourSlot {
  int offsetHours = 0;
  int hourOfDay = 0;  // 0..23 (czas lokalny)
  WeatherSnapshot data{};
  bool valid = false;
};

struct DaySlot {
  int dayOffset = 0;  // 1 = jutro
  float tempMax = 0.f;
  float tempMin = 0.f;
  float precipMm = 0.f;
  float uvMax = 0.f;
  float windMaxKmh = 0.f;
  int weatherCode = 0;
  char name[8] = {};   // PON, WT, ...
  char date[8] = {};   // 13.07
  bool valid = false;
};

struct WeatherModel {
  WeatherSnapshot current{};
  HourSlot hours[WX_HOURS]{};
  DaySlot days[WX_DAYS]{};
  char sunrise[6] = {};  // "04:32"
  char sunset[6] = {};   // "21:14"
  float uvTodayMax = 0.f;
  float precipToday = 0.f;
  char updatedAt[20] = {};
  bool ready = false;
  char errorMsg[48] = {};

  // Radar opadowy (realny pomiar, nie model) — patrz RadarClient.h
  uint8_t radarLevel = 0;
  uint32_t radarAgeSec = 0;
  bool radarValid = false;
};

// ----------------------------------------------------- dzień / noc dla PV ----

// "HH:MM" -> minuty od północy. -1, gdy pole jest puste albo nie ma sensu.
inline int wxTimeToMinutes(const char* hhmm) {
  if (!hhmm || hhmm[0] == '\0' || hhmm[2] != ':') {
    return -1;
  }
  const int h = (hhmm[0] - '0') * 10 + (hhmm[1] - '0');
  const int m = (hhmm[3] - '0') * 10 + (hhmm[4] - '0');
  if (h < 0 || h > 23 || m < 0 || m > 59) {
    return -1;
  }
  return h * 60 + m;
}

// Minuty od północy (czas lokalny). -1, dopóki nie ma czasu z NTP.
inline int localMinutesNow() {
  const time_t t = time(nullptr);
  if (t < 1700000000) {
    return -1;
  }
  struct tm tmv{};
  localtime_r(&t, &tmv);
  return tmv.tm_hour * 60 + tmv.tm_min;
}

// Huawei SUN2000 wyłącza Modbus TCP po zachodzie — nocne milczenie falownika to
// NIE jest awaria. Okno "wolno mu milczeć" jest celowo przesunięte w obie strony
// o ten sam margines:
//   * zaczyna się 30 min PO zachodzie — bo falownik potrafi odpowiadać jeszcze
//     chwilę po zmroku i wtedy pokazujemy normalne dane,
//   * kończy się 30 min PO wschodzie — bo rano budzi się z opóźnieniem
//     (rozgrzewka Modbusa sięga ~100 s) i nie chcemy czerwonego alarmu co świt.
// Bez godzin wschodu/zachodu (brak prognozy) zwracamy false — wolimy nie zgadywać
// i pokazać uczciwy błąd, niż zamieść realną awarię pod dywan.
constexpr int PV_SLEEP_MARGIN_MIN = 30;

inline bool pvMayBeAsleep(const char* sunrise, const char* sunset, int nowMinutes) {
  const int sr = wxTimeToMinutes(sunrise);
  const int ss = wxTimeToMinutes(sunset);
  if (sr < 0 || ss < 0 || nowMinutes < 0) {
    return false;
  }

  const int start = (ss + PV_SLEEP_MARGIN_MIN) % 1440;  // początek okna snu
  const int end = (sr + PV_SLEEP_MARGIN_MIN) % 1440;    // koniec okna snu

  if (start < end) {
    // Zachód tak późny, że margines przeniósł początek okna za północ.
    return nowMinutes >= start && nowMinutes < end;
  }
  // Normalny przypadek: okno przechodzi przez północ.
  return nowMinutes >= start || nowMinutes < end;
}
