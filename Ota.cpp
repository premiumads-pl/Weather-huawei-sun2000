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
#include "Log.h"
#include "OtaGuard.h"
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
  client.setTimeout(15);
  client.setHandshakeTimeout(15);

  HTTPClient http;
  http.setTimeout(15000);
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent("pogoda-esp32");

  if (!http.begin(client, cfg::OTA_VERSION_URL)) {
    Serial.println("OTA: http.begin() nie powiodlo sie");
    return false;
  }
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("OTA: sprawdzenie wersji -> HTTP %d (%s), heap=%u B\n", code,
                  HTTPClient::errorToString(code).c_str(),
                  static_cast<unsigned>(ESP.getFreeHeap()));
    http.end();
    return false;
  }
  const String body = http.getString();
  http.end();

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("OTA: zly JSON wersji (%s)\n", err.c_str());
    return false;
  }
  version = doc["version"] | 0;
  if (version <= 0) {
    Serial.println("OTA: brak pola 'version'");
    return false;
  }
  return true;
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

bool Ota::checkAndUpdate(bool manual) {
  if (gBusy) {
    Serial.println("OTA: sprawdzanie juz trwa — pomijam");
    return false;
  }

  // KLUCZOWE dla rollbacku: dopóki BIEŻĄCA wersja nie jest potwierdzona, nie wolno
  // nic wgrywać. Nowy obraz poszedłby na drugą partycję — czyli nadpisałby JEDYNĄ
  // sprawną wersję, na którą moglibyśmy się cofnąć.
  //
  // ESP-IDF pilnuje tego samo w esp_ota_begin() (ESP_ERR_OTA_ROLLBACK_INVALID_STATE),
  // ale arduinowy Update w ogóle nie używa esp_ota_begin() — zapisuje partycję sam
  // i woła tylko esp_ota_set_boot_partition(), które takiej blokady nie ma.
  // Dlatego musimy jej pilnować tutaj.
  if (otaTrialActive()) {
    LOG("OTA: trwa okres próbny bieżącej wersji — nie ruszam aktualizacji\n");
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
    // SIEĆ BEZPIECZEŃSTWA: jeśli sterty jest tak mało, że nie da się zestawić TLS,
    // urządzenie nie mogłoby się już NIGDY zaktualizować po sieci (dokładnie to
    // zabiło v14). Oddajemy więc 150 kB bufora ekranu i próbujemy jeszcze raz.
    const uint32_t heap = ESP.getFreeHeap();
    if (heap < 60000) {
      LOG("OTA: malo RAM (%lu B) — zwalniam bufor i probuje ponownie\n",
          static_cast<unsigned long>(heap));
      gStatus.state = OtaState::DOWNLOADING;   // UI odda bufor
      setMsg("Sprawdzam aktualizacje...");
      for (int i = 0; i < 60 && !gUiFreed; ++i) {
        delay(50);
      }
      if (!fetchRemoteVersion(remote)) {
        gStatus.state = OtaState::FAILED;
        LOG("OTA: nie udalo sie mimo zwolnienia bufora — restart\n");
        delay(3000);
        ESP.restart();
        return false;
      }
      // udało się — jeśli nie ma nowszej wersji, wracamy do normalnej pracy
      if (remote <= FW_VERSION) {
        gStatus.state = OtaState::IDLE;
        setMsg("");
        return false;   // render() sam odtworzy bufor
      }
    } else {
      gStatus.state = OtaState::IDLE;
      setMsg("");
      return false;
    }
  }

  lastRemote_ = remote;
  gStatus.remoteVersion = remote;
  diag().otaRemote = remote;
  diag().otaOkAt = millis();

  if (remote <= FW_VERSION) {
    gStatus.state = OtaState::IDLE;
    setMsg("");
    Serial.printf("OTA: aktualna wersja %d (zdalna %d)\n", FW_VERSION, remote);
    return false;
  }

  // Ta wersja już raz (a właściwie dwa razy) nie przeżyła okresu próbnego. Bez tej
  // blokady wpadlibyśmy w pętlę: pobierz cegłę -> rollback -> pobierz tę samą cegłę.
  // Ręczne sprawdzenie z panelu WWW blokadę omija — to furtka na wypadek, gdyby
  // wersja została odrzucona niesłusznie (np. przez awarię routera w złym momencie).
  if (!manual && otaVersionRejected(remote)) {
    gStatus.state = OtaState::IDLE;
    setMsg("");
    LOG("OTA: wersja %d była już odrzucona po rollbacku — pomijam "
        "(wymuś ręcznie w panelu, jeśli to pomyłka)\n",
        remote);
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

  // Obraz siedzi już na drugiej partycji, a esp_ota_set_boot_partition() (w środku
  // Update.end()) ustawił jej stan na ESP_OTA_IMG_NEW. Zapisz w NVS, KTÓRA wersja
  // zaraz wejdzie w okres próbny — bo jeśli padnie panikiem od razu po starcie,
  // cofnie ją sam bootloader i nie zdąży o sobie nic powiedzieć. Bez tego znacznika
  // pobralibyśmy dokładnie tę samą cegłę w kółko.
  otaGuardArmTrial(remote);

  delay(1200);
  ESP.restart();
  return true;
}
