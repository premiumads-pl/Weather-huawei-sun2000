#pragma once

#include <cstdint>

// Aktualizacja przez internet z GitHub Releases.
// Repo jest publiczne, wiec nie potrzeba zadnego tokenu.
//
//   https://github.com/<OWNER>/<REPO>/releases/latest/download/version.json
//   https://github.com/<OWNER>/<REPO>/releases/latest/download/firmware.bin
//
// version.json:  {"version": 12, "size": 1835152, "sha256": "ab12...", "notes": "..."}

enum class OtaState : uint8_t { IDLE, CHECKING, DOWNLOADING, DONE, FAILED };

// To, co version.json ZAPOWIADA o pliku firmware.bin. Zyje jako zmienna LOKALNA
// w Ota::checkAndUpdate() (stos netTask) i jest podawana dalej przez referencje —
// swiadomie NIE jest polem klasy Ota, bo `ota` (pogoda-gdynia.ino) jest obiektem
// globalnym i sizeof(OtaManifest) = 76 B (4 + 4 + 65, wyrownane do 4) weszloby na
// stale do .bss, mimo ze te dane zyja tylko przez jedno wywolanie. Zero sterty.
//
// PO CO TO W OGOLE: pola "size" i "sha256" byly w version.json od zawsze, ale kod OTA
// je IGNOROWAL. 16.08.2026 CDN GitHuba oddal urzadzeniu uszkodzona/nieaktualna tresc
// (dwa kolejne sprawdzenia wersji zwrocily 154, gdy na dysku bylo juz 155), firmware
// przeszedl na partycje "w calosci", a poleglo dopiero Update.end(true) z bledem 9 —
// czyli w jedynym momencie, w ktorym nie da sie juz powiedziec, CO poszlo zle.
// Majac te dwa pola urzadzenie odrzuca zly plik samo i mowi, co dostalo.
//
// OBA POLA SA OPCJONALNE. Starsze wydania (i recznie skladany version.json) moga ich
// nie miec — brak pola znaczy "nie ma czego sprawdzic", a NIE "plik jest zly".
struct OtaManifest {
  int version = 0;
  int size = 0;           // bajty; 0 = pola nie bylo w JSON-ie
  char sha256[65] = {};   // 64 znaki hex + NUL; pusty napis = pola nie bylo
};

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
  //
  // manual = true oznacza "uzytkownik kliknal w panelu". Tylko wtedy wolno
  // ponownie pobrac wersje, ktora zostala juz odrzucona po rollbacku — to jest
  // furtka ratunkowa, gdyby blokada zadzialala niepotrzebnie.
  bool checkAndUpdate(bool manual = false);

  int lastRemoteVersion() const { return lastRemote_; }

 private:
  int lastRemote_ = 0;
  // Wypelnia CALY manifest, nie samo `version` — pobranie i weryfikacja musza patrzec
  // na ten sam opis wydania, a version.json jest czytany tylko raz.
  bool fetchRemoteVersion(OtaManifest& man);
  // Manifest przez const& (a nie kopia): 76 B mniej na stosie netTask przy kazdym
  // wywolaniu i jasne, ze downloadAndFlash() go nie zmienia.
  bool downloadAndFlash(const OtaManifest& man);
};

// Status widoczny dla UI (aktualizowany z zadania sieciowego).
OtaStatus& otaStatus();

// Panel WWW tylko ZGŁASZA chęć sprawdzenia — samo OTA robi zadanie sieciowe.
// Dzięki temu obiekt Update nigdy nie jest używany z dwóch zadań naraz.
void requestOtaCheck();
bool takeOtaRequest();

// UI zgłasza, że oddał 150 kB bufora ekranu — dopiero wtedy ruszamy z TLS.
void otaUiBufferFreed();
