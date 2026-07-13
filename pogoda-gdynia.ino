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
#include "Led.h"
#include "Log.h"
#include "MqttClient.h"
#include "Ota.h"
#include "BleSensors.h"
#include "OtaGuard.h"
#include "RoomHistory.h"
#include "Portal.h"
#include "PvClient.h"
#include "PvData.h"
#include "RadarClient.h"
#include "Settings.h"
#include "Version.h"
#include "WeatherClient.h"
#include "WeatherData.h"
#include "WeatherUi.h"

WeatherUi ui;
WeatherClient weatherClient;
PvClient pvClient;
FlightClient flightClient;
RadarClient radarClient;
Ota ota;

SemaphoreHandle_t gLock = nullptr;
WeatherModel gWeather{};
PvModel gPv{};
PvHistory gHist{};
RoomHistory gRooms{};
RoomHistory uiRooms{};
FlightModel gFlights{};
volatile bool gWifiOk = false;
volatile bool gBooting = true;
volatile bool gFlightsNeeded = false;

// Radar potrzebuje 47 kB w JEDNYM kawalku. Sterty jest 113 kB, ale bufor ekranu
// (66 kB) siedzi w srodku i dzieli ja tak, ze najwiekszy spojny blok ma 43 kB.
// Brakuje 2,6 kB — wiec na czas dekodowania PNG oddajemy bufor. Ekran zamiera na
// chwile raz na 5 minut; to lepsze niz radar, ktory nie dziala nigdy.
// Bufor musi zwolnic rdzen 1 (to on rysuje), stad ta wymiana sygnalow.
volatile bool gRadarWantMem = false;
volatile bool gRadarMemReady = false;
volatile int gWifiAttempt = 0;
char gBootMsg[48] = "Łączenie z WiFi...";

