#include "BleGateway.h"

#include "BleSensors.h"
#include "Log.h"
#include "Settings.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClient.h>

#include <cstring>

namespace blegw {
namespace {

uint32_t gOkAt = 0;
int gCount = 0;
char gErr[48] = "wylaczona";

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

void poll() {
  if (settings().bleGwHost[0] == '\0') return;

  WiFiClient client;
  client.setTimeout(6);
  HTTPClient http;
  http.setTimeout(6000);
  http.setReuse(false);

  char url[80];
  snprintf(url, sizeof(url), "http://%s/script/1/ble", settings().bleGwHost);
  if (!http.begin(client, url)) {
    snprintf(gErr, sizeof(gErr), "brak polaczenia");
    LOG("Bramka BLE: nie moge otworzyc %s", url);
    return;
  }
  const int code = http.GET();
  if (code != 200) {
    snprintf(gErr, sizeof(gErr), "HTTP %d", code);
    LOG("Bramka BLE: %s -> HTTP %d", url, code);
    http.end();
    return;
  }
  String body = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) {
    snprintf(gErr, sizeof(gErr), "zly JSON");
    return;
  }

  int n = 0;
  for (JsonPairConst kv : doc.as<JsonObjectConst>()) {
    const char* mac = kv.key().c_str();
    const int rssi = kv.value()["r"] | 0;
    const char* hex = kv.value()["d"] | "";
    const size_t hl = strlen(hex);
    if (hl < 12 || hl > 64 || (hl & 1)) continue;

    uint8_t raw[32];
    bool ok = true;
    for (size_t i = 0; i < hl / 2; ++i) {
      const int hi = hexVal(hex[i * 2]), lo = hexVal(hex[i * 2 + 1]);
      if (hi < 0 || lo < 0) { ok = false; break; }
      raw[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    if (!ok) continue;
    if (ble::feedRaw(mac, raw, hl / 2, rssi)) ++n;
  }

  gCount = n;
  gOkAt = millis();
  gErr[0] = '\0';
  static int lastN = -1;
  if (n != lastN) {   // logujemy tylko zmiany — inaczej zalejemy bufor
    LOG("Bramka BLE: %d ramek przyjetych", n);
    lastN = n;
  }
}

uint32_t lastOkAt() { return gOkAt; }
int lastCount() { return gCount; }
const char* lastError() { return gErr; }

}  // namespace blegw
