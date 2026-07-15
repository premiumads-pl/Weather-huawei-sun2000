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

// Ile kilometrow ma stopien dlugosci geograficznej na NASZEJ szerokosci.
// Bylo tu zaszyte 64,6 = 111,2 * cos(54,5 stopnia) — poprawne dla Gdyni i TYLKO dla
// Gdyni, podczas gdy lat/lon sa konfigurowalne w panelu (Krakow: 71,4, blad 10%).
// cosf liczymy przy zmianie szerokosci, a nie per samolot — distKm leci w petli.
float lonKmPerDeg() {
  static float cachedLat = 1000.f;   // niemozliwa szerokosc = "jeszcze nie liczone"
  static float cached = 64.6f;
  const float lat = settings().lat;
  if (lat != cachedLat) {
    cachedLat = lat;
    cached = 111.2f * cosf(lat * 0.017453293f);   // stopnie -> radiany
  }
  return cached;
}

float distKm(float lat, float lon) {
  const float dLat = (lat - settings().lat) * 111.2f;
  const float dLon = (lon - settings().lon) * lonKmPerDeg();
  return sqrtf(dLat * dLat + dLon * dLon);
}

// Karencja dla API tras. Gdy vrs-standing-data padnie, nie ma sensu walic w nie
// po 4 razy w kazdym cyklu (a cykl leci co minute) — trasy i tak dojda pozniej.
uint32_t gRouteBlockUntil = 0;   // millis(); 0 = brak karencji

bool routeApiBlocked() {
  return gRouteBlockUntil != 0 && static_cast<int32_t>(millis() - gRouteBlockUntil) < 0;
}

// Zwraca KOD HTTP, nie bool — bo wolajacy musi odroznic "404, czyli takiej trasy na
// pewno nie ma" (mozna zapamietac) od "nie udalo sie zapytac" (nie wolno zapamietac).
// 200 = OK i JSON sparsowany. 0 = blad transportu/TLS/DNS albo zle JSON.
// HTTPClient oddaje przy bledach polaczenia wartosci ujemne — tez nie sa 200.
int httpGetJson(const char* url, JsonDocument& doc, const JsonDocument* filter) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(10);

  HTTPClient http;
  http.setTimeout(12000);
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, url)) {
    return 0;
  }
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return code;
  }
  const String body = http.getString();
  http.end();

  DeserializationError err;
  if (filter != nullptr) {
    err = deserializeJson(doc, body, DeserializationOption::Filter(*filter));
  } else {
    err = deserializeJson(doc, body);
  }
  return err ? 0 : 200;
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
// Zwraca "czy wynik wolno zapamietac w cache", a nie "czy trasa jest znana".
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
  const int code = httpGetJson(url, doc, &filter);

  // Stary komentarz mowil "404 / brak danych", ale funkcja zwracala false TAKZE przy
  // timeoucie, bledzie TLS, DNS i HTTP 5xx — i wszystkie te przypadki ladowaly
  // w cache jako known = false, czyli NA STALE. Jeden chwilowy problem z siecia
  // odbieral trafionym wtedy rejsom trase az do wykrecenia wpisu z pierscienia:
  // lista pokazywala sam znak wywolawczy zamiast "WAW-GDN", a prio spadalo z 0/1 na 2,
  // wiec samolot lecacy do Gdanska tracil priorytet i wypadal z szostki na rzecz
  // przypadkowego GA. Wszystko po cichu.
  if (code == 404) {
    gRouteBlockUntil = 0;      // API zyje i odpowiedzialo: takiej trasy po prostu nie ma
    return true;
  }
  if (code != 200) {
    gRouteBlockUntil = millis() + 600000UL;   // 10 min przerwy
    return false;                             // NIE cache'ujemy — sprobujemy w nastepnym cyklu
  }
  gRouteBlockUntil = 0;

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
  Cand cand[kMaxCandidates]{};   // trzymana caly czas ROSNACO po dist
  int n = 0;                     // ile kandydatow w tablicy (max kMaxCandidates)
  int total = 0;                 // ile samolotow w zasiegu — liczone po CALEJ odpowiedzi

  for (JsonObjectConst a : arr) {
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

    // Bylo tu "if (n >= kMaxCandidates) break;" — czyli obciecie PRZED sortowaniem.
    // adsb.fi oddaje samoloty w SWOJEJ kolejnosci, nie po odleglosci: kod bral
    // pierwszych 18 z odpowiedzi, sortowal je i pokazywal "6 najblizszych" —
    // z osiemnastu przypadkowych, nie z czterdziestu pieciu w zasiegu. Samolot
    // przelatujacy nad domem, ktory wypadl w JSON-ie na pozycji 25, nie pojawial sie
    // wcale. Do tego "total" bylo liczba kandydatow PO obcieciu, wiec naglowek
    // zatrzymywal sie na "18 w zasiegu" i nigdy nie szedl wyzej.
    // Teraz: total liczy kazdy samolot, a tablica trzyma 18 NAJBLIZSZYCH — wstawianie
    // jest O(n*18), przy n ~ 50 to darmo.
    ++total;

    const float d = distKm(lat, lon);
    if (n == kMaxCandidates && d >= cand[kMaxCandidates - 1].dist) {
      continue;  // dalszy niz najgorszy z biezacych — nie ma po co go budowac
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

    // Wstaw z zachowaniem porzadku; przy pelnej tablicy nadpisz najgorszego.
    int pos = (n < kMaxCandidates) ? n++ : kMaxCandidates - 1;
    while (pos > 0 && cand[pos - 1].dist > d) {
      cand[pos] = cand[pos - 1];
      --pos;
    }
    cand[pos].f = f;
    cand[pos].dist = d;
    cand[pos].prio = 2;
  }

  out.total = total;

  // --- trasy: najpierw z cache, potem kilka nowych zapytan na cykl ---
  int budget = kMaxLookupsPerFetch;
  for (int i = 0; i < n; ++i) {
    Flight& f = cand[i].f;
    const RouteEntry* e = findCached(f.callsign);
    if (e == nullptr && budget > 0 && !routeApiBlocked()) {
      RouteEntry tmp{};
      // Do cache trafia tylko odpowiedz, ktorej API faktycznie udzielilo.
      // Nieudane zapytanie zostaje bez sladu — sprobujemy w nastepnym cyklu.
      if (lookupRoute(f.callsign, tmp)) {
        cache_[cacheNext_] = tmp;
        cacheNext_ = (cacheNext_ + 1) % CACHE_N;
        e = findCached(f.callsign);
      }
      --budget;   // budzet zjada takze porazka — inaczej jeden padniety cykl
                  // wysylalby 18 zapytan zamiast czterech
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
