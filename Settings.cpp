#include "Settings.h"

#include <Arduino.h>
#include <Preferences.h>
#include <cstring>

#include "PvData.h"
#include "GasMeter.h"
#include "RoomHistory.h"

namespace {
Preferences prefs;
Settings gSettings;
constexpr const char* NS_CFG = "pogoda";
constexpr const char* NS_PV = "pvday";
}  // namespace

Settings& settings() {
  return gSettings;
}

void Settings::load() {
  prefs.begin(NS_CFG, true);
  String s = prefs.getString("ssid", "");
  String p = prefs.getString("pass", "");
  String c = prefs.getString("city", "Gdynia");
  String m = prefs.getString("mb", "");
  lat = prefs.getFloat("lat", 54.4870f);
  lon = prefs.getFloat("lon", 18.5216f);
  modbusPort = prefs.getUShort("mbport", 502);
  pvPeakW = prefs.getUShort("peak", 6000);
  otaEnabled = prefs.getBool("ota", true);

  String mh = prefs.getString("mqhost", "");
  String mu = prefs.getString("mquser", "");
  String mp = prefs.getString("mqpass", "");
  String mx = prefs.getString("mqpre", "pogoda-gdynia");
  mqttPort = prefs.getUShort("mqport", 1883);
  mqttEnabled = prefs.getBool("mqen", false);

  String vc = prefs.getString("vicid", "");
  String vr = prefs.getString("viref", "");
  String vi = prefs.getString("viinst", "");
  String vg = prefs.getString("vigw", "");
  viAuthAt = prefs.getUInt("viat", 0);
  String bg = prefs.getString("blegw", "");
  strncpy(bleGwHost, bg.c_str(), sizeof(bleGwHost) - 1);
  if (prefs.getBytesLength("mets") == sizeof(meters)) {
    prefs.getBytes("mets", meters, sizeof(meters));
  }
  viEnabled = prefs.getBool("vien", false);
  strncpy(viClientId, vc.c_str(), sizeof(viClientId) - 1);
  strncpy(viRefresh, vr.c_str(), sizeof(viRefresh) - 1);
  strncpy(viInstallation, vi.c_str(), sizeof(viInstallation) - 1);
  strncpy(viGateway, vg.c_str(), sizeof(viGateway) - 1);

  // czujniki BLE — bindkey jako blob (16 B), nigdy jako tekst w logach/API
  for (int i = 0; i < BLE_SLOTS; ++i) {
    char k[8];
    snprintf(k, sizeof(k), "b%dmac", i);
    String bm = prefs.getString(k, "");
    snprintf(k, sizeof(k), "b%dnam", i);
    String bn = prefs.getString(k, "");
    strncpy(ble[i].mac, bm.c_str(), sizeof(ble[i].mac) - 1);
    strncpy(ble[i].name, bn.c_str(), sizeof(ble[i].name) - 1);

    snprintf(k, sizeof(k), "b%dkey", i);
    ble[i].hasKey = prefs.getBytesLength(k) == 16 &&
                    prefs.getBytes(k, ble[i].key, 16) == 16;
  }
  prefs.end();

  strncpy(ssid, s.c_str(), sizeof(ssid) - 1);
  strncpy(pass, p.c_str(), sizeof(pass) - 1);
  strncpy(city, c.c_str(), sizeof(city) - 1);
  strncpy(modbusHost, m.c_str(), sizeof(modbusHost) - 1);

  strncpy(mqttHost, mh.c_str(), sizeof(mqttHost) - 1);
  strncpy(mqttUser, mu.c_str(), sizeof(mqttUser) - 1);
  strncpy(mqttPass, mp.c_str(), sizeof(mqttPass) - 1);
  strncpy(mqttPrefix, mx.c_str(), sizeof(mqttPrefix) - 1);
  if (mqttPrefix[0] == '\0') {
    strncpy(mqttPrefix, "pogoda-gdynia", sizeof(mqttPrefix) - 1);
  }
}

