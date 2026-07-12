#pragma once

#include <cstdint>

// UWAGA: w tym pliku NIE MA żadnych sekretów.
// SSID, hasło, IP falownika i lokalizacja siedzą w pamięci NVS (patrz Settings.h)
// i konfiguruje się je przez panel WWW urządzenia.

namespace cfg {

// ---------- Aktualizacje OTA (publiczne repo, bez tokenu) ----------
constexpr const char* OTA_VERSION_URL =
    "https://github.com/premiumads-pl/Weather-huawei-sun2000/releases/latest/download/"
    "version.json";
constexpr const char* OTA_FIRMWARE_URL =
    "https://github.com/premiumads-pl/Weather-huawei-sun2000/releases/latest/download/"
    "firmware.bin";
constexpr uint32_t OTA_CHECK_MS = 15UL * 60UL * 1000UL;

// ---------- Dioda RGB (bilans z siecią) ----------
constexpr uint8_t LED_DAY = 90;         // jasność w dzień
constexpr uint8_t LED_NIGHT = 12;       // w nocy — ma nie oślepiać
constexpr int32_t LED_BALANCE_W = 300;  // +/- 300 W = "równowaga" (niebieski)

// ---------- Lotnisko / loty ----------
constexpr float EPGD_LAT = 54.3823f;
constexpr float EPGD_LON = 18.4654f;
constexpr int FLIGHT_RADIUS_NM = 40;

// ---------- Ekran ----------
constexpr int SCREEN_W = 320;
constexpr int SCREEN_H = 240;
constexpr uint8_t TFT_ROTATION = 1;
constexpr bool TFT_INVERT_DISPLAY = false;
constexpr bool COLOR_TEST_MODE = false;

constexpr int PIN_TFT_CS = 10;
constexpr int PIN_TFT_DC = 8;
constexpr int PIN_TFT_RST = 9;
constexpr int PIN_TFT_MOSI = 11;
constexpr int PIN_TFT_SCLK = 12;
constexpr int PIN_TFT_BL = 14;

// ---------- Siatka layoutu ----------
constexpr int HEADER_H = 28;
constexpr int PROG_Y = 29;
constexpr int PROG_H = 3;
constexpr int CONTENT_Y = 34;
constexpr int CONTENT_H = 172;
constexpr int FOOTER_Y = 208;
constexpr int FOOTER_H = 32;

// ---------- Podświetlenie / tryb nocny ----------
constexpr uint32_t BL_PWM_FREQ = 5000;
constexpr uint8_t BL_PWM_BITS = 8;
constexpr uint8_t BL_DAY = 255;
constexpr uint8_t BL_NIGHT = 45;
constexpr int NIGHT_FROM_H = 22;
constexpr int NIGHT_TO_H = 6;

// ---------- Czasy ----------
constexpr uint32_t WEATHER_REFRESH_MS = 15UL * 60UL * 1000UL;
constexpr uint32_t PV_REFRESH_MS = 30UL * 1000UL;
constexpr uint32_t PV_STORE_MS = 5UL * 60UL * 1000UL;  // zapis profilu do NVS
constexpr uint32_t WIFI_RETRY_MS = 8000;
constexpr uint32_t RADAR_REFRESH_MS = 5UL * 60UL * 1000UL;  // klatki radaru co ~10 min
constexpr uint32_t FLIGHT_REFRESH_MS = 15000;
constexpr uint32_t FLIGHT_PREFETCH_MS = 6000;
constexpr uint32_t VIEW_HOLD_MS = 9000;
constexpr uint32_t TRANSITION_MS = 340;
constexpr uint32_t ENTER_ANIM_MS = 550;
constexpr uint32_t ALERT_SHOW_MS = 6500;
constexpr uint32_t ALERT_COOLDOWN_MS = 10UL * 60UL * 1000UL;
constexpr uint32_t FRAME_ACTIVE_MS = 33;
constexpr uint32_t FRAME_IDLE_MS = 70;

constexpr int VIEW_COUNT = 5;
constexpr int VIEW_FLIGHTS = 4;
constexpr uint32_t VIEW_HOLD_FLIGHTS_MS = 15000;

}  // namespace cfg
