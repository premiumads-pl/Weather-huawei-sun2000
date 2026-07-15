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

// Profil dnia bieżącego: 1 próbka co 10 minut (144 sloty).
// Dwie serie, żeby wykres pokazywał nie tylko ile wyprodukowaliśmy, ale też ile
// z tego zużyliśmy na miejscu i kiedy musieliśmy dobrać z sieci.
//
// TRWALOSC: ta struktura jest utrwalana w NVS - patrz pvHistoryLoad/Save
// w Settings.cpp. NIE jest zapisywana bajt w bajt: idzie przez wlasna strukture
// PvProfileBlob pod kluczem "prof1", z polem wersji. Kazda zmiana ukladu ALBO
// znaczenia `watts`/`load` (np. przejscie z watow na dziesiatki watow - rozmiar
// zostaje ten sam!) MUSI podbic PV_PROF_VER albo klucz, inaczej stary profil
// wczyta sie jako nowy i wykres pokaze nieprawde bez zadnego ostrzezenia.
struct PvHistory {
  static constexpr int SLOTS = 144;
  uint16_t watts[SLOTS] = {};  // produkcja PV [W]
  uint16_t load[SLOTS] = {};   // pobór domu [W]
  bool filled[SLOTS] = {};
  int day = -1;  // tm_yday, do resetu o północy

  void reset(int yday) {
    for (int i = 0; i < SLOTS; ++i) {
      watts[i] = 0;
      load[i] = 0;
      filled[i] = false;
    }
    day = yday;
  }

  static uint16_t clampW(int32_t w) {
    if (w < 0) return 0;
    return static_cast<uint16_t>(w > 65535 ? 65535 : w);
  }

  void push(int yday, int hour, int minute, int32_t w, int32_t loadW) {
    if (yday != day) {
      reset(yday);
    }
    const int slot = (hour * 60 + minute) / 10;
    if (slot < 0 || slot >= SLOTS) {
      return;
    }
    watts[slot] = clampW(w);
    load[slot] = clampW(loadW);
    filled[slot] = true;
  }

  // Szczyt z OBU serii — pobór potrafi przewyższyć produkcję (dobieramy z sieci),
  // a wtedy czerwony słupek musi się zmieścić w tej samej skali co żółty.
  uint16_t peak() const {
    uint16_t m = 0;
    for (int i = 0; i < SLOTS; ++i) {
      if (!filled[i]) continue;
      if (watts[i] > m) m = watts[i];
      if (load[i] > m) m = load[i];
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
      // NIE "Praca". Zaszyta odpowiedz na "nie wiem" zamieniala KAZDY nieznany kod
      // w uspokajajacy komunikat: falownik mogl zglaszac stan, ktorego nie znamy,
      // a ekran twierdzil, ze wszystko gra. "Stan nieznany" mozna wygooglowac
      // (numer rejestru jest w /api/state), "Praca" nie da sie podwazyc.
      //
      // Zwracamy literal, NIE bufor statyczny. Kusi, zeby wypisac tu kod szesnastkowo
      // przez `static char[]` — to bylby wyscig miedzyrdzeniowy: ta funkcja jest
      // wolana z MqttClient.cpp:553 (netTask, rdzen 0) i z WeatherUi.cpp:990
      // (renderowanie, rdzen 1). Dwa watki, jeden bufor, rwany napis.
      return "Stan nieznany";
  }
}

inline bool pvStatusIsFault(uint16_t code) {
  return code == 0x0300;
}

inline bool pvStatusIsRunning(uint16_t code) {
  return code >= 0x0200 && code <= 0x0202;
}
