#include "Viessmann.h"

#include "Log.h"
#include "Settings.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>

#include <cstring>
#include <ctime>

namespace vi {
namespace {

constexpr char kIam[] = "https://iam.viessmann-climatesolutions.com/idp/v3";
constexpr char kApi[] = "https://api.viessmann-climatesolutions.com/iot/v2";
constexpr uint32_t kRefreshTtlDays = 180;

char gVerifier[65] = {};       // PKCE — zyje miedzy authUrl() a exchangeCode()
char gAccess[1400] = {};
uint32_t gAccessExpAt = 0;     // millis()
char gErr[56] = {};

// ArduinoJson w PSRAM: odpowiedz z /features ma ~53 kB, a po sparsowaniu jeszcze
// wiecej. W SRAM nie ma na to miejsca; w PSRAM to szum (mamy 1,9 MB).
struct PsramAlloc : ArduinoJson::Allocator {
  void* allocate(size_t n) override { return ps_malloc(n); }
  void deallocate(void* p) override { free(p); }
  void* reallocate(void* p, size_t n) override { return ps_realloc(p, n); }
};
PsramAlloc gAlloc;

// Duze odpowiedzi API leca jako Transfer-Encoding: chunked (sprawdzone: /features
// = chunked, male bledy = Content-Length). http.getStream() oddaje SUROWY strumien,
// razem z naglowkami porcji ("1f4a\r\n{...") — ArduinoJson slusznie odrzucal to
// jako InvalidInput. writeToStream() rozpakowuje porcje za nas; zbieramy calosc
// do PSRAM i dopiero parsujemy.
// writeToStream() chce Stream*, nie Print* — stad puste metody odczytu.
class PsramSink : public Stream {
 public:
  ~PsramSink() { free(buf_); }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* d, size_t n) override {
    if (len_ + n + 1 > cap_) {
      size_t want = cap_ ? cap_ * 2 : 16384;
      while (want < len_ + n + 1) want *= 2;
      if (want > 400000) return 0;   // bezpiecznik — /features ma ~53 kB
      char* p = static_cast<char*>(buf_ ? ps_realloc(buf_, want) : ps_malloc(want));
      if (p == nullptr) return 0;
      buf_ = p;
      cap_ = want;
    }
    memcpy(buf_ + len_, d, n);
    len_ += n;
    buf_[len_] = '\0';
    return n;
  }
  const char* data() const { return buf_ ? buf_ : ""; }
  size_t size() const { return len_; }

 private:
  char* buf_ = nullptr;
  size_t cap_ = 0, len_ = 0;
};

// base64url bez wypelnienia — tego wymaga PKCE
void b64url(const uint8_t* in, size_t inLen, char* out, size_t outLen) {
  size_t n = 0;
  mbedtls_base64_encode(reinterpret_cast<uint8_t*>(out), outLen, &n, in, inLen);
  out[n] = '\0';
  for (size_t i = 0; i < n; ++i) {
    if (out[i] == '+') out[i] = '-';
    else if (out[i] == '/') out[i] = '_';
    else if (out[i] == '=') { out[i] = '\0'; break; }
  }
}

void makeVerifier() {
  static const char kAlpha[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
  for (int i = 0; i < 64; ++i) {
    gVerifier[i] = kAlpha[esp_random() % (sizeof(kAlpha) - 1)];
  }
  gVerifier[64] = '\0';
}

String challengeFrom(const char* verifier) {
  uint8_t hash[32];
  mbedtls_sha256(reinterpret_cast<const uint8_t*>(verifier), strlen(verifier), hash, 0);
  char out[64];
  b64url(hash, sizeof(hash), out, sizeof(out));
  return String(out);
}

String urlenc(const char* s) {
  String o;
  for (const char* p = s; *p; ++p) {
    const char c = *p;
    if (isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '~') {
      o += c;
    } else {
      char b[4];
      snprintf(b, sizeof(b), "%%%02X", static_cast<unsigned char>(c));
      o += b;
    }
  }
  return o;
}

// POST form -> JSON. Wspolne dla wymiany kodu i odswiezania.
bool postToken(const String& body, JsonDocument& doc, char* errOut, size_t errLen) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15);

  HTTPClient http;
  http.setTimeout(15000);
  String url = String(kIam) + "/token";
  if (!http.begin(client, url)) {
    snprintf(errOut, errLen, "brak polaczenia");
    return false;
  }
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  const int code = http.POST(body);
  String resp = http.getString();
  http.end();

