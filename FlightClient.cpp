#include "FlightClient.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <cmath>
#include <cstring>

#include "Config.h"
#include "Settings.h"
#include "MapData.h"

namespace {

constexpr int kMaxLookupsPerFetch = 4;  // ile nowych tras dociagamy w jednym cyklu
constexpr int kMaxCandidates = 18;      // ile samolotow bierzemy pod uwage

void trimCallsign(const char* src, char* dst, size_t n) {
  size_t j = 0;
  for (size_t i = 0; src[i] != '\0' && j < n - 1; ++i) {
    if (src[i] != ' ') {
      dst[j++] = src[i];
    }
  }
  dst[j] = '\0';
}

bool inMapBounds(float lat, float lon) {
  return lat >= gmap::LAT_MIN && lat <= gmap::LAT_MAX && lon >= gmap::LON_MIN &&
         lon <= gmap::LON_MAX;
}

float distKm(float lat, float lon) {
  const float dLat = (lat - settings().lat) * 111.2f;
  const float dLon = (lon - settings().lon) * 64.6f;
  return sqrtf(dLat * dLat + dLon * dLon);
}

bool httpGetJson(const char* url, JsonDocument& doc, const JsonDocument* filter) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(10);

  HTTPClient http;
  http.setTimeout(12000);
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, url)) {
    return false;
  }
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }
  const String body = http.getString();
  http.end();

  DeserializationError err;
  if (filter != nullptr) {
    err = deserializeJson(doc, body, DeserializationOption::Filter(*filter));
  } else {
    err = deserializeJson(doc, body);
  }
  return !err;
}

}  // namespace

const FlightClient::RouteEntry* FlightClient::findCached(const char* callsign) const {
  for (int i = 0; i < CACHE_N; ++i) {
    if (cache_[i].used && strcmp(cache_[i].callsign, callsign) == 0) {
      return &cache_[i];
    }
  }
  return nullptr;
}

