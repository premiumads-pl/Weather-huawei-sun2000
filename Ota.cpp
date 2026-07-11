#include "Ota.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFiClientSecure.h>
#include <cstring>

#include "Config.h"
#include "Version.h"

namespace {
OtaStatus gStatus;

void setMsg(const char* m) {
  strncpy(gStatus.message, m, sizeof(gStatus.message) - 1);
  gStatus.message[sizeof(gStatus.message) - 1] = '\0';
}
}  // namespace

OtaStatus& otaStatus() {
  return gStatus;
}

bool Ota::fetchRemoteVersion(int& version) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(12);

  HTTPClient http;
  http.setTimeout(12000);
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(client, cfg::OTA_VERSION_URL)) {
    return false;
  }
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }
  const String body = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    return false;
  }
  version = doc["version"] | 0;
  return version > 0;
}

bool Ota::downloadAndFlash() {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(20);

  HTTPClient http;
  http.setTimeout(20000);
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(client, cfg::OTA_FIRMWARE_URL)) {
    setMsg("Nie mogę pobrać pliku");
    return false;
  }
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    char b[48];
    snprintf(b, sizeof(b), "HTTP %d przy pobieraniu", code);
    setMsg(b);
    http.end();
    return false;
  }

  const int total = http.getSize();
  if (total <= 0) {
    setMsg("Nieznany rozmiar pliku");
    http.end();
    return false;
  }
  if (!Update.begin(total)) {
    setMsg("Za mało miejsca na OTA");
    http.end();
    return false;
  }

  Serial.printf("OTA: pobieram %d bajtow...\n", total);
  gStatus.state = OtaState::DOWNLOADING;
  gStatus.progress = 0;
  setMsg("Pobieram nową wersję");

  Stream* stream = http.getStreamPtr();
  uint8_t buf[2048];
  int written = 0;
  uint32_t lastData = millis();

  while (written < total) {
    const size_t avail = stream->available();
    if (avail == 0) {
      if (millis() - lastData > 15000) {
        setMsg("Przerwane pobieranie");
        Update.abort();
        http.end();
        return false;
      }
      delay(5);
      continue;
    }
    lastData = millis();
    const int n = stream->readBytes(buf, avail > sizeof(buf) ? sizeof(buf) : avail);
    if (n <= 0) {
      continue;
    }
    if (Update.write(buf, n) != static_cast<size_t>(n)) {
      setMsg("Błąd zapisu OTA");
      Update.abort();
      http.end();
      return false;
    }
    written += n;
    gStatus.progress = (written * 100) / total;
  }

  http.end();

  if (!Update.end(true)) {
    char b[48];
    snprintf(b, sizeof(b), "Update.end blad %d", Update.getError());
    setMsg(b);
    return false;
  }

  gStatus.state = OtaState::DONE;
  gStatus.progress = 100;
  setMsg("Gotowe — restart");
  Serial.println("OTA: zapisano, restart");
  return true;
}

bool Ota::checkAndUpdate() {
  gStatus.state = OtaState::CHECKING;
  setMsg("Sprawdzam aktualizacje");

  int remote = 0;
  if (!fetchRemoteVersion(remote)) {
    gStatus.state = OtaState::IDLE;
    setMsg("");
    return false;
  }

  lastRemote_ = remote;
  gStatus.remoteVersion = remote;

  if (remote <= FW_VERSION) {
    gStatus.state = OtaState::IDLE;
    setMsg("");
    Serial.printf("OTA: aktualna wersja %d (zdalna %d)\n", FW_VERSION, remote);
    return false;
  }

  Serial.printf("OTA: nowa wersja %d (mam %d) — aktualizuje\n", remote, FW_VERSION);

  if (!downloadAndFlash()) {
    gStatus.state = OtaState::FAILED;
    Serial.printf("OTA BLAD: %s\n", gStatus.message);
    return false;
  }

  delay(1200);
  ESP.restart();
  return true;
}
