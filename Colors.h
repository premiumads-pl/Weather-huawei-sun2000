#pragma once

#include <cstdint>

// Standardowy RGB565 — zamianę R/B robi sterownik TFT_eSPI (TFT_BGR w User_Setup.h).
inline constexpr uint16_t C565(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

namespace col {

// --- baza ---
constexpr uint16_t BG = C565(6, 10, 20);
constexpr uint16_t BG_CARD = C565(17, 30, 50);
constexpr uint16_t BG_CARD_HI = C565(26, 46, 74);
constexpr uint16_t HEADER = C565(10, 24, 44);
constexpr uint16_t DIVIDER = C565(32, 56, 86);

// --- typografia ---
constexpr uint16_t TEXT = C565(250, 252, 255);
constexpr uint16_t TEXT_DIM = C565(140, 165, 195);
constexpr uint16_t TEXT_MUTE = C565(90, 112, 140);
constexpr uint16_t ACCENT = C565(0, 220, 240);
constexpr uint16_t ACCENT_WARM = C565(255, 170, 40);

// --- pogoda ---
constexpr uint16_t SUN = C565(255, 210, 50);
constexpr uint16_t CLOUD = C565(175, 195, 220);
constexpr uint16_t RAIN = C565(60, 150, 255);
constexpr uint16_t RAIN_DK = C565(24, 68, 130);
constexpr uint16_t SNOW = C565(230, 242, 255);
constexpr uint16_t STORM = C565(150, 120, 255);
constexpr uint16_t WIND = C565(90, 215, 195);
constexpr uint16_t HUMID = C565(60, 175, 255);
constexpr uint16_t PRESS = C565(190, 160, 255);

// --- skala temperatury ---
constexpr uint16_t T_FREEZE = C565(120, 190, 255);
constexpr uint16_t T_COLD = C565(60, 140, 255);
constexpr uint16_t T_MILD = C565(70, 215, 120);
constexpr uint16_t T_WARM = C565(255, 190, 40);
constexpr uint16_t T_HOT = C565(255, 70, 30);

// --- fotowoltaika ---
constexpr uint16_t PV_SOLAR = C565(255, 195, 30);
constexpr uint16_t PV_SOLAR_DK = C565(90, 66, 8);
constexpr uint16_t PV_EXPORT = C565(40, 225, 110);
constexpr uint16_t PV_IMPORT = C565(255, 105, 65);
constexpr uint16_t PV_HOUSE = C565(120, 190, 255);
constexpr uint16_t PV_TRACK = C565(22, 40, 62);

// --- stany ---
constexpr uint16_t OK = C565(40, 220, 120);
constexpr uint16_t WARN = C565(255, 175, 40);
constexpr uint16_t ERR = C565(255, 70, 70);
constexpr uint16_t ALERT_BG = C565(52, 22, 14);

// --- mapa lotow ---
constexpr uint16_t MAP_SEA = C565(7, 17, 36);
constexpr uint16_t MAP_LAND = C565(24, 46, 40);
constexpr uint16_t MAP_COAST = C565(72, 132, 118);
constexpr uint16_t MAP_LABEL = C565(96, 130, 150);
constexpr uint16_t FLY_ARRIVE = C565(40, 230, 110);   // laduje w Gdansku
constexpr uint16_t FLY_DEPART = C565(255, 165, 40);   // startuje z Gdanska
constexpr uint16_t FLY_OVER = C565(235, 243, 255);    // przelot

// --- wykresy ---
constexpr uint16_t GRID = C565(26, 44, 68);
constexpr uint16_t GRID_HI = C565(40, 66, 98);
constexpr uint16_t CHART_SPARK_BG = C565(12, 20, 34);

}  // namespace col
