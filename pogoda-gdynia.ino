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
#include "BleGateway.h"
#include "BleSensors.h"
#include "OtaGuard.h"
#include "RadarMap.h"
#include "GasMeter.h"
#include "Viessmann.h"
#include "RoomHistory.h"
#include "Touch.h"

#include <esp_wifi.h>
#include <esp_event.h>
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
vi::Model gVi{};
vi::Model uiVi{};
BurnerHistory gBurner{};
BurnerHistory uiBurner{};
GasHistory gGas{};
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
// Uchwyty zadan — potrzebne, zeby zmierzyc, ile stosu naprawde zuzywaja.
// Oba maja po 16 kB w SRAM (stosow FreeRTOS nie da sie trzymac w PSRAM), wiec
// jesli realnie biora 6-8 kB, mamy do odzyskania kilkanascie kB.
TaskHandle_t gNetTask = nullptr;
TaskHandle_t gWebTask = nullptr;

// Roaming. 802.11r (Fast Transition) NIE jest wkompilowany w rdzen Arduino —
// sprawdzone w symbolach: zero wpa_ft_*. Zostaje przelaczanie "recznie":
// rozlaczenie i polaczenie z mocniejszym punktem po jego BSSID (1-3 s przerwy).
// Ale MOMENT decyzji bierzemy juz ze sprzetu: esp_wifi_set_rssi_threshold()
// wyzwala zdarzenie, gdy sygnal spadnie ponizej progu — nie musimy odpytywac.
volatile bool gRoamWanted = false;
constexpr int kRssiRoamBelow = -67;   // ponizej tego szukamy czegos lepszego
constexpr int kRssiRoamGain = 8;      // ...i przenosimy sie tylko przy realnym zysku

// Zdarzenie leci z zadania systemowego WiFi — nie wolno tu nic robic dlugo.
// Tylko podnosimy flage; przenosinami zajmie sie netTask.
void onRssiLow(void*, esp_event_base_t, int32_t, void*) {
  gRoamWanted = true;
}

