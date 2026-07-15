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
    char name[24] = {};  // "Łazienka Góra" — UTF-8, wiec 2 B na znak z ogonkiem
    uint8_t key[16] = {};
    bool hasKey = false;
  } ble[4];

  // --- Viessmann (piec) ---
  // Client ID jest PUBLICZNY (siedzi w kazdej instalacji PyViCare) — ale refresh
  // token juz nie: przez 180 dni daje pelny dostep do ogrzewania. Dlatego oba leza
  // wylacznie w NVS, nigdy w repo, i /api/state nie zwraca tokena — tylko flage.
  char viClientId[40] = {};
  char viRefresh[600] = {};      // JWT bywa dlugi
  char viInstallation[12] = {};  // cache — zeby nie pytac o to co odczyt
  char viGateway[20] = {};
  uint32_t viAuthAt = 0;         // epoch autoryzacji — refresh token zyje 180 dni
  bool viEnabled = false;

  bool hasViessmann() const { return viEnabled && viClientId[0] != '\0' && viRefresh[0] != '\0'; }
  void viSave();

  // Odczyty licznika gazu wpisywane recznie — do weryfikacji, czy piec nie klamie.
  static constexpr int METERS = 8;
  struct MeterCfg { uint32_t day = 0; float m3 = 0.f; };
  MeterCfg meters[METERS];
  bool meterAdd(uint32_t day, float m3);
  bool meterDel(uint32_t day);
  void meterSave();

  // Bramka BLE (Shelly) — druga para uszu dla czujnikow poza zasiegiem wyswietlacza.
  char bleGwHost[24] = {};

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

// --- historia czujnikow BLE (24 h, ruchome okno; przezywa zanik zasilania) ---
void roomHistoryLoad(struct RoomHistory& h);
void roomHistorySave(const struct RoomHistory& h);
