#include "Ota.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFiClientSecure.h>
#include <cstring>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include "Config.h"
#include "Version.h"

namespace {
OtaStatus gStatus;
volatile bool gRequested = false;
volatile bool gBusy = false;
volatile bool gUiFreed = false;

void setMsg(const char* m) {
  strncpy(gStatus.message, m, sizeof(gStatus.message) - 1);
  gStatus.message[sizeof(gStatus.message) - 1] = '\0';
}
}  // namespace

OtaStatus& otaStatus() {
  return gStatus;
}

void requestOtaCheck() {
  gRequested = true;
}

bool takeOtaRequest() {
  if (!gRequested) {
    return false;
  }
  gRequested = false;
  return true;
}

void otaUiBufferFreed() {
  gUiFreed = true;
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
  // Poczekaj, aż UI odda bufor ekranu (150 kB) — bez tego heapu starcza tylko
  // na TLS, a pobieranie 1,3 MB się wykłada.
  gStatus.state = OtaState::DOWNLOADING;
  setMsg("Zwalniam pamiec...");
  for (int i = 0; i < 60 && !gUiFreed; ++i) {
    delay(50);
  }
  Serial.printf("OTA: heap przed pobieraniem = %u B\n",
                static_cast<unsigned>(ESP.getFreeHeap()));

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

  // Diagnostyka + sprzątanie po ewentualnej przerwanej próbie.
  const esp_partition_t* run = esp_ota_get_running_partition();
  const esp_partition_t* nxt = esp_ota_get_next_update_partition(nullptr);
  Serial.printf("OTA: plik=%d B, wolny heap=%u B\n", total,
                static_cast<unsigned>(ESP.getFreeHeap()));
  if (run) Serial.printf("OTA: dziala z '%s' (%u B)\n", run->label,
                         static_cast<unsigned>(run->size));
  if (nxt) Serial.printf("OTA: cel '%s' (%u B)\n", nxt->label,
                         static_cast<unsigned>(nxt->size));

  if (Update.isRunning()) {
    Serial.println("OTA: poprzednia proba wisiala — czyszcze");
    Update.abort();
  }

  if (!Update.begin(total)) {
    char b[48];
    snprintf(b, sizeof(b), "%s", Update.errorString());
    setMsg(b);
    Serial.printf("OTA: Update.begin() blad %d: %s\n", Update.getError(),
                  Update.errorString());
    Update.abort();
    http.end();
    return false;
  }

  Serial.printf("OTA: pobieram %d bajtow...\n", total);
  gStatus.state = OtaState::DOWNLOADING;
  gStatus.progress = 0;
  setMsg("Pobieram nową wersję");

  // UWAGA: nie odpytujemy available() — na TLS potrafi zwracać 0 mimo danych
  // w drodze i robi się fałszywy timeout. writeStream() czyta blokująco.
  Update.onProgress([](size_t done, size_t all) {
    gStatus.progress = all ? static_cast<int>((done * 100) / all) : 0;
  });

  Stream& stream = http.getStream();
  const size_t written = Update.writeStream(stream);

  if (written != static_cast<size_t>(total)) {
    char b[48];
    snprintf(b, sizeof(b), "Pobrano %u z %d B", static_cast<unsigned>(written), total);
    setMsg(b);
    Serial.printf("OTA: %s (%s)\n", b, Update.errorString());
    Update.abort();
    http.end();
    return false;
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
  if (gBusy) {
    Serial.println("OTA: sprawdzanie juz trwa — pomijam");
    return false;
  }
  gBusy = true;

  struct Guard {
    ~Guard() { gBusy = false; }
  } guard;

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
    Serial.printf("OTA BLAD: %s — restart\n", gStatus.message);
    // Bufor ekranu jest już zwolniony; najczystszy powrót do normalnej pracy
    // to restart (spróbujemy ponownie przy kolejnym sprawdzeniu).
    delay(4000);
    ESP.restart();
    return false;
  }

  delay(1200);
  ESP.restart();
  return true;
}
