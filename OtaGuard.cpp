#include "OtaGuard.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <cstdio>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <esp_timer.h>

#include "Log.h"
#include "Settings.h"
#include "Version.h"

namespace {

constexpr const char* NS = "otaguard";

// Ile czasu od startu dajemy nowej wersji na udowodnienie, że żyje.
constexpr uint32_t TRIAL_TIMEOUT_MS = 180000;  // 3 minuty

// Poniżej tego progu nie da się zestawić TLS — a bez TLS nie ma już żadnego OTA
// i urządzenie zostaje cegłą do wyjęcia ze ściany. Dokładnie to zabiło v14.
constexpr uint32_t TRIAL_MIN_HEAP = 40000;

// Po tylu porażkach przestajemy w kółko pobierać tę samą zepsutą wersję.
// 2 = wersja dostaje jeszcze jedną szansę (chwilowa awaria WiFi to nie wyrok).
constexpr uint8_t MAX_BAD_TRIES = 2;

// Bezpiecznik ostateczny. Miękki limit pilnuje netTask, ale netTask może się nie
// odpalić (zawieszony setup) albo zawisnąć na blokującym gnieździe. Wtedy wersja
// próbna wisiałaby na ścianie w nieskończoność — bootloader cofnie ją dopiero
// przy restarcie, a nikt tego restartu nie zrobi. Ten timer chodzi w zadaniu
// esp_timer (stos 8192 B — sprawdzone w sdkconfig, starczy na NVS i esp_ota).
constexpr uint64_t TRIAL_HARD_TIMEOUT_US =
    (static_cast<uint64_t>(TRIAL_TIMEOUT_MS) + 20000ULL) * 1000ULL;

// Werdykt może próbować wydać netTask, setup() (otaGuardFatal) albo zadanie
// esp_timer — a wydać go wolno dokładnie raz.
portMUX_TYPE gMux = portMUX_INITIALIZER_UNLOCKED;

volatile TrialState gState = TrialState::STABLE;
volatile bool gDone = true;  // domyślnie: nie ma czego rozstrzygać

uint32_t gMaxHeap = 0;  // największa wolna sterta zaobserwowana w okresie próbnym
int gBadVersion = 0;    // wersja, która nie przeżyła próby (z NVS)
uint8_t gBadCount = 0;  // ile razy nie przeżyła
esp_timer_handle_t gHardTimer = nullptr;

bool isCrash(uint8_t r) {
  return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
         r == ESP_RST_WDT || r == ESP_RST_BROWNOUT;
}

// --- NVS -------------------------------------------------------------------

// Znacznik "ta wersja właśnie wchodzi w okres próbny". Rozliczany przy NASTĘPNYM
// starcie: jeśli obudzimy się na innej wersji, to znaczy, że tamta nie przeżyła.
void nvsSetTrial(int version) {
  Preferences p;
  if (!p.begin(NS, false)) {
    return;
  }
  if (version == 0) {
    p.remove("trialver");
  } else {
    p.putInt("trialver", version);
  }
  p.end();
}

void nvsSetBad(int version, uint8_t count) {
  Preferences p;
  if (!p.begin(NS, false)) {
    return;
  }
  if (version == 0) {
    p.remove("badver");
    p.remove("badcnt");
  } else {
    p.putInt("badver", version);
    p.putUChar("badcnt", count);
  }
  p.end();
}

// --- werdykt ---------------------------------------------------------------

// Zwraca true tylko temu, kto pierwszy zdąży zamknąć okres próbny.
bool claimVerdict() {
  bool mine = false;
  portENTER_CRITICAL(&gMux);
  if (!gDone) {
    gDone = true;
    mine = true;
  }
  portEXIT_CRITICAL(&gMux);
  return mine;
}

void stopHardTimer() {
  // Uchwytu NIE kasujemy: jedną ze ścieżek dojścia tutaj jest callback tego
  // właśnie timera, a esp_timer_delete() z wnętrza własnego callbacku jest
  // zabronione. Timer jest jednorazowy, więc kosztuje ~50 B sterty do restartu.
  if (gHardTimer) {
    esp_timer_stop(gHardTimer);
  }
}

// Wołać wyłącznie po udanym claimVerdict().
void finishConfirm(const char* why) {
  gState = TrialState::CONFIRMED;
  stopHardTimer();

  const esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
  diag().otaTrial = 2;
  diag().otaConfirmAt = millis();
  LOG("OTA: wersja v%d POTWIERDZONA (%s), esp_err=%d\n", FW_VERSION, why,
      static_cast<int>(e));

  // Werdykt zapadł — znacznik próby jest już niepotrzebny i NIE wolno go zostawić
  // (przy następnym starcie zostałby wzięty za nierozliczoną porażkę).
  nvsSetTrial(0);

  // Skoro jednak działa, skasuj ewentualny wpis o jej wcześniejszych porażkach.
  if (gBadVersion == FW_VERSION) {
    gBadVersion = 0;
    gBadCount = 0;
    diag().otaBadVersion = 0;
    nvsSetBad(0, 0);
  }
}

// Wołać wyłącznie po udanym claimVerdict().
void finishRollback(const char* why) {
  LOG("OTA: wersja próbna v%d ODRZUCONA (%s) — cofam na poprzednią\n", FW_VERSION,
      why);
  stopHardTimer();

  if (esp_ota_check_rollback_is_possible()) {
    // Znacznik "trialver" ZOSTAJE w NVS. Porażkę tej wersji dopisze poprzednia
    // wersja przy najbliższym starcie (patrz otaGuardBegin) — tak samo, jak gdyby
    // wersja padła panikiem i cofnął ją sam bootloader. Dzięki temu liczymy w
    // JEDNYM miejscu i nie ma ryzyka policzenia tej samej porażki dwa razy.
    delay(300);  // niech log zdąży wyjść na Serial
    esp_ota_mark_app_invalid_rollback_and_reboot();  // normalnie NIE wraca

    // Tu dochodzimy tylko wtedy, gdy ESP-IDF mimo zgody odmówił cofnięcia.
    LOG("OTA: rollback NIE POWIÓDŁ SIĘ mimo zgody ESP-IDF\n");
    finishConfirm("rollback odmówił");
    return;
  }

  // Nie ma na co wracać: druga partycja jest pusta/uszkodzona albo jej wpis w
  // otadata nigdy nie powstał (tak bywa po świeżym wgraniu przez USB, zanim
  // urządzenie zrobi choć jedno OTA).
  LOG("OTA: brak sprawnej poprzedniej wersji — nie ma na co wrócić\n");

  // Świadoma decyzja: PĘTLA RESTARTÓW BYŁABY GORSZA niż praca w gorszej wersji.
  // Zostawiamy to, co jest, i oznaczamy jako ważne — urządzenie ma wisieć na
  // ścianie i działać, a nie resetować się w kółko.
  finishConfirm("nie ma na co wrócić");
}

// force = twardy limit czasu; werdykt musi zapaść teraz.
void evaluate(bool force) {
  if (gState != TrialState::TRIAL || gDone) {
    return;
  }

  const uint32_t heap = ESP.getFreeHeap();
  if (heap > gMaxHeap) {
    gMaxHeap = heap;
  }

  // 1) Sterta. Bierzemy MAKSIMUM z okresu próbnego, a nie chwilowy odczyt —
  //    normalna praca ma głębokie dołki (TLS, radar, zrzut ekranu), więc jedna
  //    próbka kłamie. Zdrowa wersja w ciągu 3 minut na pewno kiedyś ma > 40 kB;
  //    wersja typu v14 nie zbliży się do tego nigdy.
  const bool heapOk = gMaxHeap > TRIAL_MIN_HEAP;

  // 2) Dowód, że sieć naprawdę działa: WiFi + realnie pobrane dane po HTTPS.
  //    Bierzemy DOWOLNE źródło (pogoda / radar / loty), a nie tylko pogodę — każde
  //    z nich przechodzi przez TLS, więc każde jednakowo dowodzi, że heapu starcza
  //    na handshake. Chwilowa awaria samego Open-Meteo nie może cofać sprawnej wersji.
  //    Falownika tu NIE ma celowo: Modbus to goły TCP, niczego o TLS nie dowodzi.
  const bool netOk = (WiFi.status() == WL_CONNECTED) &&
                     (diag().weatherOkAt != 0 || diag().radarOkAt != 0 ||
                      diag().flightOkAt != 0);

  // Urządzenie bez skonfigurowanego WiFi siedzi w trybie AP i czeka na
  // użytkownika. Nie ma jak pobrać danych, a cofnięcie wersji niczego by nie
  // naprawiło (poprzednia też nie miałaby hasła). Zostaje sam warunek sterty —
  // i to właśnie on łapie regresję typu v14.
  const bool noWifiConfigured = !settings().hasWifi();

  if (heapOk && (netOk || noWifiConfigured)) {
    if (claimVerdict()) {
      finishConfirm(noWifiConfigured ? "brak konfiguracji WiFi, sterta OK"
                                     : "WiFi + dane z sieci + sterta OK");
    }
    return;
  }

  if (force || millis() > TRIAL_TIMEOUT_MS) {
    char why[72];
    snprintf(why, sizeof(why), "heap_max=%lu B, wifi=%d, dane=%d",
             static_cast<unsigned long>(gMaxHeap),
             WiFi.status() == WL_CONNECTED ? 1 : 0, netOk ? 1 : 0);
    if (claimVerdict()) {
      finishRollback(why);
    }
  }
}

void hardTimeoutCb(void*) {
  LOG("OTA: twardy limit okresu próbnego minął — rozstrzygam awaryjnie\n");
  evaluate(true);
}

}  // namespace

