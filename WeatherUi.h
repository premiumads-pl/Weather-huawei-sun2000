#pragma once

#include <TFT_eSPI.h>

#include "FlightData.h"
#include "PvData.h"
#include "WeatherData.h"

enum class AlertKind : uint8_t {
  NONE = 0,
  STORM,
  WIND,
  FROST,
  HEAT,
  HEAVY_RAIN,
  PV_FAULT,
  PV_OFFLINE
};

struct Alert {
  AlertKind kind = AlertKind::NONE;
  char title[24] = {};
  char text[48] = {};
  uint16_t color = 0;
  int iconCode = -1;  // kod WMO do ikony, -1 = brak
};

class WeatherUi {
 public:
  bool begin();

  void drawBoot(const char* status, int attempt);
  void drawFatal(const char* msg);
  void drawColorTest();
  void drawSetup(const char* apSsid, const char* apPass, const char* apIp);
  void drawOta(int progress, const char* msg);
  void drawNetInfo(const char* ssid, const char* ip, int rssi, int secsLeft, int total);

  // Na czas OTA oddajemy 150 kB bufora — inaczej TLS + pobieranie 1,3 MB
  // nie mają z czego działać (zostawało ~17 kB heapu).
  void releaseBuffer();
  bool restoreBuffer();  // odtwarza bufor po zakończonym OTA
  void drawOtaDirect(int progress, const char* msg);

  // Główna pętla rysowania. Zwraca true, jeśli coś się animuje (potrzebne szybkie klatki).
  bool render(const WeatherModel& w, const PvModel& pv, const PvHistory& hist,
              const FlightModel& fl, bool wifiOk, uint32_t nowMs);

  // Czy zadanie sieciowe ma teraz odswiezac loty (ekran aktywny lub zaraz bedzie).
  bool needsFlights(uint32_t nowMs) const;

  void raiseAlert(const Alert& a, uint32_t nowMs);
  void setBacklightTarget(uint8_t v) { blTarget_ = v; }
  void tickBacklight();

 private:
  TFT_eSPI tft_;
  TFT_eSprite spr_{&tft_};
  bool ready_ = false;
  bool freed_ = false;

  // rotacja widoków
  uint8_t view_ = 0;
  uint8_t prevView_ = 0;
  uint32_t viewStart_ = 0;
  uint32_t enterStart_ = 0;
  uint32_t transStart_ = 0;
  bool transitioning_ = false;

  // alert
  Alert alert_{};
  bool alertActive_ = false;
  uint32_t alertStart_ = 0;

  // animowane liczniki
  float animAcW_ = 0.f;
  float animLoadW_ = 0.f;
  float animGridW_ = 0.f;
  float pvScaleW_ = 0.f;

  // podświetlenie
  uint8_t blCurrent_ = 0;
  uint8_t blTarget_ = 255;

  // temperatura rdzenia ESP32-S3 (odczyt co 2 s)
  float cpuTempC_ = 0.f;
  uint32_t cpuTempAt_ = 0;

  // rysowanie
  void drawHeader(const WeatherModel& w, bool wifiOk, uint32_t nowMs);
  void drawProgress(uint32_t nowMs);
  void drawFooter(const PvModel& pv, bool wifiOk);
  void drawSysBox();
  void drawContentBg();
  void drawView(uint8_t view, int ox, float t, const WeatherModel& w, const PvModel& pv,
                const PvHistory& hist, const FlightModel& fl);
  void drawViewNow(int ox, float t, const WeatherModel& w);
  void drawViewHours(int ox, float t, const WeatherModel& w);
  void drawViewDays(int ox, float t, const WeatherModel& w);
  void drawViewPv(int ox, float t, const PvModel& pv, const PvHistory& hist);
  void drawViewFlights(int ox, float t, const FlightModel& fl);
  void drawAlert(float t);
  void drawNoData(int ox, const char* msg);
  uint32_t holdFor(uint8_t view) const;
};
