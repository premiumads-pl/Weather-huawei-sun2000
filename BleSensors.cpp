#include "BleSensors.h"

#include "Log.h"
#include "Settings.h"

#include <Arduino.h>
#include <BLEAdvertisedDevice.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <mbedtls/ccm.h>

#include <cstring>

namespace ble {
namespace {

Sensor gSensors[MAX_SENSORS];
int gCount = 0;
bool gReady = false;
volatile bool gScanning = false;
char gErr[48] = "nie uruchomiony";
SemaphoreHandle_t gMx = nullptr;

// Stos BLE bierze ~72 kB. Nie podnosimy go, jesli sterta jest juz napieta —
// lepiej odpuscic nasluch niz wywrocic TLS albo dekoder radaru.
constexpr uint32_t kMinHeapForBle = 100000;

// Xiaomi LYWSD03MMC ma adresy z puli A4:C1:38:xx:xx:xx. Firmware pvvx potrafi
// adres losowac, wiec NIE filtrujemy po adresie — filtrujemy po formacie ramki.

Sensor* slotFor(const char* mac) {
  for (int i = 0; i < gCount; ++i) {
    if (strcmp(gSensors[i].mac, mac) == 0) return &gSensors[i];
  }
  if (gCount >= MAX_SENSORS) return nullptr;
  Sensor* s = &gSensors[gCount++];
  snprintf(s->mac, sizeof(s->mac), "%s", mac);
  return s;
}

void hexDump(char* out, size_t n, const uint8_t* d, size_t len) {
  size_t p = 0;
  for (size_t i = 0; i < len && p + 3 < n; ++i) {
    p += snprintf(out + p, n - p, "%02X", d[i]);
  }
  out[p] = '\0';
}

// pvvx (15 B, little-endian) / ATC (13 B, big-endian) — obie na UUID 0x181A.
bool parse181A(const uint8_t* d, size_t len, const char* mac, int rssi) {
  Sensor tmp{};
  if (len == 15) {
    // MAC(6) T(2,LE,0.01C) H(2,LE,0.01%) Vbat(2,LE,mV) bat% cnt flags
    tmp.tempC = static_cast<int16_t>(d[6] | (d[7] << 8)) / 100.f;
    tmp.humidity = static_cast<uint16_t>(d[8] | (d[9] << 8)) / 100.f;
    tmp.batteryMv = static_cast<uint16_t>(d[10] | (d[11] << 8));
    tmp.batteryPct = d[12];
  } else if (len == 13) {
    // MAC(6) T(2,BE,0.1C) H(1,%) bat% Vbat(2,BE,mV) cnt
    tmp.tempC = static_cast<int16_t>((d[6] << 8) | d[7]) / 10.f;
    tmp.humidity = d[8];
    tmp.batteryPct = d[9];
    tmp.batteryMv = static_cast<uint16_t>((d[10] << 8) | d[11]);
  } else {
    return false;
  }

  if (tmp.tempC < -40.f || tmp.tempC > 85.f) return false;      // sanity
  if (tmp.humidity < 0.f || tmp.humidity > 100.f) return false;

  xSemaphoreTake(gMx, portMAX_DELAY);
  Sensor* s = slotFor(mac);
  if (s != nullptr) {
    const float t = tmp.tempC, h = tmp.humidity;
    const int bp = tmp.batteryPct, bm = tmp.batteryMv;
    snprintf(s->mac, sizeof(s->mac), "%s", mac);
    s->tempC = t;
    s->humidity = h;
    s->batteryPct = bp;
    s->batteryMv = bm;
    s->rssi = rssi;
    s->rssiOwn = rssi;
    s->ownAt = millis();
    s->seenAt = millis();
    s->hasTemp = true;
    s->hasHum = true;
    s->valid = true;
  }
  xSemaphoreGive(gMx);
  return true;
}

// --- MiBeacon v5 (fabryczny firmware Xiaomi), rozglaszanie ZASZYFROWANE ---
//
// Uklad ramki 0xFE95 z flaga szyfrowania:
//   [0..1] frame control (LE)   — bit 3 = zaszyfrowane, bit 4 = MAC w ramce
//   [2..3] product id (LE)      — 0x055B = LYWSD03MMC
//   [4]    licznik ramek
//   [5..10] MAC (odwrocony)
//   [11 .. len-8] szyfrogram
//   [len-7 .. len-5] rozszerzony licznik (3 B)
//   [len-4 .. len-1] MIC (4 B)
//
// Nonce = MAC(6) + productId(2) + licznik(1) + licznik rozszerzony(3) = 12 B
// AAD = 0x11, szyfr = AES-CCM, tag 4 B. Klucz (bindkey) pochodzi z chmury Xiaomi
// i lezy w NVS — nigdy w repo.
bool parseMiBeacon(const uint8_t* d, size_t len, const char* mac, int rssi, bool gw = false) {
  if (len < 5) return false;

  const uint16_t fc = static_cast<uint16_t>(d[0] | (d[1] << 8));
  const bool encrypted = (fc & 0x0008) != 0;
  const bool hasMac = (fc & 0x0010) != 0;
  if (!encrypted || !hasMac || len < 19) return false;

  const Settings::BleCfg* cfg = settings().bleFind(mac);
  if (cfg == nullptr || !cfg->hasKey) {
    // Widzimy czujnik, ale nie mamy klucza. Zapisujemy sam fakt istnienia —
    // panel pokaze go z adnotacja "brak klucza", zeby bylo co skonfigurowac.
    xSemaphoreTake(gMx, portMAX_DELAY);
    Sensor* s = slotFor(mac);
    if (s != nullptr) {
      s->rssi = rssi;
      if (gw) { s->rssiGw = rssi; s->gwAt = millis(); }
      else    { s->rssiOwn = rssi; s->ownAt = millis(); }
      s->seenAt = millis();
      s->encrypted = true;
      s->needsKey = true;
      s->viaGw = gw;
    }
    xSemaphoreGive(gMx);
    return true;
  }

  uint8_t nonce[12];
  memcpy(nonce, d + 5, 6);       // MAC z ramki
  nonce[6] = d[2];               // product id LO
  nonce[7] = d[3];               // product id HI
  nonce[8] = d[4];               // licznik ramek
  memcpy(nonce + 9, d + len - 7, 3);  // licznik rozszerzony

  const uint8_t* ct = d + 11;
  const size_t ctLen = len - 11 - 7;
  const uint8_t* mic = d + len - 4;
  if (ctLen == 0 || ctLen > 16) return false;

  uint8_t plain[16] = {};
  const uint8_t aad = 0x11;

  mbedtls_ccm_context ccm;
  mbedtls_ccm_init(&ccm);
  int rc = mbedtls_ccm_setkey(&ccm, MBEDTLS_CIPHER_ID_AES, cfg->key, 128);
  if (rc == 0) {
    rc = mbedtls_ccm_auth_decrypt(&ccm, ctLen, nonce, sizeof(nonce), &aad, 1, ct, plain,
                                  mic, 4);
  }
  mbedtls_ccm_free(&ccm);

  if (rc != 0) {
    LOG("BLE: %s zly klucz albo uszkodzona ramka (mbedtls %d)", mac, rc);
    xSemaphoreTake(gMx, portMAX_DELAY);
    Sensor* s = slotFor(mac);
    if (s != nullptr) {
      s->needsKey = true;
      s->encrypted = true;
      s->seenAt = millis();
    }
    xSemaphoreGive(gMx);
    return true;
  }

  // Odszyfrowany obiekt: [typ(2, LE)][dlugosc(1)][wartosc...]
  if (ctLen < 3) return true;
  const uint16_t type = static_cast<uint16_t>(plain[0] | (plain[1] << 8));
  const uint8_t vlen = plain[2];
  const uint8_t* v = plain + 3;
  if (3u + vlen > ctLen) return true;

  xSemaphoreTake(gMx, portMAX_DELAY);
  Sensor* s = slotFor(mac);
  if (s != nullptr) {
    s->encrypted = true;
    s->needsKey = false;
    s->rssi = rssi;
    if (gw) { s->rssiGw = rssi; s->gwAt = millis(); }
    else    { s->rssiOwn = rssi; s->ownAt = millis(); }
    s->seenAt = millis();
    s->viaGw = gw;

    switch (type) {
      case 0x1004:  // temperatura, 0,1 C
        if (vlen >= 2) {
          s->tempC = static_cast<int16_t>(v[0] | (v[1] << 8)) / 10.f;
          s->hasTemp = true;
          s->valid = true;
        }
        break;
      case 0x1006:  // wilgotnosc, 0,1 %
        if (vlen >= 2) {
          s->humidity = static_cast<uint16_t>(v[0] | (v[1] << 8)) / 10.f;
          s->hasHum = true;
          s->valid = true;
        }
        break;
      case 0x100A:  // bateria, %
        if (vlen >= 1) s->batteryPct = v[0];
        break;
      case 0x100D:  // temperatura + wilgotnosc razem
        if (vlen >= 4) {
          s->tempC = static_cast<int16_t>(v[0] | (v[1] << 8)) / 10.f;
          s->humidity = static_cast<uint16_t>(v[2] | (v[3] << 8)) / 10.f;
          s->hasTemp = true;
          s->hasHum = true;
          s->valid = true;
        }
        break;
      default:
        break;
    }
  }
  xSemaphoreGive(gMx);
  return true;
}

// --- Qingping (0xFDCD), np. cgllc.sensor_ht.qpg1 ---
// Zero szyfrowania — dane leca otwartym tekstem, wiec zaden bindkey nie jest
// potrzebny (mimo ze chmura Xiaomi go dla tego czujnika wydaje).
//
// Uklad, rozebrany na prawdziwej ramce 8816e97f11342d5801040e01b101020164:
//   [0]    flagi
//   [1]    id produktu
//   [2..7] MAC, odwrocony
//   [8..]  rekordy typ/dlugosc/wartosc:
//            0x01, len 4: temperatura (int16 LE, 0.1 C) + wilgotnosc (uint16 LE, 0.1 %)
//            0x02, len 1: bateria [%]
bool parseQingping(const uint8_t* d, size_t len, const char* mac, int rssi, bool gw) {
  if (len < 10) return false;

  float t = 0.f, h = 0.f;
  int batt = -1;
  bool hasT = false, hasH = false;

  size_t i = 8;
  while (i + 2 <= len) {
    const uint8_t type = d[i];
    const uint8_t ln = d[i + 1];
    if (i + 2 + ln > len) break;
    const uint8_t* v = d + i + 2;

    if (type == 0x01 && ln >= 4) {
      t = static_cast<int16_t>(v[0] | (v[1] << 8)) / 10.f;
      h = static_cast<uint16_t>(v[2] | (v[3] << 8)) / 10.f;
      hasT = hasH = true;
    } else if (type == 0x02 && ln >= 1) {
      batt = v[0];
    }
    i += 2 + ln;
  }
  if (!hasT && !hasH) return false;
  if (t < -40.f || t > 85.f || h < 0.f || h > 100.f) return false;

  xSemaphoreTake(gMx, portMAX_DELAY);
  Sensor* s = slotFor(mac);
  if (s != nullptr) {
    s->tempC = t;
    s->humidity = h;
    s->hasTemp = hasT;
    s->hasHum = hasH;
    if (batt >= 0) s->batteryPct = batt;
    s->rssi = rssi;
    if (gw) { s->rssiGw = rssi; s->gwAt = millis(); }
    else    { s->rssiOwn = rssi; s->ownAt = millis(); }
    s->seenAt = millis();
    s->encrypted = false;
    s->needsKey = false;
    s->viaGw = gw;
    s->valid = true;
  }
  xSemaphoreGive(gMx);
  return true;
}

class Cb : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    if (!dev.haveServiceData()) return;