// ---------------------------------------------------------------------------

void otaGuardBegin() {
  // --- powód ostatniego resetu: dotąd nie wiedzieliśmy, czy urządzenie się wywala ---
  const uint8_t rr = static_cast<uint8_t>(esp_reset_reason());
  diag().resetReason = rr;

  int trialVer = 0;
  Preferences p;
  if (p.begin(NS, false)) {
    diag().prevResetReason = p.getUChar("rst", 0);

    uint16_t panics = p.getUShort("panics", 0);
    if (isCrash(rr) && panics < 0xFFFF) {
      ++panics;
      p.putUShort("panics", panics);
    }
    diag().panicCount = panics;
    p.putUChar("rst", rr);

    gBadVersion = p.getInt("badver", 0);
    gBadCount = p.getUChar("badcnt", 0);
    trialVer = p.getInt("trialver", 0);
    p.end();
  }

  LOG("Start: reset = %s (poprzedni: %s), awarie od zawsze: %u\n",
      resetReasonText(rr), resetReasonText(diag().prevResetReason),
      static_cast<unsigned>(diag().panicCount));

  // --- czy to świeża, jeszcze niepotwierdzona wersja? ---
  const esp_partition_t* run = esp_ota_get_running_partition();
  esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
  const esp_err_t e =
      run ? esp_ota_get_state_partition(run, &st) : ESP_ERR_NOT_FOUND;

  if (e == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
    gState = TrialState::TRIAL;
    gDone = false;
    gMaxHeap = 0;
    diag().otaTrial = 1;
    diag().otaBadVersion = (gBadCount >= MAX_BAD_TRIES) ? gBadVersion : 0;

    // Znacznik musi wskazywać wersję, która NAPRAWDĘ jest w próbie. Gdy aktualizację
    // wgrała wersja jeszcze bez OtaGuard (przejście na pierwsze wydanie z tą
    // ochroną), nikt go nie uzbroił — dopisujemy się sami.
    if (trialVer != FW_VERSION) {
      nvsSetTrial(FW_VERSION);
    }

    const esp_timer_create_args_t args = {
        .callback = &hardTimeoutCb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "otaguard",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &gHardTimer) == ESP_OK) {
      esp_timer_start_once(gHardTimer, TRIAL_HARD_TIMEOUT_US);
    }

    LOG("OTA: v%d to wersja PRÓBNA (partycja '%s') — mam %lu s na dowód, że działa\n",
        FW_VERSION, run->label,
        static_cast<unsigned long>(TRIAL_TIMEOUT_MS / 1000));
    return;
  }

  // --- zwykły start: stan VALID / UNDEFINED (świeżo wgrane przez USB) / błąd ---
  // Nie ma czego potwierdzać. Milczenie jest tu bezpieczne: w najgorszym razie
  // tracimy ochronę rollbackiem, ale NIGDY nie zrestartujemy sprawnego urządzenia.
  gState = TrialState::STABLE;
  gDone = true;
  diag().otaTrial = 0;

  // --- rozliczenie poprzedniego okresu próbnego ---
  if (trialVer != 0) {
    if (trialVer == FW_VERSION) {
      // Ta wersja próbę już przeszła (albo wgrano ją przez USB) — znacznik jest
      // nieaktualny i musi zniknąć, żeby nikt nie wziął go za porażkę.
      nvsSetTrial(0);
    } else {
      // Budzimy się na INNEJ wersji niż ta, która weszła w okres próbny. Czyli
      // tamta nie przeżyła: cofnął ją bootloader (panic / brownout / zawieszenie
      // + reset) albo nasz własny rollback. Bez tego zapisu za chwilę pobralibyśmy
      // dokładnie tę samą cegłę — i tak w kółko, aż do zajechania flasha.
      const uint8_t cnt =
          (gBadVersion == trialVer && gBadCount < 0xFF)
              ? static_cast<uint8_t>(gBadCount + 1)
              : 1;
      gBadVersion = trialVer;
      gBadCount = cnt;
      nvsSetBad(trialVer, cnt);
      nvsSetTrial(0);
      LOG("OTA: v%d NIE przeżyła okresu próbnego (porażka %u z %u) — wróciliśmy na v%d\n",
          trialVer, static_cast<unsigned>(cnt),
          static_cast<unsigned>(MAX_BAD_TRIES), FW_VERSION);
    }
  }

  diag().otaBadVersion = (gBadCount >= MAX_BAD_TRIES) ? gBadVersion : 0;
  if (diag().otaBadVersion != 0) {
    LOG("OTA: wersja v%d jest zablokowana (%u porażek) — pobierze się tylko ręcznie\n",
        gBadVersion, static_cast<unsigned>(gBadCount));
  }
}

