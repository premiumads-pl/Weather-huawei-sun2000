#include "BleSensors.h"

#include "Log.h"

#include <Arduino.h>
#include <BLEAdvertisedDevice.h>
#include <BLEDevice.h>
#include <BLEScan.h>

#include <cstring>

namespace ble {
namespace {

Sensor gSensors[MAX_SENSORS];
int gCount = 0;
bool gReady = false;
char gErr[48] = "nie uruchomiony";
SemaphoreHandle_t gMx = nullptr;

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
    s->seenAt = millis();
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

      if (u16 == 0x181A && parse181A(d, len, mac, dev.getRSSI())) return;

      // Nieznana ramka — zapisujemy surowo, zeby dalo sie ja rozpoznac zdalnie.
      // MiBeacon (0xFE95) z fabrycznego firmware bywa zaszyfrowany; wtedy widac
      // to wlasnie tutaj i wiadomo, ze trzeba bindkey.
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

void begin() {
  if (gMx == nullptr) gMx = xSemaphoreCreateMutex();

  const uint32_t before = ESP.getFreeHeap();
  BLEDevice::init("");
  gReady = true;
  snprintf(gErr, sizeof(gErr), "%s", "");
  LOG("BLE: start, sterta %lu -> %lu (koszt %ld B)", static_cast<unsigned long>(before),
      static_cast<unsigned long>(ESP.getFreeHeap()),
      static_cast<long>(before) - static_cast<long>(ESP.getFreeHeap()));
}

void scan(int seconds) {
  if (!gReady) return;

  BLEScan* sc = BLEDevice::getScan();
  sc->setAdvertisedDeviceCallbacks(&gCb, false);
  sc->setActiveScan(false);  // pasywnie: nie odpytujemy, tylko sluchamy
  sc->setInterval(100);
  sc->setWindow(99);
  sc->start(seconds, false);
  sc->clearResults();  // inaczej wyniki zostaja na stercie
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

bool ready() {
  return gReady;
}

const char* lastError() {
  return gErr;
}

}  // namespace ble
