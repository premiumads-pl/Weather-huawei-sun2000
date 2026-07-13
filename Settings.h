#pragma once

#include <cstdint>

// Cała konfiguracja urządzenia siedzi w pamięci nieulotnej (NVS).
// W kodzie źródłowym NIE MA żadnych haseł ani adresów — repo może być publiczne,
// a binarka OTA nie zawiera niczego prywatnego.
struct Settings {
  char ssid[33] = {};
  char pass[65] = {};

  char city[40] = "Gdynia";
  float lat = 54.4870f;
  float lon = 18.5216f;

  char modbusHost[24] = {};  // ustawiane w panelu WWW
  uint16_t modbusPort = 502;
  uint16_t pvPeakW = 6000;

  bool otaEnabled = true;

  // --- MQTT / Home Assistant (domyslnie WYLACZONE) ---
  // Prefix jest krotki celowo: wchodzi do kazdego retained pakietu discovery,
  // a bufor klienta MQTT ma tylko 512 B (patrz MqttClient.cpp).
  char mqttHost[40] = {};
  uint16_t mqttPort = 1883;
  char mqttUser[32] = {};
  char mqttPass[64] = {};
  char mqttPrefix[24] = "pogoda-gdynia";
  bool mqttEnabled = false;

  // --- czujniki BLE (Xiaomi LYWSD03MMC) ---
  // Fabryczny firmware szyfruje rozgłaszanie. Klucz (bindkey) wyciąga się z chmury
  // Xiaomi i jest PRYWATNY — dlatego siedzi wyłącznie tutaj, w NVS, nigdy w repo.
  // Czujnik z firmware pvvx/ATC nadaje otwartym tekstem i klucza nie potrzebuje.
  struct BleCfg {
    char mac[18] = {};   // "a4:c1:38:54:f9:a9"
    char name[14] = {};  // "Łazienka"
    uint8_t key[16] = {};
    bool hasKey = false;
  } ble[4];

  const BleCfg* bleFind(const char* mac) const;
  bool bleSet(const char* mac, const char* name, const char* keyHex);  // keyHex: 32 znaki lub ""

  bool hasWifi() const { return ssid[0] != '\0'; }
  bool hasInverter() const { return modbusHost[0] != '\0'; }
  bool hasMqtt() const { return mqttEnabled && mqttHost[0] != '\0'; }

  void load();
  void save();
  void clearWifi();
};

Settings& settings();

// --- profil produkcji PV z bieżącego dnia (trwały po zaniku zasilania) ---
void pvHistoryLoad(struct PvHistory& h);
void pvHistorySave(const struct PvHistory& h);
void pvHistoryClear();
