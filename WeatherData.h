#pragma once

#include <cstdint>

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
};