// gRadarWantMem/gRadarMemReady usuniete razem z mechanizmem oddawania bufora —
// patrz komentarz przy pobieraniu radaru. releaseBuffer() zostaje: uzywa go OTA
// (tam zwolnienie 129 kB PSRAM ma sens, bo Update alokuje wlasny bufor).
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
// Rozmiar z wartownika enuma, nie z literalu. Zaszyta osemka byla DOKLADNIE rowna
// liczbie pozycji AlertKind — czyli tablica wygladala na zapasowa, a byla pelna.
uint32_t lastAlertAt[static_cast<int>(AlertKind::COUNT)] = {};

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

  // Domyslnie ESP32 laczy sie z PIERWSZYM punktem, ktory odpowie na dana nazwe
  // (WIFI_FAST_SCAN) — a nie z najmocniejszym. Przy dwoch punktach o tej samej
  // nazwie potrafilo to zlapac ten dalszy: w logach mielismy raz -49 dBm, raz
  // -78 dBm w tym samym miejscu. Kazemy przeskanowac WSZYSTKIE kanaly i wybrac
  // najsilniejszy sygnal. Kosztuje to ~1 s dluzszego laczenia przy starcie.
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);

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

  // Uzbrajamy sprzetowy prog. Zdarzenie jest JEDNORAZOWE — po kazdym wyzwoleniu
  // trzeba je uzbroic ponownie (robimy to po probie przenosin).
  esp_wifi_set_rssi_threshold(kRssiRoamBelow);

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
  uint32_t nextGwAt = 12000;
  uint32_t nextRoomSaveAt = 0;
  uint32_t nextRoamAt = 120000;   // pierwszy przeglad po 2 min od startu
  uint32_t nextRadarMapAt = 25000;
  uint32_t nextViAt = 35000;
  bool firstWeather = false;

  for (;;) {
    // Okres próbny po OTA. MUSI tykać w każdej iteracji — także wtedy, gdy nie ma
    // WiFi — bo brak sieci to jeden z powodów, dla których wersję trzeba cofnąć.
    // Dlatego stoi PRZED wszystkimi "continue" poniżej.
    otaGuardTick();

    // ---- polnoc: profile doby (palnik I fotowoltaika) przestaja byc "dzis" ----
    // MUSI tykac niezaleznie od odpytu pieca i falownika — i dlatego stoi TU, przed
    // wszystkimi "continue" ponizej, obok otaGuardTick(). Bez WiFi tez.
    //
    // OBIE historie mialy DOKLADNIE ten sam blad, bo maja dokladnie ta sama budowe:
    // reset() jest wolany WYLACZNIE z push(), a push() leci tylko po UDANYM fetchu.
    // Nie ma fetchu — nie ma push() — nie ma resetu — `day` zostaje wczorajszy,
    // a 144 sloty trzymaja wczorajsze slupki. Oba wykresy sa podpisane "DZIS"
    // (WeatherUi.cpp:575 — stopka wykresu PV, WeatherUi.cpp:2313 — "PRACA PALNIKA
    // DZIŚ") i ZADEN z nich nie sprawdza doby. Ufaja tym strukturom.
    //
    // PALNIK: profil przezywa restart w NVS, wiec restart o 00:10 odtwarzal profil
    // WCZORAJSZY z wczorajszym `day`. Do pierwszego udanego odpytu — minutami.
    // A gdy piec milczy (token wygasl, API lezy, nie ma WiFi) — godzinami.
    //
    // FOTOWOLTAIKA: gorzej, bo tu nie trzeba ZADNEGO restartu. pvClient.fetch()
    // zwraca ok == false przez cala noc (Huawei wylacza Modbus TCP po zachodzie —
    // patrz PvModel::asleep), wiec: asleep => ok == false => push nie leci => reset
    // nie leci. Co noc, od polnocy do pierwszego udanego odczytu po wschodzie
    // slonca, ekran fotowoltaiki pokazywal WCZORAJSZA krzywa pod napisem "DZIS".
    // Zima to 8 godzin dziennie. To nie byl blad restartu, to bylo codzienne
    // klamstwo. Store co 5 min utrwalal je jeszcze do NVS.
    //
    // DLACZEGO KASUJEMY, A NIE PODPISUJEMY "WCZORAJ": pusty wykres o 03:00 jest
    // PRAWDZIWY — dzisiejsza produkcja naprawde wynosi wtedy zero. Wczorajsza
    // krzywa podpisana "dzis" to nieprawda, nawet jesli ladniejsza od pustki.
    // Ta sama klasa bledu, co "wilgotnosc pokazana jako temperatura".
    //
    // Warunki, obydwa istotne (dla obu historii tak samo):
    //   day >= 0        — nie bylo jeszcze ZADNEGO profilu, nie ma czego kasowac;
    //   day != tm_yday  — profil odtworzony z NVS w TEJ SAMEJ dobie zostaje
    //                     nietkniety, po to go zapisujemy (kapiel 8:00, restart
    //                     14:00, slupek ma byc).
    // Zegar musi byc pewny (NTP), inaczej skasowalibysmy dobry profil na podstawie
    // daty z 1970. Sam reset to 144 sloty w RAM — wolno go zrobic pod gLock, bo to
    // ani siec, ani NVS.
    //
    // GasHistory (gGas) NIE jest tu ruszana i to jest swiadome: to bufor 120 dni,
    // ktory przewija sie sam przez advance(epoch) zerujac przespane doby, a jego
    // `head` zawsze wskazuje wlasciwy dzien, bo push() leci tylko tuz po udanym
    // advance(). Nie ma tam "dzis", ktore moglo by sklamac — czyta go wylacznie
    // sumBetween() po jawnym zakresie dat.
    {
      const time_t tt = time(nullptr);
      if (tt > 1700000000) {
        struct tm tmv{};
        localtime_r(&tt, &tmv);
        xSemaphoreTake(gLock, portMAX_DELAY);
        const bool burnerRolled = (gBurner.day >= 0 && gBurner.day != tmv.tm_yday);
        if (burnerRolled) {
          gBurner.reset(tmv.tm_yday);
        }
        const bool pvRolled = (gHist.day >= 0 && gHist.day != tmv.tm_yday);
        if (pvRolled) {
          gHist.reset(tmv.tm_yday);
        }
        xSemaphoreGive(gLock);
        // Pusty profil trafi do NVS przy najblizszym zapisie (co 5 min, czyli o 00:05)
        // i tak ma byc: nowa doba NAPRAWDE jest pusta. Guard `snap.day >= 0` przy
        // zapisie tego nie blokuje i nie ma blokowac — po reset(tm_yday) `day` jest
        // >= 0, bo profil ISTNIEJE, tylko jest (zgodnie z prawda) pusty. Ten guard
        // odsiewa co innego: day == -1, czyli "nie bylo jeszcze ANI JEDNEGO odczytu",
        // kiedy nadpisanie NVS skasowaloby profil odtworzony przy starcie.
        // Wczorajszej doby i tak nie umiemy pokazac — obie struktury trzymaja jedna.
        if (burnerRolled) {
          LOG("Piec: nowa doba — profil palnika wyzerowany (bez czekania na odpyt)");
        }
        if (pvRolled) {
          LOG("PV: nowa doba — profil produkcji wyzerowany (bez czekania na falownik)");
        }
      }
    }

    if (!settings().hasWifi()) {
      gBooting = false;
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    // Panel testuje wlasnie nowe dane WiFi — nie wchodzimy mu w droge. Nasze
    // WiFi.begin() poszloby ze STARYMI danymi z NVS, zabiloby jego probe, a panel
    // uznalby powrot na stara siec za dowod, ze nowe haslo dziala. Flaga wygasa
    // sama (portal::wifiConfigBusy), wiec nie da sie tu zawisnac na stale.
    if (portal::wifiConfigBusy()) {
      gWifiOk = (WiFi.status() == WL_CONNECTED);
      vTaskDelay(pdMS_TO_TICKS(250));
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
      // USUNIETE: proszenie rdzenia 1 o oddanie bufora ekranu przed dekodowaniem PNG.
      //
      // Ten mechanizm zamrazal ekran na CALY czas pobierania radaru (nie na 3 s —
      // 3000 ms to bylo tylko czekanie na potwierdzenie z rdzenia 1; `fetch()` leci
      // dopiero potem, a `gRadarWantMem` wraca na false po nim). Co 5 minut, bo
      // warunek `getMaxAllocHeap() < 48000` jest na tym urzadzeniu spelniony ZAWSZE
      // (zmierzone: largest_block ~39-43 kB).
      //
      // I nie mogl pomoc, bo mierzyl co innego, niz zwalnial:
      //   - getMaxAllocHeap() to MALLOC_CAP_INTERNAL, czyli SRAM,
      //   - a bufor sprite'a (129 kB) siedzi w PSRAM. Nie przez TFT_eSPI — jego
      //     `#if defined(CONFIG_SPIRAM_SUPPORT)` jest FALSZYWY, bo rdzen 3.3.10
      //     definiuje CONFIG_SPIRAM, nie CONFIG_SPIRAM_SUPPORT. Trafia tam przez
      //     CONFIG_SPIRAM_USE_MALLOC=1 z CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096:
      //     kazda alokacja > 4 kB idzie do PSRAM sama.
      // Czyli: oddawalismy PSRAM, zeby zrobic miejsce w SRAM-ie.
      //
      // Prawdziwy straznik jest w RadarClient (prog 64 kB, licznik radar.skips_low_ram)
      // i na urzadzeniu pokazuje ZERO — dekoder nigdy nie odpuscil z braku pamieci.
      RadarSnapshot rs{};
      const bool radarOk = radarClient.fetch(rs);

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

    // ---- animowana mapa opadow (7 kafelkow, 2 h wstecz) ----
    // Idzie PO radarze punktowym i PRZED lotami: sciaga 7 obrazkow, wiec nie chcemy
    // tego robic czesto ani rownolegle z niczym ciezkim.
    if (radarmap::wantsFetch() || static_cast<int32_t>(millis() - nextRadarMapAt) >= 0) {
      if (radarmap::fetch()) {
        nextRadarMapAt = millis() + cfg::RADAR_MAP_REFRESH_MS;
      } else {
        LOG("Radar mapa BLAD: %s", radarmap::lastError());
        nextRadarMapAt = millis() + 120000;
      }
    }

    // ---- piec Viessmann (chmura ViCare) ----
    // Co 3 minuty = 480 zapytan na dobe. Limit Viessmanna to 1450/dobe i 120 na
    // 10 minut, wiec mamy zapas takze na odswiezanie tokena (co ~55 min).
    if (settings().hasViessmann() && static_cast<int32_t>(millis() - nextViAt) >= 0) {
      vi::Model tmp{};
      const bool ok = vi::fetch(tmp);
      bool gasWantSave = false;
      xSemaphoreTake(gLock, portMAX_DELAY);
      if (ok) {
        gVi = tmp;
        diag().viOkAt = millis();
        diag().viErr[0] = '\0';
        diag().viDhwC = tmp.dhwTempC;
        diag().viSupplyC = tmp.supplyTempC;

        // Surowe liczniki do /api/diag (patrz Log.h). Przepisujemy je BEZ zadnej
        // obrobki — cala wartosc tych pol polega na tym, ze pokazuja to, co
        // naprawde przyszlo z pieca.
        // Kazda liczba pod WLASNA flaga: `hours` i `starts` to osobne wlasciwosci
        // i moga przyjsc niezaleznie. Przepisujemy tylko to, co naprawde doszlo —
        // inaczej polowiczna odpowiedz zostawialaby w diagnostyce ostatnia dobra
        // wartosc podpisana swieza flaga.
        diag().viHasBurnerHours = tmp.hasBurnerHours;
        diag().viHasBurnerStarts = tmp.hasBurnerStarts;
        if (tmp.hasBurnerHours) diag().viBurnerHours = tmp.burnerHours;
        if (tmp.hasBurnerStarts) diag().viBurnerStarts = tmp.burnerStarts;
        diag().viBurnerActive = tmp.burnerActive;
        // "Ostatnia ZLAPANA": nadpisujemy tylko, gdy modulacja faktycznie doszla.
        // Brakujaca cecha zapisalaby tu zero, czyli "palnik stoi" — a to jest dokladnie
        // to klamstwo, ktore mamy zmierzyc, a nie powielic.
        if (tmp.hasModulation) diag().viModulation = tmp.modulationPct;
        diag().viHasGas = tmp.hasGas;
        if (tmp.hasGas) diag().viGasDayM3 = tmp.gasDhwM3 + tmp.gasHeatM3;

        // Profil doby palnika + wlasny log zuzycia gazu. Log jest wlasny, bo
        // liczniki miesieczne/roczne pieca sa zepsute (currentMonth < lastSevenDays,
        // currentYear = 5.3 m3 po 4 latach) — ufamy tylko currentDay.
        const time_t tt = time(nullptr);
        if (tt > 1700000000) {
          struct tm tmv{};
          localtime_r(&tt, &tmv);
          // Do historii wpisujemy TYLKO to, co model potwierdza flaga. Bez tego
          // brak cechy "heating.burners.0" w odpowiedzi API zapisywal "palnik nie
          // pracowal" w slocie, w ktorym pracowal — a to jest zapis TRWALY, ktory
          // potem klamie na wykresie doby. Zero jest tu nieodroznialne od pomiaru.
          if (tmp.hasBurnerState) {
            gBurner.push(tmv.tm_yday, tmv.tm_hour, tmv.tm_min,
                         tmp.hasModulation ? tmp.modulationPct : 0, tmp.burnerActive);
          }
          if (tmp.hasGas) {
            const uint32_t prevDay = gGas.lastDay;
            if (gGas.advance(static_cast<uint32_t>(tt))) {
              gGas.push(tmp.gasDhwM3 + tmp.gasHeatM3);
              // Zapis RAZ NA DOBE, przy przewinieciu dnia — nie co 3 minuty.
              // Doba wlasnie sie zamknela, wiec jej suma jest juz ostateczna.
              // Snapshot na stos (248 B) i zapis POZA mutexem: NVS nigdy pod gLock.
              if (prevDay != 0 && gGas.lastDay != prevDay) gasWantSave = true;
            }
          }
        }
      } else {
        gVi.valid = false;
        snprintf(diag().viErr, sizeof(diag().viErr), "%s", tmp.err);
        LOG("Piec BLAD: %s", tmp.err);
      }

      // Snapshot pod mutexem, zapis POZA nim — NVS nigdy pod gLock (rdzen 1
      // renderuje 30 klatek/s i czeka na te sama blokade).
      GasHistory gasSnap;
      if (gasWantSave) gasSnap = gGas;
      xSemaphoreGive(gLock);
      if (gasWantSave) {
        gasHistorySave(gasSnap);
        LOG("Gaz: zamknieta doba, log zapisany");
      }
      nextViAt = millis() + (ok ? 180000UL : 120000UL);
    }

    // ---- bramka BLE (Shelly slyszy to, czego my nie slyszymy) ----
    // Co 20 s, bo to tanie: jeden GET po WiFi, kilkaset bajtow. Nasze wlasne radio
    // dziala dalej — bramka tylko dokłada uszu, nie zastepuje ich.
    if (settings().bleGwHost[0] != '\0' && static_cast<int32_t>(millis() - nextGwAt) >= 0) {
      blegw::poll();
      nextGwAt = millis() + 20000;
    }

    // ---- nasłuch czujników BLE (Xiaomi) ----
    // Skanujemy z przerwami, a nie ciągle: radio 2,4 GHz jest jedno i dzieli je
    // WiFi. Czujnik nadaje co kilka-kilkanaście sekund, więc 6 s nasłuchu co
    // 45 s spokojnie wystarczy, a WiFi (panel, OTA, MQTT) tego nie odczuwa.
    if (ble::ready() && static_cast<int32_t>(now - nextBleAt) >= 0) {
      ble::scan(4);
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
          // ROOMS, nie "4" — Settings ma osiem slotow BLE, historia szesc.
          // Zaszyta czworka oznaczala, ze piaty czujnik dalo sie skonfigurowac
          // w panelu, ale nie trafial ani do historii, ani na ekran.
          for (int k = 0; k < RoomHistory::ROOMS; ++k) {
            if (&settings().ble[k] == bc) {
              gRooms.push(k, bs.hasTemp, bs.tempC);
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

      nextBleAt = millis() + 20000;
    }

    // ---- roaming: czy nie wisimy na slabszym punkcie dostepowym? ----
    // ESP32 sam NIE przechodzi miedzy punktami — raz zlapany trzyma do konca.
    // Gdy sygnal jest kiepski, skanujemy i przenosimy sie na wyraznie lepszy.
    // Warunek "wyraznie" (>= 8 dB) chroni przed skakaniem tam i z powrotem.
    const bool roamNow = gWifiOk && (gRoamWanted ||
                                     static_cast<int32_t>(now - nextRoamAt) >= 0);
    if (roamNow) {
      gRoamWanted = false;
      nextRoamAt = millis() + 180000;
      const int cur = WiFi.RSSI();
      if (cur < kRssiRoamBelow - 1) {
        // Bufor wynikow skanu jest w rdzeniu JEDEN i wspolny z apiScan ("Wyszukaj
        // sieci" w panelu), a scanNetworks() zaczyna od jego zwolnienia. Blokada musi
        // objac skan, odczyt wynikow i scanDelete() — inaczej klikniecie skanu w tej
        // chwili wywracalo urzadzenie. To, co potrzebne dalej (BSSID, kanal),
        // kopiujemy do zmiennych lokalnych JESZCZE pod blokada; samo przelaczanie
        // (disconnect + begin, kilka sekund) idzie juz bez niej.
        uint8_t bssid[6] = {};
        int32_t ch = 0;
        int bestRssi = 0;
        bool move = false;
        const bool scanned = portal::scanLock(20000);
        if (scanned) {
          const int found = WiFi.scanNetworks(false, false, false, 250);
          int best = -1;
          bestRssi = cur + kRssiRoamGain;
          for (int i = 0; i < found; ++i) {
            if (WiFi.SSID(i) != settings().ssid) continue;
            if (WiFi.RSSI(i) > bestRssi) {
              bestRssi = WiFi.RSSI(i);
              best = i;
            }
          }
          if (best >= 0) {
            memcpy(bssid, WiFi.BSSID(best), 6);
            ch = WiFi.channel(best);
            move = true;
          }
          WiFi.scanDelete();
          portal::scanUnlock();
        }

        if (!scanned) {
          LOG("WiFi: skan zajety przez panel — przeglad roamingowy pomijam");
        } else if (move) {
          LOG("WiFi: przenosze sie na mocniejszy punkt (%d -> %d dBm)", cur, bestRssi);
          WiFi.disconnect();
          WiFi.begin(settings().ssid, settings().pass, ch, bssid);
          for (int i = 0; i < 60 && WiFi.status() != WL_CONNECTED; ++i) {
            vTaskDelay(pdMS_TO_TICKS(100));
          }
          diag().wifiRoams++;
          LOG("WiFi: po przenosinach %d dBm", WiFi.RSSI());
        } else {
          LOG("WiFi: sygnal %d dBm, ale nic lepszego nie ma — zostajemy", cur);
        }
      }
      // Prog jest jednorazowy: po wyzwoleniu trzeba go uzbroic od nowa.
      esp_wifi_set_rssi_threshold(kRssiRoamBelow);
    }

    // ---- ile stosu naprawde zuzywaja zadania (do przyciecia 2 x 16 kB) ----
    if (gNetTask != nullptr) {
      diag().stackNet = uxTaskGetStackHighWaterMark(gNetTask) * sizeof(StackType_t);
    }
    if (gWebTask != nullptr) {
      diag().stackWeb = uxTaskGetStackHighWaterMark(gWebTask) * sizeof(StackType_t);
    }

    // ---- zapis profili doby do NVS: produkcja PV + palnik ----
    // Oba profile jada na jednym zegarze (co 5 min), bo maja identyczny problem:
    // trzymaja JEDNA dobe, ktora jest kasowana o polnocy, wiec jedyne, co moze je
    // uratowac przed restartem, to zapis W TRAKCIE doby.
    //
    // DLACZEGO PALNIK NIE ZAPISUJE SIE "RAZ NA DOBE PRZY ZMIANIE day", JAK GAZ:
    // bo dla tej struktury to nie dziala. GasHistory to bufor 120 dni — zamknieta
    // doba zostaje w nim na zawsze, wiec zapis na przewinieciu dnia utrwala gotowa
    // sume. BurnerHistory ma tylko biezaca dobe i BurnerHistory::reset() zeruje
    // wszystkie 144 sloty przy zmianie tm_yday. Zapis dokladnie w tym momencie
    // wpisalby do NVS PUSTY profil, a restart o 14:00 odtworzylby z niego zero —
    // czyli ten sam objaw, ktory naprawiamy ("wykres pieca nie pamieta po resecie").
    // PvHistory ma to samo ograniczenie i z tego samego powodu zapisuje sie co 5 min.
    // To jest CALY powod, dla ktorego "fotowoltaika pamieta, a piec nie".
    //
    // Koszt: 296 B blobu (struktura na stosie ma 292 — reszta to `ver` i wyrownanie)
    // co 5 min, obok istniejacych 584 B (PV) w tym samym takcie i
    // 1736 B co 10 min (pokoje). NVS jest wear-levelowane, a to najmniejszy z tych
    // trzech zapisow — nie zmienia rzedu wielkosci zuzycia.
    if (static_cast<int32_t>(now - nextStoreAt) >= 0) {
      // Osobne zakresy: dwa snapshoty (724 B + 292 B) nie musza zyc naraz na stosie
      // netTask. Zapis ZAWSZE poza gLock — rdzen 1 czeka na te sama blokade z klatka.
      {
        xSemaphoreTake(gLock, portMAX_DELAY);
        PvHistory snap = gHist;
        xSemaphoreGive(gLock);
        if (snap.day >= 0) {
          pvHistorySave(snap);
        }
      }
      {
        xSemaphoreTake(gLock, portMAX_DELAY);
        BurnerHistory snap = gBurner;
        xSemaphoreGive(gLock);
        // day < 0 = nie bylo jeszcze ANI JEDNEGO odczytu pieca (piec wylaczony,
        // brak autoryzacji, same bledy). Pusty profil nadpisalby ten z NVS
        // i skasowal to, co wlasnie odtworzylismy przy starcie.
        if (snap.day >= 0) {
          burnerHistorySave(snap);
        }
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
    snprintf(a.text, sizeof(a.text), "Status 0x%04X - sprawdź instalację",
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
      snprintf(a.text, sizeof(a.text), "Burza nad %s - teraz", settings().city);
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
    snprintf(a.text, sizeof(a.text), "Do %.1f mm/h - za %d h", maxRain, rainIn);
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
    snprintf(a.text, sizeof(a.text), "Do %.0f°C - pij wodę", maxTemp);
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

  // PSRAM: do v49 kompilowalismy z PSRAM=disabled, wiec psramInit() nigdy sie nie
  // wykonywal i diagnostyka pokazywala "psram: 0". To nie byl dowod braku pamieci,
  // tylko dowod, ze o nia nie pytalismy. Teraz pytamy.
  if (psramFound()) {
    LOG("PSRAM: JEST — %lu kB (wolne %lu kB)",
        static_cast<unsigned long>(ESP.getPsramSize() / 1024),
        static_cast<unsigned long>(ESP.getFreePsram() / 1024));
  } else {
    LOG("PSRAM: brak (uklad bez PSRAM albo init sie nie powiodl)");
  }

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

  // Czujniki: PIR cyfrowy + LDR analog.
  // GOLY INPUT, nie INPUT_PULLDOWN. Poczatkowo dalem pulldown "na wszelki wypadek",
  // ale pomiar na urzadzeniu (v100) pokazal, ze przekombinowalem: spoczynek jest
  // stabilnie LOW (SR505 sam trzyma low, wiec floating nie grozi), za to pulldown 45k
  // sciagal stan WYSOKI ponizej progu VIH i czesc machniec nie wyzwalala PIR
  // (pir_last_s rosl do 100+ mimo ciaglego ruchu). Bez pulldownu SR505 steruje sam.
  // analogRead na GPIO1 = ADC1 dziala przy WiFi; ADC_11db daje pelny zakres ~0-3,1V
  // pod dzielnik 3,3V. Nastawy sa domyslne, ustawiamy je jawnie dla pewnosci.
  pinMode(cfg::PIN_PIR, INPUT);
  analogSetPinAttenuation(cfg::PIN_LDR, ADC_11db);

  pvHistoryLoad(gHist);
  roomHistoryLoad(gRooms);
  uiRooms = gRooms;
  ui.setRoomHistory(&uiRooms);
  gasHistoryLoad(gGas);   // 120 dni logu gazu — bez tego weryfikacja licznika nie ma z czym porownywac
  burnerHistoryLoad(gBurner);   // profil doby palnika — dotad ginal przy KAZDYM restarcie
  uiBurner = gBurner;           // zeby wykres mial dane JUZ w pierwszej klatce, przed pierwszym odpytem pieca
  ui.setBoiler(&uiVi);
  ui.setBurnerHistory(&uiBurner);

  if (!settings().hasWifi()) {
    portal::beginAp();
    gBooting = false;
  }

  ui.drawBoot(gBootMsg, 0);

  // Zrzut ekranu leci z zadania web (rdzen 0) i NIE zatrzymuje rysowania (rdzen 1) —
  // inaczej ekran zamiera na czas wysylania BMP. Rysuje wlasna kopie klatki do
  // malego sprite'a, wiec bufora wyswietlacza w ogole nie dotyka. Czekamy tylko na
  // spokojna klatke, zeby nie zlapac ekranu w polowie przejscia miedzy widokami.
  //
  // SWIADOMA DECYZJA: czytamy uiWeather/uiPv/uiHist/uiFlights BEZ gLock, chociaz
  // loop() (rdzen 1) przepisuje je pod gLock. Zrzut trwa setki ms, wiec sklada dane
  // z kilku roznych chwil — podglad w panelu moze pokazac klatke, ktorej na TFT nie
  // bylo. Crashu tu nie ma: wszystkie tablice w tych strukturach maja stala dlugosc,
  // a liczniki (np. FlightModel::count) nie wyjda poza zakres nawet przy rozdarciu.
  // Wziecie gLock na czas calego zrzutu zablokowaloby netTask i rysowanie na setki ms
  // (panel ciagnie /api/screen co 700 ms) — zamienilibysmy niescisly podglad na
  // zacinajace sie urzadzenie. Wniosek: zrzut to PODGLAD, nie dowod w diagnostyce.
  // Od diagnostyki jest /api/diag i /api/log.
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
  touch::begin();
  radarmap::begin();

  esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_BSS_RSSI_LOW, &onRssiLow, nullptr);

  // Zrzut ekranu rysuje teraz calą klatkę (a nie tylko czyta bufor), więc stos zadania
  // web musi pomieścić cały łańcuch rysujący. 4 kB zapasu ze sterty, której i tak
  // przybyło 66 kB.
  xTaskCreatePinnedToCore(webTask, "web", 16384, nullptr, 2, &gWebTask, 0);
  xTaskCreatePinnedToCore(netTask, "net", 16384, nullptr, 3, &gNetTask, 0);
}

// ------------------------------------------------------------------- loop ----

void loop() {
  if (cfg::COLOR_TEST_MODE) {
    delay(1000);
    return;
  }

  const uint32_t now = millis();

  // --- czujniki v100 (LDR analog + PIR cyfrowy), surowo do /api/diag ---
  // Co 250 ms wystarczy: LDR jest wolnozmienny, a SR505 trzyma OUT ~8 s po ruchu,
  // wiec polling nie zgubi zbocza. Odczyt to mikrosekundy, nie rusza mutexa (Diag to
  // migawka bez blokad — pojedyncze pole rozjechane o klatke nikogo nie boli).
  {
    static uint32_t nextSensorAt = 0;
    if (static_cast<int32_t>(now - nextSensorAt) >= 0) {
      nextSensorAt = now + 250;
      uint32_t acc = 0;
      for (int i = 0; i < 8; ++i) acc += analogRead(cfg::PIN_LDR);  // usredniamy szum ADC
      diag().ldrRaw = static_cast<uint16_t>(acc / 8);
      diag().ldrMv = static_cast<uint16_t>(analogReadMilliVolts(cfg::PIN_LDR));
      const bool pir = digitalRead(cfg::PIN_PIR) != 0;
      diag().pirState = pir;
      if (pir) diag().pirLastAt = now;
    }
  }

  // --- dotyk GPIO7 ---
  switch (touch::poll()) {
    case touch::Tap::SINGLE:
      ui.restartHold();
      LOG("Dotyk: odliczanie ekranu od nowa");
      break;
    case touch::Tap::DOUBLE:
      ui.prevView();
      LOG("Dotyk x2: poprzedni ekran");
      break;
    default:
      break;
  }

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
  uiVi = gVi;
  uiBurner = gBurner;
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

  // TAKTOWANIE KLATEK, nie stala pauza. Poprzednio bylo delay(33) PO renderowaniu,
  // wiec okres klatki wynosil "rysowanie + 33 ms": przy 66 ms rysowania dawalo to
  // 99 ms (10 fps). Podniesienie SPI z 27 na 80 MHz scielo rysowanie do 32 ms,
  // ale pauza i tak doklejala swoje 33 — z 31 klatek, ktore sprzet potrafi,
  // polowa szla do kosza. Teraz czekamy tylko RESZTE okresu.
  // Cel liczymy od POPRZEDNIEGO CELU, nie od "teraz". Inaczej — a tak wlasnie
  // zrobilem za pierwszym razem — po kazdym renderowaniu jestesmy "spoznieni"
  // wzgledem samych siebie, resetujemy baze i czekamy pelny okres. Czyli znowu
  // "rysowanie + 70 ms" (zmierzone: 110 ms / 9 fps) zamiast rownych 70 ms.
  static uint32_t nextFrameAt = 0;
  const uint32_t period = animating ? cfg::FRAME_ACTIVE_MS : cfg::FRAME_IDLE_MS;

  if (nextFrameAt == 0) nextFrameAt = millis();
  nextFrameAt += period;

  int32_t left = static_cast<int32_t>(nextFrameAt - millis());
  if (left < 1) {
    // Rysowanie nie wyrobilo sie w okresie. Jesli zostalismy w tyle o wiecej niz
    // caly okres (zrzut ekranu, OTA), kasujemy dlug — inaczej gonilibysmy strate
    // serią klatek bez pauzy i zaglodzili pozostale zadania.
    if (left < -static_cast<int32_t>(period)) nextFrameAt = millis();
    left = 1;
  }
  delay(left);

  // rzeczywisty okres klatki (srednia kroczaca) — zeby bylo widac, czy cel 30 fps
  // jest naprawdę osiągany, a nie tylko deklarowany
  static uint32_t lastFrame = 0;
  if (lastFrame != 0) {
    diag().framePeriodUs = (diag().framePeriodUs * 7 + (micros() - lastFrame)) / 8;
  }
  lastFrame = micros();
}
