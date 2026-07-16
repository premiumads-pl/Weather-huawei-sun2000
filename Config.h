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
constexpr uint32_t LED_TEST_MS = 500;   // autotest kolorow przy starcie (na kolor)

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

// ---------- Czujniki (nowe, v100) ----------
// LDR MUSI byc na ADC1 (GPIO 1-10), nie ADC2 — ADC2 nie dziala przy wlaczonym WiFi
// (radio zabiera przetwornik). GPIO 1 = ADC1_CH0, wolny (TFT ma 8-12/14, dotyk 7).
// Dzielnik: 3,3V -[LDR]- GPIO1 -[7,93k]- GND. Jasno => R_LDR male => napiecie WYZSZE.
constexpr int PIN_LDR = 1;
// AM312 (PIR): modul 3,3 V, OUT 3,3 V. GPIO 13 wolny, nie strapujacy. Odczyt CYFROWY,
// wiec fakt, ze GPIO 13 to ADC2, nie ma znaczenia (nie uzywamy tam ADC — i nie probuj,
// bo ADC2 przy wlaczonym WiFi nie dziala).
// Stalo tu "SR505 (PIR): VCC 5V" — nieprawda w obu polowach. Wlutowany jest AM312 i on
// jest zasilany 3,3 V; potwierdzil wlasciciel 16.07.2026. To nie jest kosmetyka: AM312 ma
// impuls ~2 s i okno martwe ~2 s, a SR505 ~8 s, wiec ta nazwa uzasadniala (poprawny)
// okres odpytywania w loop() zupelnie nie ta liczba. Realne zachowanie TEGO egzemplarza
// mierzymy dopiero teraz — pir_* w /api/diag.
constexpr int PIN_PIR = 13;

// ---------- Siatka layoutu ----------
constexpr int HEADER_H = 28;
constexpr int PROG_Y = 29;
constexpr int PROG_H = 3;
constexpr int CONTENT_Y = 34;
constexpr int CONTENT_H = 172;
// FOOTER_Y/FOOTER_H tu NIE MA i niech tak zostanie. Byly, nie definiowaly niczego
// (zero uzyc w calym repo) i do tego podawaly zle liczby: twierdzily 208/32, gdy
// stopka realnie stoi na 206 i ma 34 px. Ktos, kto w dobrej wierze zmienilby te
// stala, nie zobaczylby ZADNEGO efektu. Jedyne zrodlo prawdy o stopce to
// WeatherUi::VIEW_H (= CONTENT_Y + CONTENT_H) i wysokosc liczona w drawFooterTo().

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
// Noc: falownik śpi (Modbus TCP wyłączony), więc nie ma po co dobijać się co 30 s.
// Wracamy do 30 s natychmiast, gdy tylko falownik znów odpowie.
constexpr uint32_t PV_REFRESH_NIGHT_MS = 5UL * 60UL * 1000UL;
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
constexpr uint32_t FRAME_IDLE_MS = 50;   // 20 fps na statycznym ekranie (pasek odliczania)

// Pomiar czasu klatki (rysowanie / wypchnięcie na SPI) + stan sterty, co 2 s na
// Serial. Przydatne po zmianie na dwa pasy — domyślnie wyłączone, bo to tylko log.
constexpr bool PROFILE_FRAME = false;

constexpr int VIEW_COUNT = 9;   // TERAZ / GODZINY / RADAR / 5 DNI / W DOMU / PIEC / PV / SAMOLOTY / STATYSTYKI
constexpr int VIEW_NOW = 0;     // te dwa brakowaly, wiec switch w drawView() musial
constexpr int VIEW_HOURS = 1;   // uzywac golych literalow "case 0:" / "case 1:"
constexpr int VIEW_RADAR = 2;   // animowana mapa opadow (pomijany, gdy nie pada)
constexpr int VIEW_DAYS = 3;
constexpr int VIEW_HOME = 4;    // czujniki BLE — pomijany, gdy zadnego nie ma
constexpr int VIEW_BOILER = 5;  // piec — pomijany, gdy nieautoryzowany
constexpr int VIEW_PV = 6;
constexpr int VIEW_FLIGHTS = 7;
constexpr int VIEW_STATS = 8;   // ekran serwisowy

// --- progi zdrowia urządzenia (wskaźniki na ekranie statystyk) ---
// Temperatura: czujnik w ESP32-S3 mierzy strukturę (die), nie otoczenie.
// Nota katalogowa: zalecane otoczenie do +85 °C, maksymalna temperatura złącza
// (Tj) 125 °C — i to jest koniec skali, nie punkt pracy.
constexpr float CPU_T_MIN = 20.f;   // początek skali
constexpr float CPU_T_OK = 70.f;    // do tego miejsca: spokojnie (zielony)
constexpr float CPU_T_WARN = 90.f;  // powyżej: gorąco (żółty -> czerwony)
constexpr float CPU_T_SPEC = 85.f;  // granica z noty katalogowej — kreska na skali
constexpr float CPU_T_MAX = 125.f;  // Tj max — koniec skali

// Wolna sterta: poniżej DANGER radar nie ma jak zdekodować PNG, a TLS się dławi.
constexpr uint32_t HEAP_DANGER = 25000;
constexpr uint32_t HEAP_WARN = 45000;
constexpr uint32_t HEAP_FULL = 160000;  // pełna skala wskaźnika
constexpr uint32_t VIEW_HOLD_FLIGHTS_MS = 15000;
constexpr uint32_t VIEW_HOLD_STATS_MS = VIEW_HOLD_MS;   // tyle samo co reszta
constexpr uint32_t VIEW_HOLD_RADAR_MS = 16000;  // tyle, zeby animacja zdazyla przejsc 2x
constexpr uint32_t RADAR_MAP_REFRESH_MS = 10UL * 60UL * 1000UL;
constexpr uint32_t RADAR_FRAME_MS = 650;       // wolniej = oko nadaza za frontem

}  // namespace cfg
