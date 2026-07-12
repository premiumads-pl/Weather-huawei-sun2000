#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// Ochrona przed "cegłą" po nieudanej aktualizacji OTA.
//
// FAKT (sprawdzone w sdkconfig rdzenia Arduino ESP32 3.3.10 oraz w źródłach
// ESP-IDF 5.5, nie zgadywane):
//   CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
//   CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK  — wyłączone (i dobrze, to co innego)
// Tylko dzięki temu esp_ota_set_boot_partition() (wołane przez arduinowe
// Update.end()) ustawia stan partycji na ESP_OTA_IMG_NEW — patrz funkcja
// set_new_state_otadata() w components/app_update/esp_ota_ops.c. Sprzętowy
// rollback ESP-IDF DZIAŁA i wygląda tak:
//
//   Update.end()  -> esp_ota_set_boot_partition() -> stan partycji ESP_OTA_IMG_NEW
//   bootloader (1. start) -> zamienia stan na ESP_OTA_IMG_PENDING_VERIFY
//   aplikacja MUSI potwierdzić, że żyje: esp_ota_mark_app_valid_cancel_rollback()
//   jeśli nie potwierdzi i urządzenie się zrestartuje -> bootloader widzi
//   PENDING_VERIFY, oznacza wersję jako ABORTED i wraca na poprzednią partycję.
//
// Potwierdzamy DOPIERO po dowodzie, że wersja naprawdę działa:
//   * WiFi połączone,
//   * co najmniej jedno udane pobranie danych z sieci (HTTPS, czyli i TLS),
//   * sterta nie leży na dnie (patrz TRIAL_MIN_HEAP w OtaGuard.cpp).
// Ostatni warunek to wprost obrona przed regresją typu v14: globalny dekoder PNG
// podniósł RAM statyczny 53 -> 100 kB, zabrakło heapu na TLS, OTA przestało
// działać NA ZAWSZE i urządzenie dało się odratować tylko kablem USB.
//
// UWAGA na pętlę "pobierz cegłę -> rollback -> pobierz tę samą cegłę". Bootloader
// cofa wersję sam, po cichu, także wtedy, gdy nowa wersja wywali się PANIKIEM,
// zanim zdąży cokolwiek zapisać — czyli w najczęstszym scenariuszu. Dlatego numer
// wersji wchodzącej w okres próbny zapisujemy w NVS PRZED restartem
// (otaGuardArmTrial), a rozliczamy go przy następnym starcie: jeśli budzimy się
// na INNEJ wersji niż zapisana, to znaczy, że tamta nie przeżyła — i dopisujemy
// jej porażkę. Po MAX_BAD_TRIES porażkach przestajemy ją pobierać automatycznie.
// ---------------------------------------------------------------------------

enum class TrialState : uint8_t {
  STABLE = 0,     // zwykły start — nie ma czego potwierdzać
  TRIAL = 1,      // świeża, niepotwierdzona wersja; trwa okres próbny
  CONFIRMED = 2,  // wersja próbna potwierdzona w tym starcie
};

// Wywołać NA POCZĄTKU setup(), zaraz po settings().load(), a PRZED alokacją
// dużych buforów (bufor ekranu potrafi się nie zmieścić i to też jest regresja).
void otaGuardBegin();

// Wywołać w KAŻDEJ iteracji pętli zadania sieciowego — także wtedy, gdy nie ma
// WiFi. Brak sieci jest jednym z powodów, dla których wersję trzeba cofnąć,
// więc ten tick nie może siedzieć za żadnym "continue".
void otaGuardTick();

// Wersja próbna nie jest w stanie nawet wystartować (np. brak RAM na bufor
// ekranu). Nie ma po co czekać 3 minuty — cofamy się od razu.
void otaGuardFatal(const char* why);

// Wołane przez Ota tuż PRZED restartem w świeżo wgraną wersję. Zapisuje w NVS
// numer wersji, która wchodzi w okres próbny. Bez tego nie da się rozpoznać, że
// wersja padła tak wcześnie, że nie zdążyła nic o sobie powiedzieć.
void otaGuardArmTrial(int version);

// Czy trwa okres próbny (UI pokazuje wtedy "vN · próbna").
bool otaTrialActive();

// Czy ta wersja została już odrzucona po rollbacku. Chroni przed pętlą
// "pobierz cegłę -> rollback -> pobierz tę samą cegłę -> ...".
bool otaVersionRejected(int version);

// Czy ostatni reset to była awaria (panic / watchdog / zanik napięcia).
bool resetWasCrash();

const char* resetReasonText(uint8_t r);   // do /api/diag
const char* resetReasonShort(uint8_t r);  // do karty na ekranie statystyk