    for (int i = 0; i < static_cast<int>(dev.getServiceDataCount()); ++i) {
      const String sd = dev.getServiceData(i);
      BLEUUID uuid = dev.getServiceDataUUID(i);
      const uint8_t* d = reinterpret_cast<const uint8_t*>(sd.c_str());
      const size_t len = sd.length();
      if (len < 6) continue;

      // Core 3.x opiera BLE na NimBLE — UUID trzyma w ble_uuid_any_t.
      if (uuid.bitSize() != 16) continue;
      const uint16_t u16 = uuid.getNative()->u16.value;

      const String macs = dev.getAddress().toString();
      const char* mac = macs.c_str();

      // Otwarty format (custom firmware pvvx/ATC) — klucza nie trzeba.
      if (u16 == 0x181A && parse181A(d, len, mac, dev.getRSSI())) return;

      // Fabryczny Xiaomi — zaszyfrowany MiBeacon, potrzebny bindkey z NVS.
      if (u16 == 0xFE95 && parseMiBeacon(d, len, mac, dev.getRSSI())) return;

      // Qingping — otwarty tekst, bez klucza.
      if (u16 == 0xFDCD && parseQingping(d, len, mac, dev.getRSSI(), false)) return;

      // Cokolwiek innego — surowo do logu, zeby dalo sie rozpoznac zdalnie.
      if (u16 == 0xFE95 || u16 == 0x181A) {
        char hex[64];
        hexDump(hex, sizeof(hex), d, len > 24 ? 24 : len);
        LOG("BLE ?: %s uuid=%04X len=%u %s", mac, u16, static_cast<unsigned>(len), hex);
      }
    }
  }
};

