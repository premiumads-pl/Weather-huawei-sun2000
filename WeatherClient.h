#pragma once

#include <cstddef>

#include "WeatherData.h"

class WeatherClient {
 public:
  bool fetch(WeatherModel& out);

 private:
  bool buildUrl(char* buf, std::size_t len) const;
  bool parsePayload(const char* json, std::size_t len, WeatherModel& out) const;
};