void otaGuardTick() {
  evaluate(false);
}

void otaGuardFatal(const char* why) {
  if (gState != TrialState::TRIAL || gDone) {
    return;
  }
  if (claimVerdict()) {
    finishRollback(why);
  }
}

void otaGuardArmTrial(int version) {
  if (version <= 0) {
    return;
  }
  nvsSetTrial(version);
  LOG("OTA: uzbrajam okres próbny dla v%d (znacznik w NVS)\n", version);
}

bool otaTrialActive() {
  return gState == TrialState::TRIAL && !gDone;
}

bool otaVersionRejected(int version) {
  return version != 0 && version == gBadVersion && gBadCount >= MAX_BAD_TRIES;
}

bool resetWasCrash() {
  return isCrash(diag().resetReason);
}

const char* resetReasonText(uint8_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "włączenie zasilania";
    case ESP_RST_EXT:       return "reset zewnętrzny";
    case ESP_RST_SW:        return "restart programowy";
    case ESP_RST_PANIC:     return "wyjątek (panic)";
    case ESP_RST_INT_WDT:   return "watchdog przerwań";
    case ESP_RST_TASK_WDT:  return "watchdog zadania";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_DEEPSLEEP: return "wybudzenie z uśpienia";
    case ESP_RST_BROWNOUT:  return "zanik napięcia";
    case ESP_RST_SDIO:      return "reset SDIO";
    default:                return "nieznany";
  }
}

const char* resetReasonShort(uint8_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "zasilanie";
    case ESP_RST_EXT:       return "zewnętrzny";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "WDT przerw.";
    case ESP_RST_TASK_WDT:  return "WDT zadania";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_DEEPSLEEP: return "uśpienie";
    case ESP_RST_BROWNOUT:  return "zanik nap.";
    default:                return "nieznany";
  }
}