Cb gCb;

}  // namespace

// Od v51 stos BLE zostaje w pamieci NA STALE. Wczesniej podnosilismy go i oddawali
// przy kazdym nasluchu, bo 72 kB nie miescilo sie obok reszty — kosztowalo to
// fragmentacje sterty, 6 s niedostepnego panelu i sterte spadajaca do 2 kB.
// Po wlaczeniu PSRAM (v50) bufor ekranu wyprowadzil sie z SRAM-u i jest miejsce.
void begin() {
  if (gMx == nullptr) gMx = xSemaphoreCreateMutex();

  const uint32_t h0 = ESP.getFreeHeap();
  BLEDevice::init("");

  BLEScan* sc = BLEDevice::getScan();
  sc->setAdvertisedDeviceCallbacks(&gCb, false);
  sc->setActiveScan(false);  // pasywnie: nie odpytujemy, tylko sluchamy
  sc->setInterval(100);
  sc->setWindow(99);

  gReady = true;
  gErr[0] = '\0';
  LOG("BLE: stos w pamieci na stale, sterta %lu -> %lu B",
      static_cast<unsigned long>(h0), static_cast<unsigned long>(ESP.getFreeHeap()));
}

// Stos BLE kosztuje ~72 kB sterty. Trzymany na stale zostawial 37 kB wolnego —
// za malo na TLS (pogoda, samoloty, OTA) i OtaGuard slusznie cofnal taka wersje
// (v38). Skoro sluchamy 6 s na 45 s, stos podnosimy TYLKO na czas nasluchu
// i zaraz go oddajemy. Miedzy skanami sterta jest nietknieta.
void scan(int seconds) {
  if (!gReady) return;

  gScanning = true;
  BLEScan* sc = BLEDevice::getScan();
  sc->start(seconds, false);
  sc->clearResults();  // inaczej wyniki zostaja na stercie
  gScanning = false;
}

