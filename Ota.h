#pragma once

#include <cstdint>

// Aktualizacja przez internet z GitHub Releases.
// Repo jest publiczne, wiec nie potrzeba zadnego tokenu.
//
//   https://github.com/<OWNER>/<REPO>/releases/latest/download/version.json
//   https://github.com/<OWNER>/<REPO>/releases/latest/download/firmware.bin
//
// version.json:  {"version": 12, "notes": "..."}

enum class OtaState : uint8_t { IDLE, CHECKING, DOWNLOADING, DONE, FAILED };

struct OtaStatus {
  OtaState state = OtaState::IDLE;
  int progress = 0;        // 0..100
  int remoteVersion = 0;
  char message[48] = {};
};

class Ota {
 public:
  // Sprawdza wersje; jesli nowsza — pobiera i restartuje urzadzenie.
  // Zwraca true, jesli rozpoczeto aktualizacje (urzadzenie sie zrestartuje).
  bool checkAndUpdate();

  int lastRemoteVersion() const { return lastRemote_; }

 private:
  int lastRemote_ = 0;
  bool fetchRemoteVersion(int& version);
  bool downloadAndFlash();
};

// Status widoczny dla UI (aktualizowany z zadania sieciowego).
OtaStatus& otaStatus();
