/*
  Pogoda + Fotowoltaika Huawei SUN2000 — ESP32-S3 + ST7789 320x240
  https://github.com/premiumads-pl/Weather-huawei-sun2000
  ------------------------------------------------------------------
  5 widoków rotujących automatycznie (płytka nie ma przycisków):
    1. TERAZ    — temperatura, ikona, wiatr/wilgoć/ciśnienie/chmury, wschód/zachód, UV
    2. GODZINY  — krzywa temperatury na 12 h + słupki opadów + ikony co 2 h
    3. 5 DNI    — słupki zakresu min–max, ikony, opady
    4. PV       — moc, wskaźnik obciążenia, profil produkcji dnia, dziś/dom/sieć/temp
    5. SAMOLOTY — mapa Zatoki Gdańskiej + lista lotów (ADS-B)

  Konfiguracja (WiFi, lokalizacja, falownik) siedzi w NVS i ustawia się
  przez panel WWW. W kodzie nie ma żadnych sekretów.

  Aktualizacje: OTA z GitHub Releases, sprawdzane co 15 minut.
*/

#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cstring>

#include "Colors.h"
#include "Config.h"
#include "FlightClient.h"
#include "FlightData.h"
#include "Ota.h"
#include "Portal.h"
#include "PvClient.h"
#include "PvData.h"
#include "Settings.h"
#include "Version.h"
#include "WeatherClient.h"
#include "WeatherData.h"
#include "WeatherUi.h"

WeatherUi ui;
WeatherClient weatherClient;
PvClient pvClient;
FlightClient flightClient;
Ota ota;

SemaphoreHandle_t gLock = nullptr;
WeatherModel gWeather{};
PvModel gPv{};
PvHistory gHist{};
FlightModel gFlights{};
volatile bool gWifiOk = false;
volatile bool gBooting = true;
volatile bool gFlightsNeeded = false;
volatile int gWifiAttempt = 0;
char gBootMsg[48] = "Start...";

WeatherModel uiWeather{};
PvModel uiPv{};
PvHistory uiHist{};
FlightModel uiFlights{};

AlertKind lastAlertKind = AlertKind::NONE;
uint32_t lastAlertAt[8] = {};

static void setBootMsg(const char* m) {
  strncpy(gBootMsg, m, sizeof(gBootMsg) - 1);
  gBootMsg[sizeof(gBootMsg) - 1] = '\0';
}

// ----------------------------------------------------------------- WiFi/NTP --

static bool connectWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }
  if (!settings().hasWifi()) {
    return false;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(settings().ssid, settings().pass);
  ++gWifiAttempt;
  setBootMsg("Łączenie z WiFi...");

  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 12000) {
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  Serial.printf("WiFi OK, IP: %s\n", WiFi.localIP().toString().c_str());
  setBootMsg("Synchronizacja czasu...");
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();
  for (int i = 0; i < 40 && time(nullptr) < 1700000000; ++i) {
    vTaskDelay(pdMS_TO_TICKS(250));
  }
  portal::beginSta();
  return true;
}

// ------------------------------------------------------------ zadanie sieci --