// https://vrs-standing-data.adsb.lol/routes/<2 pierwsze znaki>/<callsign>.json
bool FlightClient::lookupRoute(const char* callsign, RouteEntry& out) {
  memset(&out, 0, sizeof(out));
  strncpy(out.callsign, callsign, sizeof(out.callsign) - 1);
  out.used = true;
  out.known = false;

  if (strlen(callsign) < 3) {
    return true;  // zapamietaj jako "brak trasy"
  }

  char url[110];
  snprintf(url, sizeof(url), "https://vrs-standing-data.adsb.lol/routes/%c%c/%s.json",
           callsign[0], callsign[1], callsign);

  JsonDocument filter;
  filter["_airport_codes_iata"] = true;

  JsonDocument doc;
  if (!httpGetJson(url, doc, &filter)) {
    return true;  // 404 / brak danych — cache'ujemy jako nieznane
  }

  const char* codes = doc["_airport_codes_iata"];
  if (codes == nullptr || strlen(codes) < 5) {
    return true;
  }

  // "WAW-GDN" lub "AAA-BBB-CCC" -> bierzemy pierwszy i ostatni
  char buf[32];
  strncpy(buf, codes, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  char* first = buf;
  char* last = buf;
  for (char* p = buf; *p; ++p) {
    if (*p == '-') {
      *p = '\0';
      last = p + 1;
    }
  }
  strncpy(out.orig, first, sizeof(out.orig) - 1);
  strncpy(out.dest, last, sizeof(out.dest) - 1);
  snprintf(out.route, sizeof(out.route), "%s-%s", out.orig, out.dest);
  out.known = true;
  return true;
}

bool FlightClient::fetch(FlightModel& out) {
  out.count = 0;
  out.total = 0;
  out.errorMsg[0] = '\0';

  // adsb.fi — ma znacznie lepsze pokrycie nad Polska niz adsb.lol
  // (laczy ADS-B z MLAT, wiec widzi tez lokalny ruch GA i smiglowce).
  char url[110];
  snprintf(url, sizeof(url), "https://opendata.adsb.fi/api/v2/lat/%.4f/lon/%.4f/dist/%d",
           settings().lat, settings().lon, cfg::FLIGHT_RADIUS_NM);

  JsonDocument filter;
  JsonObject ac = filter["aircraft"].add<JsonObject>();
  ac["flight"] = true;
  ac["t"] = true;
  ac["lat"] = true;
  ac["lon"] = true;
  ac["alt_baro"] = true;
  ac["gs"] = true;
  ac["track"] = true;
  ac["calc_track"] = true;

  JsonDocument doc;
  if (!httpGetJson(url, doc, &filter)) {
    strncpy(out.errorMsg, "Brak danych ADS-B", sizeof(out.errorMsg) - 1);
    return false;
  }

  JsonArrayConst arr = doc["aircraft"];
  if (arr.isNull()) {
    strncpy(out.errorMsg, "Pusta odpowiedz", sizeof(out.errorMsg) - 1);
    return false;
  }

  struct Cand {
    Flight f;
    float dist;
    int prio;
  };
  Cand cand[kMaxCandidates]{};
  int n = 0;

  for (JsonObjectConst a : arr) {
    if (n >= kMaxCandidates) {
      break;
    }
    const char* fl = a["flight"];
    if (fl == nullptr) {
      continue;  // bez znaku wywolawczego (obiekty naziemne TWR itp.)
    }
    JsonVariantConst alt = a["alt_baro"];
    if (alt.is<const char*>()) {
      continue;  // "ground" — koloujacy samolot
    }
    const float lat = a["lat"] | 1000.f;
    const float lon = a["lon"] | 1000.f;
    if (!inMapBounds(lat, lon)) {
      continue;
    }

    Flight f{};
    trimCallsign(fl, f.callsign, sizeof(f.callsign));
    if (f.callsign[0] == '\0') {
      continue;
    }
    const char* ty = a["t"];
    if (ty != nullptr) {
      strncpy(f.type, ty, sizeof(f.type) - 1);
    }
    f.lat = lat;
    f.lon = lon;
    f.altFt = alt.as<int32_t>();
    f.gs = static_cast<int16_t>(a["gs"] | 0.f);
    // MLAT czasem nie ma "track", tylko "calc_track"
    float tr = a["track"] | -1.f;
    if (tr < 0.f) {
      tr = a["calc_track"] | 0.f;
    }
    f.track = static_cast<int16_t>(tr);

    cand[n].f = f;
    cand[n].dist = distKm(lat, lon);
    cand[n].prio = 2;
    ++n;
  }

  out.total = n;

  // --- trasy: najpierw z cache, potem kilka nowych zapytan na cykl ---
  int budget = kMaxLookupsPerFetch;
  for (int i = 0; i < n; ++i) {
    Flight& f = cand[i].f;
    const RouteEntry* e = findCached(f.callsign);
    if (e == nullptr && budget > 0) {
      RouteEntry tmp{};
      lookupRoute(f.callsign, tmp);
      cache_[cacheNext_] = tmp;
      cacheNext_ = (cacheNext_ + 1) % CACHE_N;
      --budget;
      e = findCached(f.callsign);
    }
    if (e != nullptr && e->known) {
      strncpy(f.route, e->route, sizeof(f.route) - 1);
      strncpy(f.orig, e->orig, sizeof(f.orig) - 1);
      strncpy(f.dest, e->dest, sizeof(f.dest) - 1);
      f.routeKnown = true;
    }
    // priorytet: 0 = zwiazany z Gdanskiem, 1 = znana trasa, 2 = reszta (lokalne GA)
    if (flightToGdansk(f) || flightFromGdansk(f)) {
      cand[i].prio = 0;
    } else if (f.routeKnown) {
      cand[i].prio = 1;
    } else {
      cand[i].prio = 2;
    }
  }

  // --- sortowanie: priorytet, potem odleglosc od domu ---
  for (int i = 1; i < n; ++i) {
    Cand key = cand[i];
    int j = i - 1;
    while (j >= 0 && (cand[j].prio > key.prio ||
                      (cand[j].prio == key.prio && cand[j].dist > key.dist))) {
      cand[j + 1] = cand[j];
      --j;
    }
    cand[j + 1] = key;
  }

  const int take = (n < FLIGHT_MAX) ? n : FLIGHT_MAX;
  for (int i = 0; i < take; ++i) {
    out.list[i] = cand[i].f;
  }
  out.count = take;
  out.ready = true;
  return true;
}
