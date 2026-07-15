#include "BleGateway.h"

#include "BleSensors.h"
#include "Log.h"
#include "Settings.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClient.h>

#include <cstring>

namespace blegw {
namespace {

// Bylo 6000 ms przy jednej bramce. Przy trzech martwych to do 18 s w netTask, czyli
// caly okres odpytywania (20 s): jedna bramka wyjeta z gniazdka potrafilaby zaglodzic
// pogode, radar i nasluch BLE. Shelly w LAN odpowiada w kilkadziesiat ms - 3 s to i
// tak trzydziestokrotny zapas, a najgorszy przypadek spada z 18 s do 9 s. I tylko RAZ:
// potem odzywa sie backoff nizej.
constexpr uint32_t kTimeoutMs = 3000;

// Backoff po bledzie. Cel: martwa bramka ma kosztowac 3 s na dwie minuty, a nie 3 s
// na 20 s. Zdrowa nie czeka nigdy - dla niej decyduje wylacznie zegar w netTask.
constexpr uint32_t kRetry1Ms = 20000;
constexpr uint32_t kRetry2Ms = 60000;
constexpr uint32_t kRetryNMs = 120000;

// Po tylu ms bez ramki uznajemy, ze ta bramka juz tego czujnika nie slyszy, i
// przestaje sie liczyc w wyborze opiekuna. 90 s to ~4 odpytania: czujnik nadaje co
// kilkanascie sekund, wiec zywa bramka odswiezy sie kilka razy w tym oknie.
constexpr uint32_t kHeardStaleMs = 90000;

// Histereza zmiany opiekuna. Lazienka: -72 z wlasnego radia, -70 z bramki - dwa
// decybele, czyli mniej niz normalne wahanie RSSI. Bez histerezy dwa porownywalne
// odbiorniki przerzucalyby sie opiekunem co ramke i rssiGw skakaloby bez sensu.
// Prawdziwe roznice sa duzo wieksze (Schody: -90 vs -56), wiec 6 dB niczego nie blokuje.
constexpr int kHystDb = 6;

struct Gw {
  uint32_t okAt = 0;
  uint32_t nextAt = 0;      // ma sens tylko przy backoff == true
  int16_t count = 0;
  int16_t loggedN = -1;     // ostatnio zalogowana liczba ramek - logujemy zmiany
  uint8_t fails = 0;
  bool backoff = false;
  char err[24] = {};
};
Gw gGw[SLOTS];

// Kto co slyszy. Tablica jest tu, a nie w Sensor, z dwoch powodow: Sensor ma jedno
// pole rssiGw (nie wie, KTORA bramka), a rozszerzanie go kosztowaloby pamiec w
// kazdym slocie czujnika i zmiane pliku, ktorego ta zmiana nie dotyczy.
struct Heard {
  char mac[18] = {};
  int8_t rssi[SLOTS] = {};
  uint8_t owner = 0xFF;      // indeks bramki-opiekuna, 0xFF = jeszcze nikt
  uint32_t at[SLOTS] = {};
};
// Tyle, ile slotow ma ble:: - czujnika, ktorego tam nie zmiescimy, nie ma po co
// arbitrazowac. Obce nadajniki z bloku tez tu wchodza, dokladnie jak w ble::.
Heard gHeard[ble::MAX_SENSORS];
int gHeardN = 0;

// netTask pisze (poll), webTask czyta (panel). Mutex obejmuje WYLACZNIE grzebanie w
// tablicy: nigdy GET-a, nigdy feedRaw() - ten bierze wlasny mutex w ble::, a
// zagniezdzanie dwoch blokad to gotowy przepis na zakleszczenie.
SemaphoreHandle_t gMx = nullptr;

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Brak pola "r" w JSON-ie albo bzdurna wartosc. Zero NIE moze przejsc dalej: 0 dBm
// to najsilniejszy sygnal swiata i wygralby kazdy wybor opiekuna, oddajac czujnik
// bramce, ktora go w ogole nie slyszy. Traktujemy jak sygnal na granicy.
int8_t saneRssi(int rssi) {
  if (rssi >= 0 || rssi < -120) return -99;
  return static_cast<int8_t>(rssi);
}

// --- ponizsze wolane WYLACZNIE pod gMx ---

int heardIndex(const char* mac) {
  for (int i = 0; i < gHeardN; ++i) {
    if (strcasecmp(gHeard[i].mac, mac) == 0) return i;
  }
  return -1;
}

Heard* heardFor(const char* mac) {
  const int i = heardIndex(mac);
  if (i >= 0) return &gHeard[i];
  if (gHeardN >= ble::MAX_SENSORS) return nullptr;
  Heard* h = &gHeard[gHeardN++];
  *h = Heard{};  // slot moze byc po poprzednim najemcy (retryNow zeruje licznik)
  snprintf(h->mac, sizeof(h->mac), "%s", mac);
  return h;
}

// Opiekun = najsilniejszy sygnal sposrod ZYWYCH bramek. Urzedujacy zostaje, dopoki
// zyje i dopoki wyzwanie nie jest o kHystDb lepsze. Swiezosc jest warunkiem, nie
// kryterium: bramka, ktora zamilkla, przegrywa z KAZDA slyszalna, chocby slabsza -
// lepszy slaby sygnal teraz niz swietny sprzed pol godziny.
int pickOwner(const Heard& h, uint32_t now) {
  int best = -1;
  for (int i = 0; i < SLOTS; ++i) {
    if (h.at[i] == 0 || now - h.at[i] > kHeardStaleMs) continue;
    if (best < 0 || h.rssi[i] > h.rssi[best]) best = i;
  }
  if (best < 0) return -1;

  const int cur = (h.owner < SLOTS) ? static_cast<int>(h.owner) : -1;
  const bool curAlive = cur >= 0 && h.at[cur] != 0 && now - h.at[cur] <= kHeardStaleMs;
  if (curAlive && h.rssi[best] <= h.rssi[cur] + kHystDb) return cur;
  return best;
}

// Bramka zamilkla - jej pomiary sa juz nieaktualne. Kasujemy je OD RAZU, zamiast
// czekac kHeardStaleMs: czujniki, ktorymi sie opiekowala, maja dostac nowego
// opiekuna przy pierwszej ramce z innej bramki, a nie za pol minuty. To jest to
// miejsce, w ktorym padniecie jednej bramki nie blokuje pozostalych.
void forgetGw(int idx) {
  if (gMx == nullptr) return;
  xSemaphoreTake(gMx, portMAX_DELAY);
  for (int i = 0; i < gHeardN; ++i) {
    gHeard[i].rssi[idx] = 0;
    gHeard[i].at[idx] = 0;
    if (gHeard[i].owner == idx) gHeard[i].owner = 0xFF;
  }
  xSemaphoreGive(gMx);
}

// --- koniec sekcji pod gMx ---

void fail(int idx, const char* msg) {
  Gw& g = gGw[idx];
  snprintf(g.err, sizeof(g.err), "%s", msg);
  if (g.fails < 250) ++g.fails;
  g.backoff = true;
  g.nextAt = millis() + (g.fails >= 3 ? kRetryNMs : (g.fails == 2 ? kRetry2Ms : kRetry1Ms));
  g.count = 0;
  g.loggedN = -1;
  forgetGw(idx);
}

// Zapisuje pomiar tej bramki i mowi, czy to ONA jest teraz opiekunem czujnika.
bool claim(int idx, const char* mac, int8_t rssi) {
  if (gMx == nullptr) return true;
  const uint32_t now = millis();

  int before = -1, after = -1;
  xSemaphoreTake(gMx, portMAX_DELAY);
  Heard* h = heardFor(mac);
  if (h != nullptr) {
    h->rssi[idx] = rssi;
    h->at[idx] = (now != 0) ? now : 1;  // 0 znaczy "nigdy", wiec go nie zapisujemy
    before = (h->owner < SLOTS) ? static_cast<int>(h->owner) : -1;
    after = pickOwner(*h, now);
    h->owner = (after < 0) ? 0xFF : static_cast<uint8_t>(after);
  }
  xSemaphoreGive(gMx);

  // Tablica pelna (same obce nadajniki) - nie arbitrazujemy, oddajemy do ble::.
  // Tam i tak nie bedzie slotu, ale to decyzja ble::, nie nasza.
  if (h == nullptr) return true;

  if (after != before && after >= 0) {
    LOG("Bramka BLE: %s -> opiekun bramka %d (%d dBm)", mac, after + 1,
        static_cast<int>(rssi));
  }
  return after == idx;
}

bool pollOne(int idx, const char* hostAddr) {
  WiFiClient client;
  client.setTimeout(kTimeoutMs / 1000);
  HTTPClient http;
  http.setConnectTimeout(kTimeoutMs);
  http.setTimeout(kTimeoutMs);
  http.setReuse(false);

  char url[80];
  snprintf(url, sizeof(url), "http://%s/script/1/ble", hostAddr);
  if (!http.begin(client, url)) {
    // "nie odpowiada", nie "brak polaczenia" — na ekranie statystyk przy nazwie
    // "Bramka" zostaje 76 px, a tamten napis mial 77. Przegrywal o jeden piksel.
    fail(idx, "nie odpowiada");
    LOG("Bramka BLE %d: nie moge otworzyc %s", idx + 1, url);
    return false;
  }
  const int code = http.GET();
  if (code != 200) {
    char e[24];
    snprintf(e, sizeof(e), "HTTP %d", code);
    fail(idx, e);
    LOG("Bramka BLE %d: %s -> HTTP %d", idx + 1, url, code);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) {
    fail(idx, "zly JSON");
    return false;
  }

  int n = 0;
  for (JsonPairConst kv : doc.as<JsonObjectConst>()) {
    const char* mac = kv.key().c_str();
    const int rssi = kv.value()["r"] | 0;
    const char* hex = kv.value()["d"] | "";
    const char* uuid = kv.value()["u"] | "fe95";   // starsza bramka slala tylko MiBeacon
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

    // Liczymy PRZED arbitrazem: to miara zycia bramki, nie tego, ile z niej weszlo.
    // Inaczej sprawna bramka, ktorej wszystkie czujniki sluchaja lepiej sasiadki,
    // raportowalaby "0 ramek" i wygladala na zepsuta.
    ++n;

    const int8_t r = saneRssi(rssi);
    if (!claim(idx, mac, r)) continue;  // ta sama ramka przyjdzie od opiekuna

    // Dwa formaty, dwie drogi: Xiaomi szyfruje (bindkey z NVS), Qingping nadaje
    // otwartym tekstem. Bramka nie rozumie zadnego z nich — tylko przekazuje.
    if (strcmp(uuid, "fdcd") == 0) {
      ble::feedRawQingping(mac, raw, hl / 2, r);
    } else {
      ble::feedRaw(mac, raw, hl / 2, r);
    }
  }

  Gw& g = gGw[idx];
  g.count = static_cast<int16_t>(n);
  g.okAt = millis();
  g.err[0] = '\0';
  g.fails = 0;
  g.backoff = false;
  if (n != g.loggedN) {   // logujemy tylko zmiany — inaczej zalejemy bufor
    LOG("Bramka BLE %d (%s): %d ramek przyjetych", idx + 1, hostAddr, n);
    g.loggedN = static_cast<int16_t>(n);
  }
  return true;
}

}  // namespace

void poll() {
  if (gMx == nullptr) gMx = xSemaphoreCreateMutex();
  const uint32_t now = millis();

  for (int i = 0; i < SLOTS; ++i) {
    const char* h = settings().bleGwAt(i);
    if (h[0] == '\0') {
      // Slot skasowany w panelu - nie zostawiamy po nim trupa w diagnostyce ani
      // pomiarow, ktore nigdy sie juz nie odswieza.
      if (gGw[i].okAt != 0 || gGw[i].err[0] != '\0' || gGw[i].fails != 0) {
        gGw[i] = Gw{};
        forgetGw(i);
      }
      continue;
    }
    // Zdrowa bramka nie ma backoffu i nie patrzy na zegar - o tempo dba netTask.
    if (gGw[i].backoff && static_cast<int32_t>(now - gGw[i].nextAt) < 0) continue;
    pollOne(i, h);
  }
}

const char* host(int i) { return settings().bleGwAt(i); }

uint32_t lastOkAt(int i) { return (i >= 0 && i < SLOTS) ? gGw[i].okAt : 0; }

int lastCount(int i) { return (i >= 0 && i < SLOTS) ? gGw[i].count : 0; }

const char* lastError(int i) {
  if (i < 0 || i >= SLOTS) return "";
  if (settings().bleGwAt(i)[0] == '\0') return "wylaczona";
  return gGw[i].err;
}

int configured() { return settings().bleGwCount(); }

int online() {
  int n = 0;
  for (int i = 0; i < SLOTS; ++i) {
    if (settings().bleGwAt(i)[0] == '\0') continue;
    if (gGw[i].okAt != 0 && gGw[i].err[0] == '\0') ++n;
  }
  return n;
}

uint32_t lastOkAt() {
  uint32_t t = 0;
  for (int i = 0; i < SLOTS; ++i) {
    if (settings().bleGwAt(i)[0] == '\0') continue;
    if (gGw[i].okAt > t) t = gGw[i].okAt;
  }
  return t;
}

int lastCount() {
  int n = 0;
  for (int i = 0; i < SLOTS; ++i) {
    if (settings().bleGwAt(i)[0] != '\0') n += gGw[i].count;
  }
  return n;
}

// Ekran statystyk ma na bramki JEDEN wiersz i ~76 px na status, wiec nie zmiesci
// trzech stanow. Zwracamy blad pierwszej padnietej - reszte (ile z ilu zyje) da
// online()/configured(). Przy jednej bramce zachowanie jest identyczne jak przed
// lista: ten sam napis, ta sama szerokosc.
const char* lastError() {
  for (int i = 0; i < SLOTS; ++i) {
    if (settings().bleGwAt(i)[0] == '\0') continue;
    if (gGw[i].err[0] != '\0') return gGw[i].err;
  }
  return "";
}

int ownerOf(const char* mac) {
  if (gMx == nullptr || mac == nullptr) return -1;
  int r = -1;
  xSemaphoreTake(gMx, portMAX_DELAY);
  const int i = heardIndex(mac);
  if (i >= 0 && gHeard[i].owner < SLOTS) r = static_cast<int>(gHeard[i].owner);
  xSemaphoreGive(gMx);
  return r;
}

int rssiOf(const char* mac, int idx) {
  if (gMx == nullptr || mac == nullptr || idx < 0 || idx >= SLOTS) return 0;
  int r = 0;
  xSemaphoreTake(gMx, portMAX_DELAY);
  const int i = heardIndex(mac);
  if (i >= 0) r = gHeard[i].rssi[idx];
  xSemaphoreGive(gMx);
  return r;
}

uint32_t heardAt(const char* mac, int idx) {
  if (gMx == nullptr || mac == nullptr || idx < 0 || idx >= SLOTS) return 0;
  uint32_t t = 0;
  xSemaphoreTake(gMx, portMAX_DELAY);
  const int i = heardIndex(mac);
  if (i >= 0) t = gHeard[i].at[idx];
  xSemaphoreGive(gMx);
  return t;
}

void retryNow() {
  for (int i = 0; i < SLOTS; ++i) {
    gGw[i].backoff = false;
    gGw[i].fails = 0;
    gGw[i].loggedN = -1;
  }
  if (gMx == nullptr) return;
  xSemaphoreTake(gMx, portMAX_DELAY);
  gHeardN = 0;  // adresy mogly sie przesunac - stary pomiar moze byc juz z innego urzadzenia
  xSemaphoreGive(gMx);
}

}  // namespace blegw
