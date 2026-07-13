#include "Settings.h"

#include <Arduino.h>
#include <Preferences.h>
#include <cstring>

#include "PvData.h"

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
  prefs.end();
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