  if (deserializeJson(doc, resp) != DeserializationError::Ok) {
    snprintf(errOut, errLen, "zla odpowiedz (HTTP %d)", code);
    return false;
  }
  if (code != 200) {
    const char* e = doc["error_description"] | doc["error"] | "blad autoryzacji";
    snprintf(errOut, errLen, "%.*s", static_cast<int>(errLen - 1), e);
    return false;
  }
  return true;
}

// Zapisuje tokeny. Viessmann przy odswiezaniu ROTUJE refresh token — jesli nowy
// przyjdzie, trzeba go zapisac, inaczej nastepne odswiezenie padnie.
void storeTokens(JsonDocument& doc) {
  const char* acc = doc["access_token"] | "";
  const char* ref = doc["refresh_token"] | "";
  const uint32_t exp = doc["expires_in"] | 3600;

  snprintf(gAccess, sizeof(gAccess), "%s", acc);
  gAccessExpAt = millis() + (exp > 120 ? (exp - 60) * 1000UL : 60000UL);

  if (ref[0] != '\0' && strcmp(ref, settings().viRefresh) != 0) {
    snprintf(settings().viRefresh, sizeof(settings().viRefresh), "%s", ref);
    settings().viAuthAt = static_cast<uint32_t>(time(nullptr));
    settings().viSave();
    LOG("Piec: zapisano nowy refresh token");
  }
}

bool ensureAccess() {
  if (gAccess[0] != '\0' && static_cast<int32_t>(gAccessExpAt - millis()) > 0) {
    return true;
  }
  if (settings().viRefresh[0] == '\0') {
    snprintf(gErr, sizeof(gErr), "brak autoryzacji");
    return false;
  }

  String body = "grant_type=refresh_token&client_id=" + urlenc(settings().viClientId) +
                "&refresh_token=" + urlenc(settings().viRefresh);

  JsonDocument doc;
  if (!postToken(body, doc, gErr, sizeof(gErr))) {
    LOG("Piec: odswiezenie tokena padlo: %s", gErr);
    return false;
  }
  storeTokens(doc);
  LOG("Piec: token odswiezony");
  return true;
}

// POST komendy. Zwraca true tylko przy HTTP 200/201 — piec potrafi odpowiedziec
// 200 z bledem w ciele, wiec sprawdzamy tez tresc.
bool apiPost(const String& path, const String& body, char* errOut, size_t errLen) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(20);

  HTTPClient http;
  http.setTimeout(20000);
  if (!http.begin(client, String(kApi) + path)) {
    snprintf(errOut, errLen, "brak polaczenia");
    return false;
  }
  http.addHeader("Authorization", String("Bearer ") + gAccess);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/vnd.siren+json");

  const int code = http.POST(body);
  String resp = http.getString();
  http.end();

  // KLUCZOWE: Viessmann odsyla HTTP 200 nawet gdy komenda zostala ODRZUCONA —
  // prawdziwy status siedzi w ciele ("statusCode": 400/502...). Sprawdzanie samego
  // kodu HTTP dawalo falszywy sukces: meldowalem "zapisano", a piec nie drgnal.
  // PyViCare broni sie przed tym tak samo (__handle_command_error).
  JsonDocument e;
  const bool parsed = deserializeJson(e, resp) == DeserializationError::Ok;
  const int inner = parsed ? (e["statusCode"] | 0) : 0;

  if (code != 200 && code != 201 && code != 204) {
    const char* msg = parsed ? (e["message"] | e["error"] | "?") : "?";
    snprintf(errOut, errLen, "HTTP %d: %.34s", code, msg);
    LOG("Piec: komenda odrzucona, HTTP %d, cialo: %.120s", code, resp.c_str());
    return false;
  }
  if (inner >= 400) {
    const char* msg = e["message"] | e["errorType"] | "?";
    snprintf(errOut, errLen, "piec: %.40s", msg);
    LOG("Piec: komenda odrzucona (HTTP 200, statusCode %d): %.140s", inner, resp.c_str());
    return false;
  }
  LOG("Piec: komenda OK, odpowiedz: %.100s", resp.c_str());
  return true;
}

bool apiGet(const String& path, JsonDocument& doc, JsonDocument* filter) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(25);

  HTTPClient http;
  http.setTimeout(25000);
  if (!http.begin(client, String(kApi) + path)) {
    snprintf(gErr, sizeof(gErr), "brak polaczenia");
    return false;
  }
  http.addHeader("Authorization", String("Bearer ") + gAccess);
  http.addHeader("Accept", "application/json");
  const int code = http.GET();
  if (code != 200) {
    snprintf(gErr, sizeof(gErr), "API HTTP %d", code);
    http.end();
    return false;
  }

  PsramSink sink;
  const int got = http.writeToStream(&sink);
  http.end();
  if (got <= 0 || sink.size() == 0) {
    snprintf(gErr, sizeof(gErr), "pusta odpowiedz (%d)", got);
    return false;
  }

  DeserializationError e =
      filter ? deserializeJson(doc, sink.data(), DeserializationOption::Filter(*filter))
             : deserializeJson(doc, sink.data());
  if (e != DeserializationError::Ok) {
    snprintf(gErr, sizeof(gErr), "JSON: %s (%u B)", e.c_str(),
             static_cast<unsigned>(sink.size()));
    return false;
  }
  return true;
}