bool scanning() {
  return gScanning;
}

// Ramka z bramki. Ta sama sciezka co wlasny odbior — wiec i to samo odszyfrowanie,
// te same progi, ten sam log. Bramka jest tylko innym uchem, nie innym urzadzeniem.
bool feedRaw(const char* mac, const uint8_t* data, size_t len, int rssi) {
  if (gMx == nullptr) gMx = xSemaphoreCreateMutex();
  if (mac == nullptr || data == nullptr || len < 6) return false;
  return parseMiBeacon(data, len, mac, rssi, true);
}

bool feedRawQingping(const char* mac, const uint8_t* data, size_t len, int rssi) {
  if (gMx == nullptr) gMx = xSemaphoreCreateMutex();
  if (mac == nullptr || data == nullptr || len < 10) return false;
  return parseQingping(data, len, mac, rssi, true);
}

int count() {
  if (gMx == nullptr) return 0;
  xSemaphoreTake(gMx, portMAX_DELAY);
  const int n = gCount;
  xSemaphoreGive(gMx);
  return n;
}

Sensor get(int i) {
  Sensor s{};
  if (gMx == nullptr || i < 0 || i >= MAX_SENSORS) return s;
  xSemaphoreTake(gMx, portMAX_DELAY);
  s = gSensors[i];
  xSemaphoreGive(gMx);
  return s;
}

// Bez mutexu CELOWO - patrz uzasadnienie przy deklaracji w BleSensors.h (adres
// slotu jest zapisywany raz i nigdy nie zmienia wartosci, wiec nie ma wyscigu,
// przed ktorym mialby bronic). Zwracamy wskaznik do tablicy, nie do kopii.
const char* macOf(int i) {
  if (i < 0 || i >= MAX_SENSORS) return "";
  return gSensors[i].mac;
}

bool ready() {
  return gReady;
}

const char* lastError() {
  return gErr;
}

}  // namespace ble