// Ekran "połączono" z adresem IP, pokazywany raz po starcie.
constexpr uint32_t NET_INFO_MS = 10000;
volatile uint32_t gNetInfoUntil = 0;
bool gNetInfoDone = false;
char gIpStr[20] = "0.0.0.0";
volatile int gRssi = 0;

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

  strncpy(gIpStr, WiFi.localIP().toString().c_str(), sizeof(gIpStr) - 1);
  gRssi = WiFi.RSSI();
  ++diag().wifiConnects;
  LOG("WiFi OK, IP: %s (%d dBm), heap %lu\n", gIpStr, gRssi,
      (unsigned long)ESP.getFreeHeap());

  if (!gNetInfoDone) {
    gNetInfoDone = true;
    gNetInfoUntil = millis() + NET_INFO_MS;
  }

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
  uint32_t nextRadarAt = 0;
  uint32_t nextBleAt = 20000;  // po WiFi i pierwszej pogodzie
  uint32_t nextRoomSaveAt = 0;
  bool firstWeather = false;

  for (;;) {
    // Okres próbny po OTA. MUSI tykać w każdej iteracji — także wtedy, gdy nie ma
    // WiFi — bo brak sieci to jeden z powodów, dla których wersję trzeba cofnąć.
    // Dlatego stoi PRZED wszystkimi "continue" poniżej.
    otaGuardTick();

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

    // ---- MQTT: utrzymanie sesji + telemetria urzadzenia ----
    // Brak brokera nie moze zatrzymac reszty: proby laczenia maja krotki timeout
    // i backoff, wiec ta linijka albo kosztuje mikrosekundy, albo raz na jakis
    // czas ~2 s (nieudany TCP connect). Nigdy nie blokuje na dluzej.
    mqttha::loop();

    // ---- pogoda ----
    if (static_cast<int32_t>(now - nextWeatherAt) >= 0) {
      if (!firstWeather) {
        setBootMsg("Pobieram prognozę...");
      }
      WeatherModel tmp{};
      if (weatherClient.fetch(tmp)) {
        xSemaphoreTake(gLock, portMAX_DELAY);
        // radar ma własny cykl — nie kasuj go świeżą prognozą
        tmp.radarLevel = gWeather.radarLevel;
        tmp.radarAgeSec = gWeather.radarAgeSec;
        tmp.radarValid = gWeather.radarValid;
        gWeather = tmp;
        xSemaphoreGive(gLock);
        nextWeatherAt = millis() + cfg::WEATHER_REFRESH_MS;
        firstWeather = true;
        diag().weatherOkAt = millis();
        diag().weatherErr[0] = '\0';
        LOG("Pogoda OK: %.1f C, kod %d, heap %lu\n", tmp.current.tempC,
            tmp.current.weatherCode, (unsigned long)ESP.getFreeHeap());
        mqttha::publishWeather(tmp);
      } else {
        strncpy(diag().weatherErr, tmp.errorMsg, sizeof(diag().weatherErr) - 1);
        LOG("Pogoda BLAD: %s (heap %lu)\n", tmp.errorMsg, (unsigned long)ESP.getFreeHeap());
        nextWeatherAt = millis() + 30000;
      }
    }

    // ---- fotowoltaika ----
    if (static_cast<int32_t>(now - nextPvAt) >= 0) {
      // Czy falownikowi wolno teraz spać? Godziny wschodu/zachodu mamy z prognozy.
      char sunrise[6], sunset[6];
      xSemaphoreTake(gLock, portMAX_DELAY);
      memcpy(sunrise, gWeather.sunrise, sizeof(sunrise));
      memcpy(sunset, gWeather.sunset, sizeof(sunset));
      xSemaphoreGive(gLock);
      const bool maySleep = pvMayBeAsleep(sunrise, sunset, localMinutesNow());

      PvModel tmp{};
      const bool ok = pvClient.fetch(tmp, maySleep);
      xSemaphoreTake(gLock, portMAX_DELAY);
      gPv = tmp;
      if (ok) {
        const time_t t = time(nullptr);
        if (t > 1700000000) {
          struct tm tmv{};
          localtime_r(&t, &tmv);
          const int prevDay = gHist.day;
          gHist.push(tmv.tm_yday, tmv.tm_hour, tmv.tm_min, tmp.data.powerAcW,
                     tmp.data.houseLoadW);
          if (prevDay != gHist.day) {
            // nowy dzień — profil wyczyszczony, zapisz od razu
            nextStoreAt = 0;
          }
        }
      }
      xSemaphoreGive(gLock);

      // Śpiący falownik odpytujemy co 5 minut zamiast co 30 s. Gdy tylko odpowie
      // (a potrafi odpowiadać jeszcze po zachodzie), wracamy do normalnego tempa.
      nextPvAt = millis() + (tmp.asleep ? cfg::PV_REFRESH_NIGHT_MS : cfg::PV_REFRESH_MS);

      if (ok) {
        diag().pvOkAt = millis();
        diag().pvErr[0] = '\0';
        diag().pvAsleep = false;
        LOG("PV: AC=%ldW DC=%ldW siec=%ldW dzis=%.2fkWh st=0x%04X\n",
                      static_cast<long>(tmp.data.powerAcW),
                      static_cast<long>(tmp.data.powerDcW),
                      static_cast<long>(tmp.data.gridPowerW), tmp.data.energyTodayKwh,
            tmp.data.statusCode);
      } else if (tmp.asleep) {
        // Stan neutralny: to nie jest błąd, więc nie zaśmiecamy nim diagnostyki
        // ani ekranu statystyk (kropka szara, nie czerwona).
        if (!diag().pvAsleep) {
          LOG("PV: falownik usypia (po zachodzie) — odpytuje co %lu min\n",
              static_cast<unsigned long>(cfg::PV_REFRESH_NIGHT_MS / 60000));
        }
        diag().pvErr[0] = '\0';
        diag().pvAsleep = true;
      } else {
        strncpy(diag().pvErr, tmp.errorMsg, sizeof(diag().pvErr) - 1);
        diag().pvAsleep = false;
        LOG("PV BLAD: %s\n", tmp.errorMsg);
      }
      // Do HA leci takze nieudany odczyt — jako moce 0 W i status "Offline"
      // (liczniki energii zostaja na ostatniej znanej wartosci, patrz MqttClient.cpp).
      mqttha::publishPv(tmp, ok);
    }

    // ---- radar opadowy (realny pomiar; model bywa ślepy na lokalne ulewy) ----
    if (static_cast<int32_t>(millis() - nextRadarAt) >= 0) {
      // Za ciasno na dekoder PNG? Poproś rdzeń 1 o oddanie bufora ekranu.
      const bool needMem = ESP.getMaxAllocHeap() < 48000;
      if (needMem) {
        gRadarWantMem = true;
        const uint32_t t0 = millis();
        while (!gRadarMemReady && millis() - t0 < 3000) vTaskDelay(pdMS_TO_TICKS(20));
      }

      RadarSnapshot rs{};
      const bool radarOk = radarClient.fetch(rs);

      if (needMem) {
        gRadarWantMem = false;  // rdzeń 1 odtworzy bufor przy najbliższej klatce
      }

      if (radarOk) {
        xSemaphoreTake(gLock, portMAX_DELAY);
        gWeather.radarLevel = rs.level;
        gWeather.radarAgeSec = rs.ageSec;
        gWeather.radarValid = true;
        xSemaphoreGive(gLock);
        diag().radarOkAt = millis();
        diag().radarLevel = rs.level;
        diag().radarAgeSec = rs.ageSec;
        diag().radarErr[0] = '\0';
        nextRadarAt = millis() + cfg::RADAR_REFRESH_MS;
      } else {
        strncpy(diag().radarErr, rs.errorMsg, sizeof(diag().radarErr) - 1);
        LOG("Radar BLAD: %s\n", rs.errorMsg);
        nextRadarAt = millis() + 60000;
      }
    }

    // ---- nasłuch czujników BLE (Xiaomi) ----
    // Skanujemy z przerwami, a nie ciągle: radio 2,4 GHz jest jedno i dzieli je
    // WiFi. Czujnik nadaje co kilka-kilkanaście sekund, więc 6 s nasłuchu co
    // 45 s spokojnie wystarczy, a WiFi (panel, OTA, MQTT) tego nie odczuwa.
    if (ble::ready() && static_cast<int32_t>(now - nextBleAt) >= 0) {
      ble::scan(6);
      mqttha::publishBle();

      // Historia 24 h: przewijamy okno i dopisujemy biezace odczyty.
      const time_t tt = time(nullptr);
      xSemaphoreTake(gLock, portMAX_DELAY);
      if (gRooms.advance(static_cast<uint32_t>(tt))) {
        for (int i = 0; i < ble::count(); ++i) {
          const ble::Sensor bs = ble::get(i);
          if (!bs.valid) continue;
          const Settings::BleCfg* bc = settings().bleFind(bs.mac);
          if (bc == nullptr) continue;
          for (int k = 0; k < 4; ++k) {
            if (&settings().ble[k] == bc) {
              gRooms.push(k, bs.hasTemp, bs.tempC, bs.hasHum, bs.humidity);
            }
          }
        }
        if (static_cast<int32_t>(millis() - nextRoomSaveAt) >= 0) {
          RoomHistory snap = gRooms;
          xSemaphoreGive(gLock);
          roomHistorySave(snap);          // NVS poza mutexem — zapis trwa
          nextRoomSaveAt = millis() + 600000;
          xSemaphoreTake(gLock, portMAX_DELAY);
        }
      }
      xSemaphoreGive(gLock);

      nextBleAt = millis() + 90000;
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
        diag().flightOkAt = millis();
        diag().flightsTotal = tmp.total;
        diag().flightErr[0] = '\0';
        LOG("Loty: %d w zasiegu, na liscie %d\n", tmp.total, tmp.count);
      } else {
        nextFlightAt = millis() + 20000;
        strncpy(diag().flightErr, tmp.errorMsg, sizeof(diag().flightErr) - 1);
        LOG("Loty BLAD: %s\n", tmp.errorMsg);
      }
    }

    // ---- OTA (jedyne miejsce, gdzie dotykamy obiektu Update) ----
    const bool otaAsked = takeOtaRequest();
    if (settings().otaEnabled &&
        (otaAsked || static_cast<int32_t>(millis() - nextOtaAt) >= 0)) {
      // otaAsked = kliknięcie w panelu; tylko wtedy wolno wymusić wersję, która
      // wcześniej nie przeżyła okresu próbnego.
      ota.checkAndUpdate(otaAsked);  // przy sukcesie restartuje urządzenie
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

// KLUCZOWE dla rollbacku. Arduino core (initArduino(), esp32-hal-misc.c) domyslnie
// sam potwierdza swiezy obraz OTA — jeszcze zanim ruszy setup(). Wtedy OtaGuard nigdy
// nie zobaczylby stanu PENDING_VERIFY i caly mechanizm bylby atrapa: /api/diag pisalby
// "stabilna", a ochrony by nie bylo (tak zachowywala sie v29).
// Ten slaby symbol przejmuje odpowiedzialnosc: obraz potwierdzamy sami, dopiero gdy
// udowodni, ze dziala (WiFi + udane pobranie po TLS + heap ponad progiem).
extern "C" bool verifyRollbackLater() {
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(200);
  LOG("=== Pogoda + PV — firmware v%d ===\n", FW_VERSION);

  settings().load();

  // Zanim cokolwiek zaalokujemy: sprawdź, czy to świeża wersja po OTA (okres
  // próbny) i zapamiętaj powód ostatniego resetu.
  otaGuardBegin();

  gLock = xSemaphoreCreateMutex();

  if (!ui.begin()) {
    LOG("BLAD: nie udalo sie utworzyc bufora TFT (wolny heap %lu B)\n",
        static_cast<unsigned long>(ESP.getFreeHeap()));
    // Regresja typu v14: nowa wersja zjadła tyle RAM, że nie ma na bufor ekranu.
    // Nie ma po co czekać 3 minuty — jeśli to wersja próbna, cofamy się natychmiast.
    otaGuardFatal("brak RAM na bufor ekranu");
    while (true) {
      delay(1000);
    }
  }

  if (cfg::COLOR_TEST_MODE) {
    ui.drawColorTest();
    return;
  }

  ledBegin();
  pvHistoryLoad(gHist);
  roomHistoryLoad(gRooms);
  uiRooms = gRooms;
  ui.setRoomHistory(&uiRooms);

  if (!settings().hasWifi()) {
    portal::beginAp();
    gBooting = false;
  }

  ui.drawBoot(gBootMsg, 0);

  // Zrzut ekranu leci z zadania web (rdzen 0) i NIE zatrzymuje rysowania (rdzen 1) —
  // inaczej ekran zamiera na czas wysylania BMP. Rysuje wlasna kopie klatki do
  // malego sprite'a, wiec bufora wyswietlacza w ogole nie dotyka. Czekamy tylko na
  // spokojna klatke, zeby nie zlapac ekranu w polowie przejscia miedzy widokami.
  portal::setScreenshotHandler([](WiFiClient& c) {
    // Nie w trakcie nasluchu BLE: zrzut alokuje sprite'y, a stos Bluetooth trzyma
    // wtedy 72 kB. Zbieg tych dwoch rzeczy zjechal sterta do 2 kB.
    uint32_t tw = millis();
    while (ble::scanning() && millis() - tw < 9000) delay(50);

    const uint32_t t0 = millis();
    while (!ui.stableFrame() && millis() - t0 < 800) delay(10);
    ui.streamScreenshot(c, uiWeather, uiPv, uiHist, uiFlights, gWifiOk);
  });

  portal::setViewHandler([](int i) { ui.pinView(i); },
                         [](int& cur, int& pin) { ui.viewState(cur, pin); });

  // BLE dopiero TERAZ — bufor ekranu (66 kB) jest już zaalokowany, więc stos
  // Bluetooth bierze z tego, co zostało, a nie odwrotnie. Gdyby zabrakło sterty,
  // OtaGuard i tak cofnie tę wersję: to jest właśnie ten scenariusz, przed którym
  // ma bronić.
  ble::begin();

  // Zrzut ekranu rysuje teraz calą klatkę (a nie tylko czyta bufor), więc stos zadania
  // web musi pomieścić cały łańcuch rysujący. 4 kB zapasu ze sterty, której i tak
  // przybyło 66 kB.
  xTaskCreatePinnedToCore(webTask, "web", 16384, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(netTask, "net", 16384, nullptr, 3, nullptr, 0);
}

// ------------------------------------------------------------------- loop ----

void loop() {
  if (cfg::COLOR_TEST_MODE) {
    delay(1000);
    return;
  }

  // --- radar prosi o pamięć: oddajemy bufor i nie rysujemy, dopóki nie skończy ---
  // releaseBuffer(false) = bez czyszczenia ekranu, więc panel zostaje z ostatnią
  // klatką zamiast mrugnąć na czarno.
  if (gRadarWantMem) {
    if (!gRadarMemReady) {
      ui.releaseBuffer(false);
      gRadarMemReady = true;
    }
    delay(30);
    return;
  }
  gRadarMemReady = false;  // render() sam odtworzy bufor przy najbliższej klatce

  const uint32_t now = millis();

  // --- test diody RGB przy starcie (3 x 1,5 s) ---
  if (const char* colorName = ledTestStep()) {
    ui.drawLedTest(colorName);
    delay(40);
    return;
  }

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

  // --- aktualizacja w toku: oddaj bufor ekranu i rysuj wprost na TFT ---
  const OtaStatus& os = otaStatus();
  if (os.state == OtaState::DOWNLOADING || os.state == OtaState::DONE ||
      os.state == OtaState::FAILED) {
    ui.releaseBuffer();
    otaUiBufferFreed();
    ui.drawOtaDirect(os.progress, os.message);
    delay(50);
    return;
  }

  // --- ekran "połączono" z adresem IP (10 s po starcie) ---
  if (gNetInfoUntil != 0) {
    if (static_cast<int32_t>(now - gNetInfoUntil) < 0) {
      const int left = static_cast<int>((gNetInfoUntil - now + 999) / 1000);
      ui.drawNetInfo(settings().ssid, gIpStr, gRssi, left, NET_INFO_MS / 1000);
      delay(60);
      return;
    }
    gNetInfoUntil = 0;
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
  uiRooms = gRooms;
  uiFlights = gFlights;
  xSemaphoreGive(gLock);

  gFlightsNeeded = ui.needsFlights(now);

  // --- tryb nocny + dioda RGB (bilans z siecią) ---
  bool night = false;
  const time_t t = time(nullptr);
  if (t > 1700000000) {
    struct tm tmv{};
    localtime_r(&t, &tmv);
    night = (tmv.tm_hour >= cfg::NIGHT_FROM_H) || (tmv.tm_hour < cfg::NIGHT_TO_H);
    ui.setBacklightTarget(night ? cfg::BL_NIGHT : cfg::BL_DAY);
  }
  ledShowGrid(uiPv.data.gridPowerW, uiPv.online, night);

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