// Instalacja i gateway zmieniaja sie raz na nigdy — pytamy tylko, gdy pusto.
bool ensureIds() {
  if (settings().viInstallation[0] != '\0' && settings().viGateway[0] != '\0') return true;

  JsonDocument doc(&gAlloc);
  if (!apiGet("/equipment/installations?includeGateways=true", doc, nullptr)) return false;

  JsonArrayConst arr = doc["data"];
  if (arr.isNull() || arr.size() == 0) {
    snprintf(gErr, sizeof(gErr), "brak instalacji");
    return false;
  }
  const long inst = arr[0]["id"] | 0L;
  const char* gw = arr[0]["gateways"][0]["serial"] | "";
  if (inst == 0 || gw[0] == '\0') {
    snprintf(gErr, sizeof(gErr), "brak gateway");
    return false;
  }
  snprintf(settings().viInstallation, sizeof(settings().viInstallation), "%ld", inst);
  snprintf(settings().viGateway, sizeof(settings().viGateway), "%s", gw);
  settings().viSave();
  LOG("Piec: instalacja %ld, gateway %s", inst, gw);
  return true;
}

float gCircuitTarget = 0.f;

float propF(JsonObjectConst f, const char* prop) {
  return f["properties"][prop]["value"] | 0.0f;
}

}  // namespace

String authUrl(const char* clientId, const char* redirectUri) {
  makeVerifier();
  const String ch = challengeFrom(gVerifier);
  return String(kIam) + "/authorize?client_id=" + urlenc(clientId) +
         "&redirect_uri=" + urlenc(redirectUri) +
         "&response_type=code&code_challenge_method=S256&code_challenge=" + ch +
         "&scope=" + urlenc("IoT offline_access");
}

bool exchangeCode(const char* code, const char* redirectUri, char* errOut, size_t errLen) {
  if (gVerifier[0] == '\0') {
    snprintf(errOut, errLen, "najpierw wygeneruj link");
    return false;
  }
  String body = "grant_type=authorization_code&client_id=" + urlenc(settings().viClientId) +
                "&redirect_uri=" + urlenc(redirectUri) + "&code_verifier=" + urlenc(gVerifier) +
                "&code=" + urlenc(code);

  JsonDocument doc;
  if (!postToken(body, doc, errOut, errLen)) return false;

  storeTokens(doc);
  if (settings().viRefresh[0] == '\0') {
    snprintf(errOut, errLen, "brak refresh tokena (offline_access?)");
    return false;
  }
  settings().viEnabled = true;
  settings().viSave();
  gVerifier[0] = '\0';
  LOG("Piec: autoryzacja OK");
  return true;
}

int daysLeft() {
  if (settings().viAuthAt == 0) return -1;
  const uint32_t now = static_cast<uint32_t>(time(nullptr));
  if (now < 1700000000UL) return -1;
  const int32_t used = static_cast<int32_t>(now - settings().viAuthAt) / 86400;
  const int32_t left = static_cast<int32_t>(kRefreshTtlDays) - used;
  return left < 0 ? 0 : left;
}

float circuitTarget() {
  return gCircuitTarget;
}

bool setCircuitTemp(int celsius, char* errOut, size_t errLen) {
  if (celsius < 2 || celsius > 80) {
    snprintf(errOut, errLen, "poza zakresem 2..80");
    return false;
  }
  if (!settings().hasViessmann()) {
    snprintf(errOut, errLen, "piec nieskonfigurowany");
    return false;
  }
  if (!ensureAccess() || !ensureIds()) {
    snprintf(errOut, errLen, "%s", gErr);
    return false;
  }

  String path = String("/features/installations/") + settings().viInstallation + "/gateways/" +
                settings().viGateway +
                "/devices/0/features/heating.circuits.0.operating.programs.normal/commands/"
                "setTemperature";
  char body[48];
  snprintf(body, sizeof(body), "{\"targetTemperature\":%d}", celsius);

  if (!apiPost(path, body, errOut, errLen)) {
    LOG("Piec: ZAPIS nastawy %d C PADL: %s", celsius, errOut);
    return false;
  }
  LOG("Piec: zapisano nastawe obiegu %d C", celsius);
  return true;
}