void Settings::save() {
  prefs.begin(NS_CFG, false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.putString("city", city);
  prefs.putString("mb", modbusHost);
  prefs.putFloat("lat", lat);
  prefs.putFloat("lon", lon);
  prefs.putUShort("mbport", modbusPort);
  prefs.putUShort("peak", pvPeakW);
  prefs.putBool("ota", otaEnabled);
  prefs.putString("mqhost", mqttHost);
  prefs.putString("mquser", mqttUser);
  prefs.putString("mqpass", mqttPass);
  prefs.putString("mqpre", mqttPrefix);
  prefs.putUShort("mqport", mqttPort);
  prefs.putBool("mqen", mqttEnabled);
  prefs.putString("blegw", bleGwHost);
  prefs.end();
}

// Osobno od save(): refresh token zmienia sie co 180 dni, a IDy raz. Nie ma po co
// przepisywac calej konfiguracji przy kazdym odswiezeniu.
void Settings::viSave() {
  prefs.begin(NS_CFG, false);
  prefs.putString("vicid", viClientId);
  prefs.putString("viref", viRefresh);
  prefs.putString("viinst", viInstallation);
  prefs.putString("vigw", viGateway);
  prefs.putUInt("viat", viAuthAt);
  prefs.putBool("vien", viEnabled);
  prefs.end();
}

void Settings::meterSave() {
  prefs.begin(NS_CFG, false);
  prefs.putBytes("mets", meters, sizeof(meters));
  prefs.end();
}

// Trzymamy posortowane po dacie. Ten sam dzien = nadpisanie (poprawka literowki).
bool Settings::meterAdd(uint32_t day, float m3) {
  if (day == 0 || m3 <= 0.f) return false;

  int slot = -1;
  for (int i = 0; i < METERS; ++i) {
    if (meters[i].day == day) { slot = i; break; }
  }
  if (slot < 0) {
    for (int i = 0; i < METERS; ++i) {
      if (meters[i].day == 0) { slot = i; break; }
    }
  }
  if (slot < 0) {   // pelno — wyrzucamy najstarszy
    slot = 0;
    for (int i = 1; i < METERS; ++i) {
      if (meters[i].day < meters[slot].day) slot = i;
    }
  }
  meters[slot].day = day;
  meters[slot].m3 = m3;

  for (int i = 0; i < METERS - 1; ++i) {
    for (int j = i + 1; j < METERS; ++j) {
      const bool swap = (meters[i].day == 0 && meters[j].day != 0) ||
                        (meters[j].day != 0 && meters[i].day != 0 && meters[j].day < meters[i].day);
      if (swap) { MeterCfg t = meters[i]; meters[i] = meters[j]; meters[j] = t; }
    }
  }
  meterSave();
  return true;
}

bool Settings::meterDel(uint32_t day) {
  for (int i = 0; i < METERS; ++i) {
    if (meters[i].day == day) {
      meters[i].day = 0;
      meters[i].m3 = 0.f;
      meterSave();
      return true;
    }
  }
  return false;
}

const Settings::BleCfg* Settings::bleFind(const char* mac) const {
  for (int i = 0; i < BLE_SLOTS; ++i) {
    if (ble[i].mac[0] != '\0' && strcasecmp(ble[i].mac, mac) == 0) return &ble[i];
  }
  return nullptr;
}

// keyHex: 32 znaki hex (bindkey z chmury Xiaomi) albo "" — czujnik z otwartym
// firmware klucza nie potrzebuje.
bool Settings::bleSet(const char* mac, const char* name, const char* keyHex) {
  if (mac == nullptr || mac[0] == '\0') return false;

  int slot = -1;
  for (int i = 0; i < BLE_SLOTS; ++i) {
    if (strcasecmp(ble[i].mac, mac) == 0) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    for (int i = 0; i < BLE_SLOTS; ++i) {
      if (ble[i].mac[0] == '\0') {
        slot = i;
        break;
      }
    }
  }
  if (slot < 0) return false;

  BleCfg& c = ble[slot];
  strncpy(c.mac, mac, sizeof(c.mac) - 1);
  c.mac[sizeof(c.mac) - 1] = '\0';
  strncpy(c.name, name ? name : "", sizeof(c.name) - 1);
  c.name[sizeof(c.name) - 1] = '\0';

  // Puste pole = BEZ ZMIAN (tak samo jak hasło MQTT). Wcześniej pusty klucz kasował
  // zapisany — czyli sama zmiana nazwy wywalała bindkey. Skasować można wpisując "-".
  if (keyHex != nullptr && strlen(keyHex) == 32) {
    for (int i = 0; i < 16; ++i) {
      char b[3] = {keyHex[i * 2], keyHex[i * 2 + 1], '\0'};
      c.key[i] = static_cast<uint8_t>(strtoul(b, nullptr, 16));
    }
    c.hasKey = true;
  } else if (keyHex != nullptr && strcmp(keyHex, "-") == 0) {
    memset(c.key, 0, sizeof(c.key));
    c.hasKey = false;
  }

  char k[8];
  prefs.begin(NS_CFG, false);
  snprintf(k, sizeof(k), "b%dmac", slot);
  prefs.putString(k, c.mac);
  snprintf(k, sizeof(k), "b%dnam", slot);
  prefs.putString(k, c.name);
  snprintf(k, sizeof(k), "b%dkey", slot);
  if (c.hasKey) {
    prefs.putBytes(k, c.key, 16);
  } else {
    prefs.remove(k);
  }
  prefs.end();
  return true;
}

void Settings::clearWifi() {
  ssid[0] = '\0';
  pass[0] = '\0';
  prefs.begin(NS_CFG, false);
  prefs.remove("ssid");
  prefs.remove("pass");
  prefs.end();
}

// ------------------------------------------------- profil produkcji PV -------
// Zapisujemy tylko surowe waty (144 sloty po 10 minut) + numer dnia.
// Blob ma 288 B, wiec zapis co 10 minut jest dla NVS zupelnie niegrozny.

void pvHistoryLoad(PvHistory& h) {
  prefs.begin(NS_PV, true);
  const int day = prefs.getInt("day", -1);
  const size_t need = sizeof(h.watts);
  const size_t have = prefs.getBytesLength("w");
  if (day >= 0 && have == need) {
    prefs.getBytes("w", h.watts, need);
    // Pobór doszedł później niż produkcja — starszy zapis go nie ma. Wtedy po
    // prostu zostaje wyzerowany i wykres dorysuje zużycie od tej chwili.
    if (prefs.getBytesLength("l") == sizeof(h.load)) {
      prefs.getBytes("l", h.load, sizeof(h.load));
    }
    h.day = day;
    for (int i = 0; i < PvHistory::SLOTS; ++i) {
      h.filled[i] = h.watts[i] > 0 || h.load[i] > 0;
    }
    Serial.printf("PV: wczytano profil dnia %d z NVS\n", day);
  } else {
    h.reset(-1);
  }
  prefs.end();
}

void pvHistorySave(const PvHistory& h) {
  prefs.begin(NS_PV, false);
  prefs.putInt("day", h.day);
  prefs.putBytes("w", h.watts, sizeof(h.watts));
  prefs.putBytes("l", h.load, sizeof(h.load));
  prefs.end();
}

void pvHistoryClear() {
  prefs.begin(NS_PV, false);
  prefs.clear();
  prefs.end();
}

// ---------------------------------------- historia czujnikow BLE (24 h) -------
// Blob ma ~1,7 kB i leci do NVS co 10 minut — tak samo jak profil PV.
// Zapisujemy CALY bufor razem z numerem slotu; bez niego po restarcie nie dalo by
// sie stwierdzic, ktore probki sa jeszcze wazne.

void roomHistoryLoad(RoomHistory& h) {
  prefs.begin(NS_PV, true);
  const size_t need = sizeof(RoomHistory);
  if (prefs.getBytesLength("rh") == need) {
    prefs.getBytes("rh", &h, need);
    Serial.printf("BLE: wczytano historie 24 h (slot %lu)\n",
                  static_cast<unsigned long>(h.lastSlot));
  } else {
    h.reset();
  }
  prefs.end();
}

void roomHistorySave(const RoomHistory& h) {
  prefs.begin(NS_PV, false);
  prefs.putBytes("rh", &h, sizeof(RoomHistory));
  prefs.end();
}
