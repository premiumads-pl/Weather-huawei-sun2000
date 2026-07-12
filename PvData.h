#pragma once

#include <cstdint>

struct PvSnapshot {
  int32_t powerDcW = 0;
  int32_t powerAcW = 0;
  int32_t gridPowerW = 0;   // + oddawanie do sieci / - pobór
  int32_t houseLoadW = 0;   // wyliczone: AC - grid
  float energyTotalKwh = 0.f;
  float energyTodayKwh = 0.f;
  float pvVoltageV = 0.f;
  float inverterTempC = 0.f;
  float efficiencyPct = 0.f;
  uint16_t statusCode = 0;  // rejestr 32089
  bool meterOk = false;
  bool valid = false;
};

struct PvModel {
  PvSnapshot data{};
  char errorMsg[48] = {};
  bool online = false;
  // Noc: falownik nie odpowiada, ale ma do tego prawo (Huawei wyłącza Modbus TCP
  // po zachodzie). Stan neutralny — szary, nie czerwony. Patrz pvMayBeAsleep().
  bool asleep = false;
};

// Profil produkcji z dnia bieżącego: 1 próbka co 10 minut (144 sloty).
struct PvHistory {
  static constexpr int SLOTS = 144;
  uint16_t watts[SLOTS] = {};
  bool filled[SLOTS] = {};
  int day = -1;  // tm_yday, do resetu o północy

  void reset(int yday) {
    for (int i = 0; i < SLOTS; ++i) {
      watts[i] = 0;
      filled[i] = false;
    }
    day = yday;
  }

  void push(int yday, int hour, int minute, int32_t w) {
    if (yday != day) {
      reset(yday);
    }
    const int slot = (hour * 60 + minute) / 10;
    if (slot < 0 || slot >= SLOTS) {
      return;
    }
    const uint16_t v = (w < 0) ? 0 : static_cast<uint16_t>(w > 65535 ? 65535 : w);
    watts[slot] = v;
    filled[slot] = true;
  }

  uint16_t peak() const {
    uint16_t m = 0;
    for (int i = 0; i < SLOTS; ++i) {
      if (filled[i] && watts[i] > m) {
        m = watts[i];
      }
    }
    return m;
  }
};

// Kody stanu falownika Huawei (rej. 32089) -> etykieta PL.
inline const char* pvStatusLabel(uint16_t code) {
  switch (code) {
    case 0x0000:
    case 0x0001:
    case 0x0002:
    case 0x0003:
      return "Czuwanie";
    case 0x0100:
      return "Start";
    case 0x0200:
      return "Praca";
    case 0x0201:
      return "Praca (limit)";
    case 0x0202:
      return "Praca (derating)";
    case 0x0300:
      return "AWARIA";
    case 0x0301:
    case 0x0302:
    case 0x0303:
    case 0x0304:
    case 0x0305:
    case 0x0306:
    case 0x0307:
    case 0x0308:
      return "Wyłączony";
    case 0x0500:
    case 0x0600:
    case 0x0700:
    case 0x0800:
    case 0x0900:
      return "Test";
    case 0xA000:
      return "Brak słońca";
    default:
      return "Praca";
  }
}

inline bool pvStatusIsFault(uint16_t code) {
  return code == 0x0300;
}

inline bool pvStatusIsRunning(uint16_t code) {
  return code >= 0x0200 && code <= 0x0202;
}