void forget() {
  settings().viRefresh[0] = '\0';
  settings().viInstallation[0] = '\0';
  settings().viGateway[0] = '\0';
  settings().viEnabled = false;
  settings().viAuthAt = 0;
  settings().viSave();
  gAccess[0] = '\0';
  gAccessExpAt = 0;
}

bool fetch(Model& out) {
  if (!settings().hasViessmann()) {
    snprintf(out.err, sizeof(out.err), "wylaczone");
    return false;
  }
  gErr[0] = '\0';
  if (!ensureAccess() || !ensureIds()) {
    snprintf(out.err, sizeof(out.err), "%s", gErr);
    return false;
  }

  // Filtr wycina komendy i linki (~polowa payloadu) — zostawiamy sama tresc.
  JsonDocument filter;
  JsonObject fe = filter["data"].add<JsonObject>();
  fe["feature"] = true;
  fe["properties"] = true;

  String path = String("/features/installations/") + settings().viInstallation +
                "/gateways/" + settings().viGateway + "/devices/0/features";

  JsonDocument doc(&gAlloc);
  if (!apiGet(path, doc, &filter)) {
    snprintf(out.err, sizeof(out.err), "%s", gErr);
    return false;
  }

  Model m{};
  for (JsonObjectConst f : doc["data"].as<JsonArrayConst>()) {
    const char* name = f["feature"] | "";
    if (strcmp(name, "heating.dhw.sensors.temperature.dhwCylinder") == 0) {
      m.dhwTempC = propF(f, "value");
    } else if (strcmp(name, "heating.dhw.temperature.main") == 0) {
      m.dhwTargetC = propF(f, "value");
    } else if (strcmp(name, "heating.dhw.operating.modes.active") == 0) {
      snprintf(m.dhwMode, sizeof(m.dhwMode), "%s", f["properties"]["value"]["value"] | "");
    } else if (strcmp(name, "heating.boiler.sensors.temperature.commonSupply") == 0) {
      m.supplyTempC = propF(f, "value");
    } else if (strcmp(name, "heating.burners.0") == 0) {
      m.burnerActive = f["properties"]["active"]["value"] | false;
    } else if (strcmp(name, "heating.burners.0.modulation") == 0) {
      m.modulationPct = static_cast<int>(propF(f, "value"));
    } else if (strcmp(name, "heating.burners.0.statistics") == 0) {
      m.burnerHours = static_cast<uint32_t>(propF(f, "hours"));
      m.burnerStarts = static_cast<uint32_t>(propF(f, "starts"));
    } else if (strcmp(name, "heating.circuits.0.operating.modes.active") == 0) {
      snprintf(m.circuitMode, sizeof(m.circuitMode), "%s", f["properties"]["value"]["value"] | "");
    } else if (strcmp(name, "heating.circuits.0.operating.programs.normal") == 0) {
      m.circuitTargetC = propF(f, "temperature");
      gCircuitTarget = m.circuitTargetC;
    } else if (strcmp(name, "heating.gas.consumption.summary.dhw") == 0) {
      m.gasDhwM3 = propF(f, "currentDay");
    } else if (strcmp(name, "heating.gas.consumption.summary.heating") == 0) {
      m.gasHeatM3 = propF(f, "currentDay");
    } else if (strcmp(name, "heating.heat.production.summary.dhw") == 0) {
      m.heatDhwKwh = propF(f, "currentDay");
    } else if (strcmp(name, "heating.heat.production.summary.heating") == 0) {
      m.heatHeatKwh = propF(f, "currentDay");
    } else if (strcmp(name, "heating.power.consumption.summary.dhw") == 0) {
      m.powerKwh += propF(f, "currentDay");
    } else if (strcmp(name, "heating.power.consumption.summary.heating") == 0) {
      m.powerKwh += propF(f, "currentDay");
    } else if (strcmp(name, "tcu.wifi") == 0) {
      m.wifiRssi = static_cast<int>(propF(f, "strength"));
    }
  }

  m.online = true;
  m.valid = true;
  m.okAt = millis();
  out = m;
  LOG("Piec: CWU %.1f°C (cel %.0f), zasilanie %.1f°C, palnik %s %d%%, gaz dzis %.1f m3",
      m.dhwTempC, m.dhwTargetC, m.supplyTempC, m.burnerActive ? "ON" : "off", m.modulationPct,
      m.gasDhwM3 + m.gasHeatM3);
  return true;
}

}  // namespace vi
