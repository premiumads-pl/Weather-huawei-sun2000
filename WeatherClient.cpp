#include "WeatherClient.h"

#include "Config.h"
#include "Settings.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <cstring>
#include <ctime>

namespace {

const char* const kDayNames[7] = {"NDZ", "PON", "WT", "ŚR", "CZW", "PT", "SOB"};

void hhmmFromEpoch(int64_t epoch, char* out, size_t len) {
  if (epoch <= 0 || len < 6) {
    return;
  }
  const time_t t = static_cast<time_t>(epoch);
  struct tm tmv{};
  localtime_r(&t, &tmv);
  snprintf(out, len, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
}

}  // namespace

bool WeatherClient::buildUrl(char* buf, std::size_t len) const {
  snprintf(buf, len,
           "https://api.open-meteo.com/v1/forecast?"
           "latitude=%.4f&longitude=%.4f&timezone=Europe%%2FWarsaw"
           "&forecast_days=6&forecast_hours=14"
           "&timeformat=unixtime"
           "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,"
           "wind_speed_10m,wind_direction_10m,cloud_cover,pressure_msl,precipitation,is_day,uv_index"
           "&hourly=temperature_2m,weather_code,precipitation,precipitation_probability,"
           "wind_speed_10m"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_sum,"
           "uv_index_max,wind_speed_10m_max,sunrise,sunset",
           settings().lat, settings().lon);
  return true;
}

bool WeatherClient::parsePayload(const char* json, std::size_t len, WeatherModel& out) const {
  // Filtr ArduinoJson — parsujemy tylko potrzebne pola, oszczędza sporo RAM-u.
  JsonDocument filter;
  filter["current"] = true;
  JsonObject fh = filter["hourly"].to<JsonObject>();
  fh["time"] = true;
  fh["temperature_2m"] = true;
  fh["weather_code"] = true;
  fh["precipitation"] = true;
  fh["precipitation_probability"] = true;
  fh["wind_speed_10m"] = true;
  filter["daily"] = true;

  JsonDocument doc;
  const DeserializationError err =
      deserializeJson(doc, json, len, DeserializationOption::Filter(filter));
  if (err) {
    snprintf(out.errorMsg, sizeof(out.errorMsg), "JSON: %s", err.c_str());
    return false;
  }

  JsonObjectConst cur = doc["current"];
  if (cur.isNull()) {
    strncpy(out.errorMsg, "Brak danych current", sizeof(out.errorMsg) - 1);
    return false;
  }

  out.current.tempC = cur["temperature_2m"].as<float>();
  out.current.feelsC = cur["apparent_temperature"].as<float>();
  out.current.humidity = cur["relative_humidity_2m"].as<int>();
  out.current.cloudCover = cur["cloud_cover"].as<int>();
  out.current.windKmh = cur["wind_speed_10m"].as<float>();
  out.current.windDir = cur["wind_direction_10m"].as<int>();
  out.current.weatherCode = cur["weather_code"].as<int>();
  out.current.pressureHpa = cur["pressure_msl"].as<float>();
  out.current.precipMm = cur["precipitation"].as<float>();
  out.current.uvIndex = cur["uv_index"].as<float>();
  out.current.isDay = cur["is_day"].as<int>() != 0;
  out.current.valid = true;

  const int64_t curEpoch = cur["time"].as<int64_t>();
  hhmmFromEpoch(curEpoch, out.updatedAt, sizeof(out.updatedAt));

  // ---- godzinowo: znajdź indeks bieżącej godziny, weź +1..+12 ----
  JsonObjectConst hourly = doc["hourly"];
  JsonArrayConst hTime = hourly["time"];
  int baseIdx = -1;
  if (!hTime.isNull()) {
    for (size_t i = 0; i < hTime.size(); ++i) {
      if (hTime[i].as<int64_t>() > curEpoch) {
        baseIdx = static_cast<int>(i);  // pierwsza godzina w przyszłości
        break;
      }
    }
  }

  for (int i = 0; i < WX_HOURS; ++i) {
    HourSlot& slot = out.hours[i];
    slot.offsetHours = i + 1;
    slot.valid = false;
    if (baseIdx < 0) {
      continue;
    }
    const int idx = baseIdx + i;
    if (idx >= static_cast<int>(hTime.size())) {
      continue;
    }
    const int64_t e = hTime[idx].as<int64_t>();
    const time_t t = static_cast<time_t>(e);
    struct tm tmv{};
    localtime_r(&t, &tmv);
    slot.hourOfDay = tmv.tm_hour;
    slot.data.tempC = hourly["temperature_2m"][idx].as<float>();
    slot.data.weatherCode = hourly["weather_code"][idx].as<int>();
    slot.data.precipMm = hourly["precipitation"][idx].as<float>();
    slot.data.precipProb = hourly["precipitation_probability"][idx].as<int>();
    slot.data.windKmh = hourly["wind_speed_10m"][idx].as<float>();
    slot.data.valid = true;
    slot.valid = true;
  }

  // ---- dziennie: dziś (0) do metadanych, jutro..+5 do widoku ----
  JsonObjectConst daily = doc["daily"];
  hhmmFromEpoch(daily["sunrise"][0].as<int64_t>(), out.sunrise, sizeof(out.sunrise));
  hhmmFromEpoch(daily["sunset"][0].as<int64_t>(), out.sunset, sizeof(out.sunset));
  out.uvTodayMax = daily["uv_index_max"][0].as<float>();
  out.precipToday = daily["precipitation_sum"][0].as<float>();

  for (int d = 0; d < WX_DAYS; ++d) {
    const int idx = d + 1;
    DaySlot& slot = out.days[d];
    slot.dayOffset = idx;
    slot.tempMax = daily["temperature_2m_max"][idx].as<float>();
    slot.tempMin = daily["temperature_2m_min"][idx].as<float>();
    slot.precipMm = daily["precipitation_sum"][idx].as<float>();
    slot.uvMax = daily["uv_index_max"][idx].as<float>();
    slot.windMaxKmh = daily["wind_speed_10m_max"][idx].as<float>();
    slot.weatherCode = daily["weather_code"][idx].as<int>();

    const int64_t e = daily["time"][idx].as<int64_t>();
    if (e > 0) {
      const time_t t = static_cast<time_t>(e);
      struct tm tmv{};
      localtime_r(&t, &tmv);
      strncpy(slot.name, kDayNames[tmv.tm_wday % 7], sizeof(slot.name) - 1);
      snprintf(slot.date, sizeof(slot.date), "%02d.%02d", tmv.tm_mday, tmv.tm_mon + 1);
    }
    slot.valid = true;
  }

  out.ready = true;
  out.errorMsg[0] = '\0';
  return true;
}

bool WeatherClient::fetch(WeatherModel& out) {
  out.errorMsg[0] = '\0';

  char url[640];
  buildUrl(url, sizeof(url));

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(15000);
  http.setReuse(false);

  if (!http.begin(client, url)) {
    strncpy(out.errorMsg, "HTTP begin fail", sizeof(out.errorMsg) - 1);
    return false;
  }

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    snprintf(out.errorMsg, sizeof(out.errorMsg), "HTTP %d", code);
    http.end();
    return false;
  }

  const String payload = http.getString();
  http.end();

  if (payload.length() < 100) {
    strncpy(out.errorMsg, "Pusta odpowiedź", sizeof(out.errorMsg) - 1);
    return false;
  }

  return parsePayload(payload.c_str(), payload.length(), out);
}