static void netTask(void*) {
  uint32_t nextWeatherAt = 0;
  uint32_t nextPvAt = 0;
  uint32_t nextFlightAt = 0;
  uint32_t nextOtaAt = 45000;   // pierwsze sprawdzenie po 45 s od startu
  uint32_t nextStoreAt = 0;
  bool firstWeather = false;

  for (;;) {
    if (!settings().hasWifi()) {
      gBooting = false;
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    if (WiFi.status() != WL_CONNECTED) {
      gWifiOk = false;
      if (!connectWifi()) {
        vTaskDelay(pdMS_TO_TICKS(cfg::WIFI_RETRY_MS));
        continue;
      }
    }
    gWifiOk = true;
    const uint32_t now = millis();

    // ---- pogoda ----
    if (static_cast<int32_t>(now - nextWeatherAt) >= 0) {
      if (!firstWeather) {
        setBootMsg("Pobieram prognozę...");
      }
      WeatherModel tmp{};
      if (weatherClient.fetch(tmp)) {
        xSemaphoreTake(gLock, portMAX_DELAY);
        gWeather = tmp;
        xSemaphoreGive(gLock);
        nextWeatherAt = millis() + cfg::WEATHER_REFRESH_MS;
        firstWeather = true;
        Serial.printf("Pogoda OK: %.1f C, kod %d\n", tmp.current.tempC,
                      tmp.current.weatherCode);
      } else {
        Serial.printf("Pogoda BLAD: %s\n", tmp.errorMsg);
        nextWeatherAt = millis() + 30000;
      }
    }

    // ---- fotowoltaika ----
    if (static_cast<int32_t>(now - nextPvAt) >= 0) {
      PvModel tmp{};
      const bool ok = pvClient.fetch(tmp);
      xSemaphoreTake(gLock, portMAX_DELAY);
      gPv = tmp;
      if (ok) {
        const time_t t = time(nullptr);
        if (t > 1700000000) {
          struct tm tmv{};
          localtime_r(&t, &tmv);
          const int prevDay = gHist.day;
          gHist.push(tmv.tm_yday, tmv.tm_hour, tmv.tm_min, tmp.data.powerAcW);
          if (prevDay != gHist.day) {
            // nowy dzień — profil wyczyszczony, zapisz od razu
            nextStoreAt = 0;
          }
        }
      }
      xSemaphoreGive(gLock);
      nextPvAt = millis() + cfg::PV_REFRESH_MS;
      if (ok) {
        Serial.printf("PV: AC=%ldW DC=%ldW siec=%ldW dzis=%.2fkWh st=0x%04X\n",
                      static_cast<long>(tmp.data.powerAcW),
                      static_cast<long>(tmp.data.powerDcW),
                      static_cast<long>(tmp.data.gridPowerW), tmp.data.energyTodayKwh,
                      tmp.data.statusCode);
      } else {
        Serial.printf("PV BLAD: %s\n", tmp.errorMsg);
      }
    }

    // ---- zapis profilu produkcji do NVS ----
    if (static_cast<int32_t>(now - nextStoreAt) >= 0) {
      xSemaphoreTake(gLock, portMAX_DELAY);
      PvHistory snap = gHist;
      xSemaphoreGive(gLock);
      if (snap.day >= 0) {
        pvHistorySave(snap);
      }
      nextStoreAt = millis() + cfg::PV_STORE_MS;
    }

    // ---- samoloty ----
    if (gFlightsNeeded && static_cast<int32_t>(millis() - nextFlightAt) >= 0) {
      FlightModel tmp{};
      if (flightClient.fetch(tmp)) {
        xSemaphoreTake(gLock, portMAX_DELAY);
        gFlights = tmp;
        xSemaphoreGive(gLock);
        nextFlightAt = millis() + cfg::FLIGHT_REFRESH_MS;
        Serial.printf("Loty: %d w zasiegu, na liscie %d\n", tmp.total, tmp.count);
      } else {
        nextFlightAt = millis() + 20000;
        Serial.printf("Loty BLAD: %s\n", tmp.errorMsg);
      }
    }

    // ---- OTA ----
    if (settings().otaEnabled && static_cast<int32_t>(millis() - nextOtaAt) >= 0) {
      ota.checkAndUpdate();  // przy sukcesie restartuje urządzenie
      nextOtaAt = millis() + cfg::OTA_CHECK_MS;
    }

    if (firstWeather && gBooting) {
      gBooting = false;
    }
    if (gBooting && millis() > 35000) {
      gBooting = false;
    }

    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

// -------------------------------------------------------------- zadanie web --

static void webTask(void*) {
  for (;;) {
    portal::loop();
    portal::serialConsole();
    vTaskDelay(pdMS_TO_TICKS(8));
  }
}

// ---------------------------------------------------------------- alerty ----

static Alert buildAlert(const WeatherModel& w, const PvModel& pv) {
  Alert a{};

  if (pv.online && pvStatusIsFault(pv.data.statusCode)) {
    a.kind = AlertKind::PV_FAULT;
    strncpy(a.title, "Awaria falownika", sizeof(a.title) - 1);
    snprintf(a.text, sizeof(a.text), "Status 0x%04X — sprawdź instalację",
             pv.data.statusCode);
    a.color = col::ERR;
    a.iconCode = -1;
    return a;
  }

  if (!w.ready) {
    return a;
  }

  int stormIn = -1;
  int rainIn = -1;
  float maxWind = w.current.windKmh;
  float minTemp = w.current.tempC;
  float maxTemp = w.current.tempC;
  float maxRain = 0.f;

  for (int i = 0; i < WX_HOURS; ++i) {
    const HourSlot& s = w.hours[i];
    if (!s.valid) continue;
    if (stormIn < 0 && s.data.weatherCode >= 95) stormIn = s.offsetHours;
    if (s.data.precipMm >= 4.f && rainIn < 0) rainIn = s.offsetHours;
    if (s.data.precipMm > maxRain) maxRain = s.data.precipMm;
    if (s.data.windKmh > maxWind) maxWind = s.data.windKmh;
    if (s.data.tempC < minTemp) minTemp = s.data.tempC;
    if (s.data.tempC > maxTemp) maxTemp = s.data.tempC;
  }

  if (w.current.weatherCode >= 95 || stormIn >= 0) {
    a.kind = AlertKind::STORM;
    strncpy(a.title, "Burza", sizeof(a.title) - 1);
    if (w.current.weatherCode >= 95) {
      snprintf(a.text, sizeof(a.text), "Burza nad %s — teraz", settings().city);
    } else {
      snprintf(a.text, sizeof(a.text), "Prognozowana za %d h", stormIn);
    }
    a.color = col::STORM;
    a.iconCode = 95;
    return a;
  }

  if (maxWind >= 60.f) {
    a.kind = AlertKind::WIND;
    strncpy(a.title, "Silny wiatr", sizeof(a.title) - 1);
    snprintf(a.text, sizeof(a.text), "Do %.0f km/h w ciągu 12 h", maxWind);
    a.color = col::WARN;
    a.iconCode = -1;
    return a;
  }

  if (rainIn >= 0) {
    a.kind = AlertKind::HEAVY_RAIN;
    strncpy(a.title, "Ulewa", sizeof(a.title) - 1);
    snprintf(a.text, sizeof(a.text), "Do %.1f mm/h — za %d h", maxRain, rainIn);
    a.color = col::RAIN;
    a.iconCode = 65;
    return a;
  }

  if (minTemp <= -2.f) {
    a.kind = AlertKind::FROST;
    strncpy(a.title, "Mróz", sizeof(a.title) - 1);
    snprintf(a.text, sizeof(a.text), "Do %.0f°C w ciągu 12 h", minTemp);
    a.color = col::T_FREEZE;
    a.iconCode = 71;
    return a;
  }

  if (maxTemp >= 30.f) {
    a.kind = AlertKind::HEAT;
    strncpy(a.title, "Upał", sizeof(a.title) - 1);
    snprintf(a.text, sizeof(a.text), "Do %.0f°C — pij wodę", maxTemp);
    a.color = col::T_HOT;
    a.iconCode = 0;
    return a;
  }

  return a;
}

// ------------------------------------------------------------------ setup ----

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\nPogoda + PV — firmware v%d\n", FW_VERSION);

  settings().load();
  gLock = xSemaphoreCreateMutex();

  if (!ui.begin()) {
    Serial.println("BLAD: nie udalo sie utworzyc bufora TFT");
    while (true) {
      delay(1000);
    }
  }

  if (cfg::COLOR_TEST_MODE) {
    ui.drawColorTest();
    return;
  }

  pvHistoryLoad(gHist);

  if (!settings().hasWifi()) {
    portal::beginAp();
    gBooting = false;
  }

  ui.drawBoot(gBootMsg, 0);

  xTaskCreatePinnedToCore(webTask, "web", 12288, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 3, nullptr, 0);
}

// ------------------------------------------------------------------- loop ----

void loop() {
  if (cfg::COLOR_TEST_MODE) {
    delay(1000);
    return;
  }

  const uint32_t now = millis();

  // --- tryb konfiguracji: instrukcja na ekranie ---
  if (portal::apActive()) {
    ui.drawSetup(portal::apSsid(), portal::apPass(), portal::apIp());
    if (portal::wifiJustSaved()) {
      portal::clearWifiSavedFlag();
      delay(1500);
      ESP.restart();
    }
    delay(60);
    return;
  }

  // --- aktualizacja w toku ---
  const OtaStatus& os = otaStatus();
  if (os.state == OtaState::DOWNLOADING || os.state == OtaState::DONE) {
    ui.drawOta(os.progress, os.message);
    delay(60);
    return;
  }

  if (gBooting) {
    ui.drawBoot(gBootMsg, gWifiAttempt);
    delay(25);
    return;
  }

  xSemaphoreTake(gLock, portMAX_DELAY);
  uiWeather = gWeather;
  uiPv = gPv;
  uiHist = gHist;
  uiFlights = gFlights;
  xSemaphoreGive(gLock);

  gFlightsNeeded = ui.needsFlights(now);

  // --- tryb nocny ---
  const time_t t = time(nullptr);
  if (t > 1700000000) {
    struct tm tmv{};
    localtime_r(&t, &tmv);
    const bool night = (tmv.tm_hour >= cfg::NIGHT_FROM_H) || (tmv.tm_hour < cfg::NIGHT_TO_H);
    ui.setBacklightTarget(night ? cfg::BL_NIGHT : cfg::BL_DAY);
  }

  // --- alerty ---
  const Alert a = buildAlert(uiWeather, uiPv);
  if (a.kind != AlertKind::NONE) {
    const int idx = static_cast<int>(a.kind);
    const bool changed = (a.kind != lastAlertKind);
    const bool cooled =
        (lastAlertAt[idx] == 0) || (now - lastAlertAt[idx] >= cfg::ALERT_COOLDOWN_MS);
    if (changed || cooled) {
      lastAlertAt[idx] = now;
      lastAlertKind = a.kind;
      ui.raiseAlert(a, now);
      Serial.printf("ALERT: %s — %s\n", a.title, a.text);
    }
  } else {
    lastAlertKind = AlertKind::NONE;
  }

  const bool animating = ui.render(uiWeather, uiPv, uiHist, uiFlights, gWifiOk, now);
  delay(animating ? cfg::FRAME_ACTIVE_MS : cfg::FRAME_IDLE_MS);
}
