#include "MqttClient.h"

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>

#include "BleSensors.h"
#include "Log.h"
#include "Settings.h"
#include "Version.h"
// Celowo NIE wlaczamy WeatherIcons.h — to naglowek z ~96 kB tablic ikon
// (static const w naglowku = osobna kopia w kazdej jednostce kompilacji)
// i calym TFT_eSPI w zaleznosciach. Opis pogody skladamy nizej sami.

namespace mqttha {
namespace {

// --- budzet pamieci -----------------------------------------------------------
// PubSubClient trzyma JEDEN bufor na stercie (domyslnie 256 B). Najwiekszy pakiet,
// jaki wysylamy, to retained config encji. Przy maksymalnym prefiksie (23 znaki)
// i najdluzszej nazwie encji wychodzi 430 B razem z tematem i naglowkiem — 512 B
// daje na to zapas, a jest to okolo 1/80 tego, co zjada jeden handshake TLS.
// Nic nie subskrybujemy, wiec bufor wejsciowy (ten sam) nie musi byc wiekszy:
// przychodza tylko CONNACK (4 B) i PINGRESP (2 B).
// publishConfig() i tak sprawdza rozmiar kazdego pakietu i odrzuca za duze.
constexpr uint16_t kBufSize = 512;

constexpr uint16_t kKeepAliveS = 60;      // PINGREQ co minute, nie co 15 s
constexpr uint16_t kSockTimeoutS = 3;     // czekanie na CONNACK
constexpr uint32_t kConnTimeoutMs = 2000; // TCP connect (domyslne 3000 to za dlugo)
constexpr uint32_t kBackoffMinMs = 5000;
constexpr uint32_t kBackoffMaxMs = 5UL * 60UL * 1000UL;
constexpr uint32_t kDevPublishMs = 60000;

// Klient zyje na stercie tylko wtedy, gdy MQTT jest wlaczony.
WiFiClient* gSock = nullptr;
PubSubClient* gCli = nullptr;

char gDevId[16] = {};   // pg_a1b2c3 — node_id, client id, prefiks unique_id
char gAvail[36] = {};   // <prefix>/status (LWT)

uint32_t gNextTryAt = 0;
uint32_t gBackoffMs = kBackoffMinMs;
uint32_t gNextDevAt = 0;
volatile bool gReconfig = false;

// Ostatnie znane liczniki falownika. Gdy Modbus milczy (noc, restart falownika),
// publikujemy moce = 0, ale energie trzymamy — HA liczy total_increasing i zjazd
// do zera zinterpretowalby jako reset licznika i dorzucil fikcyjna produkcje.
struct PvCache {
  float todayKwh = 0.f;
  float totalKwh = 0.f;
  float tempC = 0.f;
  bool have = false;
} gPvCache;

// ---------------------------------------------------------------- pomocnicze --

void setErr(const char* msg) {
  strncpy(diag().mqttErr, msg, sizeof(diag().mqttErr) - 1);
  diag().mqttErr[sizeof(diag().mqttErr) - 1] = '\0';
}

void clearErr() {
  diag().mqttErr[0] = '\0';
}

const char* stateText(int st) {
  switch (st) {
    case -4: return "Broker nie odpowiada";
    case -3: return "Zerwane połączenie";
    case -2: return "Brak połączenia z brokerem";
    case -1: return "Rozłączony";
    case 1:  return "Broker odrzucił wersję MQTT";
    case 2:  return "Broker odrzucił client id";
    case 3:  return "Broker niedostępny";
    case 4:  return "Zły użytkownik lub hasło";
    case 5:  return "Brak autoryzacji";
    default: return "Błąd MQTT";
  }
}

// Doklejanie do bufora z wykrywaniem przepelnienia: zwraca nowa dlugosc, ktora
// moze przekroczyc cap — wtedy zawartosc jest obcieta i pakiet trzeba odrzucic.
int addf(char* b, int cap, int len, const char* fmt, ...) {
  if (len >= cap) {
    return len;
  }
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(b + len, static_cast<size_t>(cap - len), fmt, ap);
  va_end(ap);
  if (n < 0) {
    return cap;  // blad formatowania traktujemy jak przepelnienie
  }
  return len + n;
}

// -------------------------------------------------------- definicje encji -----
// Tablica jest const → laduje w .rodata (flash), nie zjada RAM-u.

struct Ent {
  const char* key;     // object_id + koncowka unique_id (ASCII)
  const char* name;    // nazwa w HA
  const char* group;   // pv | wx | dev  → temat <prefix>/<group>/state
  const char* field;   // klucz w JSON-ie stanu
  const char* devCla;  // device_class albo nullptr
  const char* unit;    // unit_of_measurement albo nullptr
  const char* staCla;  // state_class albo nullptr
  const char* icon;    // ikona albo nullptr
  bool diagnostic;     // entity_category: diagnostic
};

const Ent kEnts[] = {
    // --- fotowoltaika ---
    {"pv_ac", "Moc AC", "pv", "ac", "power", "W", "measurement", nullptr, false},
    {"pv_dc", "Moc DC", "pv", "dc", "power", "W", "measurement", nullptr, false},
    {"pv_today", "Produkcja dzisiaj", "pv", "today", "energy", "kWh", "total_increasing",
     nullptr, false},
    {"pv_total", "Produkcja całkowita", "pv", "total", "energy", "kWh", "total_increasing",
     nullptr, false},
    // znak: + oddawanie do sieci, - pobor
    {"pv_grid", "Bilans sieci", "pv", "grid", "power", "W", "measurement", nullptr, false},
    {"pv_house", "Pobór domu", "pv", "house", "power", "W", "measurement", nullptr, false},
    {"pv_temp", "Temperatura falownika", "pv", "temp", "temperature", "°C", "measurement",
     nullptr, false},
    {"pv_status", "Status falownika", "pv", "status", nullptr, nullptr, nullptr,
     "mdi:solar-power", false},

    // --- pogoda ---
    {"wx_temp", "Temperatura", "wx", "temp", "temperature", "°C", "measurement", nullptr,
     false},
    {"wx_feels", "Temperatura odczuwalna", "wx", "feels", "temperature", "°C", "measurement",
     nullptr, false},
    {"wx_hum", "Wilgotność", "wx", "hum", "humidity", "%", "measurement", nullptr, false},
    {"wx_pres", "Ciśnienie", "wx", "pres", "atmospheric_pressure", "hPa", "measurement",
     nullptr, false},
    {"wx_wind", "Wiatr", "wx", "wind", "wind_speed", "km/h", "measurement", nullptr, false},
    {"wx_cloud", "Zachmurzenie", "wx", "cloud", nullptr, "%", "measurement", "mdi:cloud",
     false},
    {"wx_uv", "Indeks UV", "wx", "uv", nullptr, nullptr, "measurement", "mdi:weather-sunny",
     false},
    {"wx_rain", "Opad", "wx", "rain", "precipitation_intensity", "mm/h", "measurement",
     nullptr, false},
    {"wx_desc", "Pogoda", "wx", "desc", nullptr, nullptr, nullptr,
     "mdi:weather-partly-cloudy", false},

    // --- samo urzadzenie (kategoria diagnostyczna) ---
    {"dev_temp", "Temperatura ESP32", "dev", "cpu", "temperature", "°C", "measurement",
     nullptr, true},
    {"dev_heap", "Wolna pamięć", "dev", "heap", "data_size", "B", "measurement", nullptr,
     true},
    {"dev_up", "Czas pracy", "dev", "up", "duration", "s", "measurement", nullptr, true},
    {"dev_rssi", "Sygnał Wi-Fi", "dev", "rssi", "signal_strength", "dBm", "measurement",
     nullptr, true},
    {"dev_fw", "Wersja firmware", "dev", "fw", nullptr, nullptr, nullptr, "mdi:chip", true},
};

constexpr int kEntCount = static_cast<int>(sizeof(kEnts) / sizeof(kEnts[0]));

// ------------------------------------------------------------------ discovery --

// Jedna encja = jeden retained config na homeassistant/sensor/<devId>/<key>/config.
// Klucze skrocone (stat_t, val_tpl, dev_cla...) — HA je rozumie, a payload schodzi
// z ~700 B do ~420 B i miesci sie w naszym malym buforze.
bool publishConfig(const Ent& e) {
  const Settings& s = settings();

  char topic[72];
  int tn = snprintf(topic, sizeof(topic), "homeassistant/sensor/%s/%s/config", gDevId, e.key);
  if (tn < 0 || tn >= static_cast<int>(sizeof(topic))) {
    return false;
  }

  char p[480];
  const int cap = static_cast<int>(sizeof(p));
  int n = addf(p, cap, 0,
               "{\"~\":\"%s\",\"name\":\"%s\",\"uniq_id\":\"%s_%s\","
               "\"stat_t\":\"~/%s/state\",\"avty_t\":\"~/status\","
               "\"val_tpl\":\"{{value_json.%s}}\"",
               s.mqttPrefix, e.name, gDevId, e.key, e.group, e.field);
  if (e.devCla != nullptr) n = addf(p, cap, n, ",\"dev_cla\":\"%s\"", e.devCla);
  if (e.unit != nullptr) n = addf(p, cap, n, ",\"unit_of_meas\":\"%s\"", e.unit);
  if (e.staCla != nullptr) n = addf(p, cap, n, ",\"stat_cla\":\"%s\"", e.staCla);
  if (e.icon != nullptr) n = addf(p, cap, n, ",\"ic\":\"%s\"", e.icon);
  if (e.diagnostic) n = addf(p, cap, n, ",\"ent_cat\":\"diagnostic\"");

  // Wspolny blok device — dzieki niemu HA sklei wszystkie 22 encje w JEDNO urzadzenie.
  n = addf(p, cap, n,
           ",\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\",\"mf\":\"premiumads-pl\","
           "\"mdl\":\"ESP32-S3 pogoda+PV\",\"sw\":\"%d\"}}",
           gDevId, s.mqttPrefix, FW_VERSION);

  if (n >= cap) {
    LOG("MQTT: config %s za dlugi (%d B) — pomijam\n", e.key, n);
    return false;
  }
  // 5 B naglowka + 2 B dlugosci tematu — musi sie zmiescic w buforze PubSubClient.
  if (tn + n + 7 > static_cast<int>(kBufSize)) {
    LOG("MQTT: pakiet %s > bufora (%d B) — pomijam\n", e.key, tn + n + 7);
    return false;
  }
  return gCli->publish(topic, reinterpret_cast<const uint8_t*>(p),
                       static_cast<unsigned int>(n), true);
}

// Czujniki BLE nie moga siedziec w statycznej tablicy: ich nazwy pochodza z NVS
// (uzytkownik wpisuje "Łazienka", "Schody"), a liczba zalezy od tego, ile ich
// skonfigurowal. Budujemy wiec Ent na stosie — publishConfig i tak uzywa wskaznikow
// od razu, jeszcze przed powrotem.
// `total` (wyjscie): ile encji BLE PROBOWALISMY wystawic. Bez tego wywolujacy zna sam
// licznik udanych i nie ma go do czego odniesc — a "ile ich w ogole mialo byc" zalezy
// od NVS i nie da sie tego policzyc z zewnatrz.
int sendBleDiscovery(int& total) {
  int ok = 0;
  total = 0;
  // BLE_USABLE, nie "4". Zaszyta czworka byla TRZECIA kopia tej samej petli
  // (ekran i .ino juz poprawione) i jedyna, ktora zostala: czujnik nr 5 pojawilby
  // sie na wyswietlaczu, ale NIGDY nie dostalby encji w Home Assistancie — bez
  // zadnego komunikatu. Objaw "na ekranie jest, w HA go nie ma" jest gorszy niz
  // brak wszedzie, bo nie wskazuje przyczyny.
  for (int i = 0; i < Settings::BLE_USABLE; ++i) {
    const Settings::BleCfg& c = settings().ble[i];
    if (c.mac[0] == '\0') continue;

    const char* room = c.name[0] ? c.name : c.mac;

    struct Def {
      const char* suffix;  // klucz + pole
      const char* label;
      const char* devCla;
      const char* unit;
      bool diag;
    };
    const Def defs[4] = {
        {"t", "temperatura", "temperature", "°C", false},
        {"h", "wilgotność", "humidity", "%", false},
        {"b", "bateria", "battery", "%", true},
        {"r", "sygnał", "signal_strength", "dBm", true},
    };

    for (const Def& d : defs) {
      ++total;
      char key[16], field[8], name[40];
      snprintf(key, sizeof(key), "ble%d_%s", i, d.suffix);
      snprintf(field, sizeof(field), "s%d%s", i, d.suffix);
      snprintf(name, sizeof(name), "%s — %s", room, d.label);

      const Ent e{key, name, "ble", field, d.devCla, d.unit, "measurement", nullptr, d.diag};
      if (publishConfig(e)) ++ok;
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
  return ok;
}

void sendDiscovery() {
  int bleTotal = 0;
  const int bleOk = sendBleDiscovery(bleTotal);

  int entOk = 0;
  for (int i = 0; i < kEntCount; ++i) {
    if (publishConfig(kEnts[i])) {
      ++entOk;
    }
    // 22 pakiety po ~490 B pod rzad potrafia zapchac okno TCP — oddajemy procesor,
    // zeby webTask (nizszy priorytet, ten sam rdzen) nie zglodnial.
    vTaskDelay(pdMS_TO_TICKS(5));
  }

  // Dwa liczniki, nie jeden. Do v107 szlo tu "discovery %d/%d" z sumy BLE+stale jako
  // licznikiem i samego kEntCount jako mianownikiem — czyli "38/22", bo mianownik
  // opisywal tylko druga polowe licznika. Czytalo sie to jak "38 z 22" i zamiast
  // powiedziec "wszystko poszlo", kazalo szukac nieistniejacej awarii.
  LOG("MQTT: discovery BLE %d/%d + stale %d/%d encji, heap %lu\n", bleOk, bleTotal, entOk,
      kEntCount, static_cast<unsigned long>(ESP.getFreeHeap()));
}

// ------------------------------------------------------------------ transport --

void teardown(bool sayGoodbye) {
  if (gCli != nullptr) {
    if (sayGoodbye && gCli->connected()) {
      gCli->publish(gAvail, "offline", true);
      gCli->disconnect();
    }
    delete gCli;
    gCli = nullptr;
  }
  if (gSock != nullptr) {
    gSock->stop();
    delete gSock;
    gSock = nullptr;
  }
}

bool ensureClient() {
  if (gCli != nullptr) {
    return true;
  }
  gSock = new (std::nothrow) WiFiClient();
  if (gSock == nullptr) {
    return false;
  }
  gCli = new (std::nothrow) PubSubClient(*gSock);
  if (gCli == nullptr) {
    delete gSock;
    gSock = nullptr;
    return false;
  }
  if (!gCli->setBufferSize(kBufSize)) {  // malloc 512 B — moze nie wyjsc przy fragmentacji
    teardown(false);
    return false;
  }
  gCli->setKeepAlive(kKeepAliveS);
  gCli->setSocketTimeout(kSockTimeoutS);
  return true;
}

void makeIds() {
  if (gDevId[0] == '\0') {
    uint8_t mac[6] = {};
    WiFi.macAddress(mac);
    snprintf(gDevId, sizeof(gDevId), "pg_%02x%02x%02x", mac[3], mac[4], mac[5]);
  }
  snprintf(gAvail, sizeof(gAvail), "%s/status", settings().mqttPrefix);
}

void backoff() {
  gNextTryAt = millis() + gBackoffMs;
  gBackoffMs = (gBackoffMs >= kBackoffMaxMs / 2) ? kBackoffMaxMs : gBackoffMs * 2;
}

bool tryConnect() {
  const Settings& s = settings();
  makeIds();

  if (!ensureClient()) {
    setErr("Za mało RAM na klienta MQTT");
    backoff();
    return false;
  }

  gSock->setConnectionTimeout(kConnTimeoutMs);
  gCli->setServer(s.mqttHost, s.mqttPort);

  const char* user = (s.mqttUser[0] != '\0') ? s.mqttUser : nullptr;
  const char* pass = (s.mqttPass[0] != '\0') ? s.mqttPass : nullptr;

  // LWT: broker sam ogłosi "offline", gdy urzadzenie zniknie bez pozegnania.
  const bool ok = gCli->connect(gDevId, user, pass, gAvail, 0, true, "offline", true);
  if (!ok) {
    const int st = gCli->state();
    setErr(stateText(st));
    LOG("MQTT: brak polaczenia z %s:%u (stan %d)\n", s.mqttHost, s.mqttPort, st);
    gSock->stop();  // nie zostawiaj wiszacego gniazda
    backoff();
    return false;
  }

  gBackoffMs = kBackoffMinMs;
  ++diag().mqttConnects;
  clearErr();
  LOG("MQTT: polaczono z %s:%u jako %s, heap %lu\n", s.mqttHost, s.mqttPort, gDevId,
      static_cast<unsigned long>(ESP.getFreeHeap()));

  gCli->publish(gAvail, "online", true);
  sendDiscovery();
  gNextDevAt = 0;  // od razu wyslij telemetrie urzadzenia
  return true;
}

// -------------------------------------------------------------------- publish --

bool pubState(const char* group, const char* json, int len) {
  if (gCli == nullptr || !gCli->connected()) {
    return false;
  }
  char topic[48];
  const int tn = snprintf(topic, sizeof(topic), "%s/%s/state", settings().mqttPrefix, group);
  if (tn < 0 || tn >= static_cast<int>(sizeof(topic))) {
    return false;
  }
  // retain: po restarcie HA od razu widzi ostatnie wartosci, nie czeka 15 minut
  const bool ok = gCli->publish(topic, reinterpret_cast<const uint8_t*>(json),
                                static_cast<unsigned int>(len), true);
  if (ok) {
    diag().mqttOkAt = millis();
    ++diag().mqttPublished;
    clearErr();
  } else {
    setErr("Broker odrzucił publikację");
  }
  return ok;
}

void publishDevice() {
  char p[160];
  const int n = snprintf(p, sizeof(p),
                         "{\"cpu\":%.1f,\"heap\":%lu,\"up\":%lu,\"rssi\":%d,\"fw\":%d}",
                         temperatureRead(),
                         static_cast<unsigned long>(ESP.getFreeHeap()),
                         static_cast<unsigned long>(millis() / 1000), WiFi.RSSI(),
                         FW_VERSION);
  if (n > 0 && n < static_cast<int>(sizeof(p))) {
    pubState("dev", p, n);
  }
}

// Opis pogody dla HA — pelniejszy niz trzyliterowa etykieta z ekranu.
const char* wxDescription(int code) {
  switch (code) {
    case 0: return "Bezchmurnie";
    case 1: return "Głównie słonecznie";
    case 2: return "Częściowe zachmurzenie";
    case 3: return "Pochmurno";
    case 45:
    case 48: return "Mgła";
    case 51:
    case 53:
    case 55: return "Mżawka";
    case 56:
    case 57: return "Marznąca mżawka";
    case 61: return "Słaby deszcz";
    case 63: return "Deszcz";
    case 65: return "Ulewa";
    case 66:
    case 67: return "Marznący deszcz";
    case 71: return "Słaby śnieg";
    case 73: return "Śnieg";
    case 75: return "Intensywny śnieg";
    case 77: return "Śnieg ziarnisty";
    case 80:
    case 81: return "Przelotny deszcz";
    case 82: return "Gwałtowna ulewa";
    case 85:
    case 86: return "Przelotny śnieg";
    case 95: return "Burza";
    case 96:
    case 99: return "Burza z gradem";
    default: return "Pogoda";
  }
}

}  // namespace

// ===================================================================== API =====

void configChanged() {
  gReconfig = true;
}

void loop() {
  const Settings& s = settings();

  // MQTT wylaczony: oddaj stertę i nie udawaj bledu na ekranie statystyk.
  if (!s.hasMqtt()) {
    if (gCli != nullptr) {
      teardown(true);
      LOG("MQTT: wyłączony — klient zwolniony\n");
    }
    gReconfig = false;
    clearErr();
    diag().mqttOkAt = 0;
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (gReconfig) {
    gReconfig = false;
    teardown(true);
    gBackoffMs = kBackoffMinMs;
    gNextTryAt = 0;
    diag().mqttOkAt = 0;
  }

  if (gCli != nullptr && gCli->connected()) {
    gCli->loop();  // keepalive; nic nie subskrybujemy, wiec nie ma callbackow
    if (static_cast<int32_t>(millis() - gNextDevAt) >= 0) {
      publishDevice();
      gNextDevAt = millis() + kDevPublishMs;
    }
    return;
  }

  if (static_cast<int32_t>(millis() - gNextTryAt) < 0) {
    return;  // backoff — broker moze sobie lezec, urzadzenie dziala dalej
  }
  tryConnect();
}

// Czujniki BLE — jeden JSON na wszystkie, klucze s0t/s0h/s0b/s0r ... s3r.
// Wysylamy tylko te, ktore maja swiezy odczyt; czujnik, ktory zamilkl, po prostu
// znika z payloadu, a HA pokaze ostatnia znana wartosc (retained).
void publishBle() {
  if (!settings().hasMqtt() || gCli == nullptr || !gCli->connected()) {
    return;
  }

  // PULAPKA, ktora sama nie wybuchla tylko dzieki limitowi 4 — i wybuchlaby przy
  // naiwnym podniesieniu go na 6:
  //   snprintf() zwraca dlugosc, jaka BY zapisal, a nie ile zapisal. Przy obcieciu
  //   `n` przekracza sizeof(p). Wtedy `sizeof(p) - n` (size_t minus int!) przekreca
  //   sie na ogromna liczbe, a `p + n` wskazuje POZA bufor. Domykajaca klamra za
  //   petla pisala by wprost w stos netTask.
  // Dlatego: rezerwujemy miejsce PRZED rekordem, zamiast sprawdzac szkode po fakcie.
  // Najdluzszy rekord: ,"s5t":-12.3,"s5h":100.0,"s5b":100,"s5r":-100 = 46 B.
  constexpr int kRec = 64;    // zapas na jeden komplet pol czujnika
  char p[384];
  const int cap = static_cast<int>(sizeof(p));
  int n = snprintf(p, sizeof(p), "{");
  bool any = false;

  for (int i = 0; i < ble::count(); ++i) {
    if (n > cap - kRec) break;   // nie zaczynamy rekordu, na ktory nie ma miejsca

    const ble::Sensor s = ble::get(i);
    if (!s.valid) continue;

    const Settings::BleCfg* cfg = settings().bleFind(s.mac);
    if (cfg == nullptr) continue;  // nieskonfigurowany — nie ma dla niego encji w HA

    // slot musi sie zgadzac z tym z discovery (indeks w settings, nie w ble::)
    int slot = -1;
    for (int k = 0; k < Settings::BLE_USABLE; ++k) {
      if (&settings().ble[k] == cfg) slot = k;
    }
    if (slot < 0) continue;

    if (any) n += snprintf(p + n, cap - n, ",");
    if (s.hasTemp) n += snprintf(p + n, cap - n, "\"s%dt\":%.1f,", slot, s.tempC);
    if (s.hasHum) n += snprintf(p + n, cap - n, "\"s%dh\":%.1f,", slot, s.humidity);
    n += snprintf(p + n, cap - n, "\"s%db\":%d,\"s%dr\":%d", slot, s.batteryPct,
                  slot, s.rssi);
    any = true;
  }

  n += snprintf(p + n, cap - n, "}");
  if (any && n > 0 && n < static_cast<int>(sizeof(p))) {
    pubState("ble", p, n);
  }
}

void publishPv(const PvModel& pv, bool ok) {
  if (!settings().hasMqtt() || gCli == nullptr || !gCli->connected()) {
    return;
  }

  const PvSnapshot& d = pv.data;
  if (ok) {
    gPvCache.todayKwh = d.energyTodayKwh;
    gPvCache.totalKwh = d.energyTotalKwh;
    gPvCache.tempC = d.inverterTempC;
    gPvCache.have = true;
  } else if (!gPvCache.have) {
    return;  // nigdy nie odczytalismy falownika — nie ma co wysylac
  }

  const char* status = ok ? pvStatusLabel(d.statusCode) : "Offline";

  char p[224];
  const int n = snprintf(
      p, sizeof(p),
      "{\"ac\":%ld,\"dc\":%ld,\"grid\":%ld,\"house\":%ld,"
      "\"today\":%.2f,\"total\":%.2f,\"temp\":%.1f,\"status\":\"%s\"}",
      ok ? static_cast<long>(d.powerAcW) : 0L, ok ? static_cast<long>(d.powerDcW) : 0L,
      ok ? static_cast<long>(d.gridPowerW) : 0L, ok ? static_cast<long>(d.houseLoadW) : 0L,
      gPvCache.todayKwh, gPvCache.totalKwh, gPvCache.tempC, status);
  if (n > 0 && n < static_cast<int>(sizeof(p))) {
    pubState("pv", p, n);
  }
}

void publishWeather(const WeatherModel& w) {
  if (!settings().hasMqtt() || gCli == nullptr || !gCli->connected() || !w.ready) {
    return;
  }
  const WeatherSnapshot& c = w.current;

  char p[256];
  const int n = snprintf(
      p, sizeof(p),
      "{\"temp\":%.1f,\"feels\":%.1f,\"hum\":%d,\"pres\":%.1f,\"wind\":%.1f,"
      "\"cloud\":%d,\"uv\":%.1f,\"rain\":%.1f,\"desc\":\"%s\"}",
      c.tempC, c.feelsC, c.humidity, c.pressureHpa, c.windKmh, c.cloudCover, c.uvIndex,
      c.precipMm, wxDescription(c.weatherCode));
  if (n > 0 && n < static_cast<int>(sizeof(p))) {
    pubState("wx", p, n);
  }
}

}  // namespace mqttha
