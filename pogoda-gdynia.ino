/*
  Pogoda + Fotowoltaika Huawei SUN2000 — ESP32-S3 + ST7789 320x240
  https://github.com/premiumads-pl/Weather-huawei-sun2000
  ------------------------------------------------------------------
  5 widoków rotujących automatycznie (płytka nie ma przycisków):
    1. TERAZ    — temperatura, ikona, wiatr/wilgoć/ciśnienie/chmury, wschód/zachód, UV
    2. GODZINY  — krzywa temperatury na 12 h + słupki opadów + ikony co 2 h
    3. 5 DNI    — słupki zakresu min–max, ikony, opady
    4. PV       — moc, wskaźnik obciążenia, profil produkcji dnia, dziś/dom/sieć/temp
    5. SAMOLOTY — mapa Zatoki Gdańskiej + lista lotów (ADS-B)

  Konfiguracja (WiFi, lokalizacja, falownik) siedzi w NVS i ustawia się
  przez panel WWW. W kodzie nie ma żadnych sekretów.

  Aktualizacje: OTA z GitHub Releases, sprawdzane co 15 minut.
*/

#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cstring>

#include "AirClient.h"
#include "AirData.h"
#include "Colors.h"
#include "Config.h"
#include "FlightClient.h"
#include "FlightData.h"
#include "Led.h"
#include "Log.h"
#include "MqttClient.h"
#include "Ota.h"
#include "BleGateway.h"
#include "BleSensors.h"
#include "OtaGuard.h"
#include "RadarMap.h"
#include "GasMeter.h"
#include "Viessmann.h"
#include "RoomHistory.h"
#include "Touch.h"

#include <esp_wifi.h>
#include <esp_event.h>
#include "hal/gpio_ll.h"   // gpio_ll_get_level() — jedyny odczyt pinu, ktory wolno wolac z ISR
#include "Portal.h"
#include "PvClient.h"
#include "PvData.h"
#include "RadarClient.h"
#include "Settings.h"
#include "Version.h"
#include "WeatherClient.h"
#include "WeatherData.h"
#include "WeatherUi.h"

WeatherUi ui;
WeatherClient weatherClient;
PvClient pvClient;
FlightClient flightClient;
RadarClient radarClient;
AirClient airClient;
Ota ota;

SemaphoreHandle_t gLock = nullptr;
WeatherModel gWeather{};
PvModel gPv{};
PvHistory gHist{};
RoomHistory gRooms{};
vi::Model gVi{};
vi::Model uiVi{};
BurnerHistory gBurner{};
BurnerHistory uiBurner{};
GasHistory gGas{};
RoomHistory uiRooms{};
FlightModel gFlights{};
AirModel gAir{};   // jakosc powietrza (v117, GA17 z zapasem GA24) — patrz netTask()
volatile bool gWifiOk = false;
volatile bool gBooting = true;
volatile bool gFlightsNeeded = false;

// Radar potrzebuje 47 kB w JEDNYM kawalku. Sterty jest 113 kB, ale bufor ekranu
// (66 kB) siedzi w srodku i dzieli ja tak, ze najwiekszy spojny blok ma 43 kB.
// Brakuje 2,6 kB — wiec na czas dekodowania PNG oddajemy bufor. Ekran zamiera na
// chwile raz na 5 minut; to lepsze niz radar, ktory nie dziala nigdy.
// Bufor musi zwolnic rdzen 1 (to on rysuje), stad ta wymiana sygnalow.
// Uchwyty zadan — potrzebne, zeby zmierzyc, ile stosu naprawde zuzywaja.
// Oba maja po 16 kB w SRAM (stosow FreeRTOS nie da sie trzymac w PSRAM), wiec
// jesli realnie biora 6-8 kB, mamy do odzyskania kilkanascie kB.
TaskHandle_t gNetTask = nullptr;
TaskHandle_t gWebTask = nullptr;

// Roaming. 802.11r (Fast Transition) NIE jest wkompilowany w rdzen Arduino —
// sprawdzone w symbolach: zero wpa_ft_*. Zostaje przelaczanie "recznie":
// rozlaczenie i polaczenie z mocniejszym punktem po jego BSSID (1-3 s przerwy).
// Ale MOMENT decyzji bierzemy juz ze sprzetu: esp_wifi_set_rssi_threshold()
// wyzwala zdarzenie, gdy sygnal spadnie ponizej progu — nie musimy odpytywac.
volatile bool gRoamWanted = false;
constexpr int kRssiRoamBelow = -67;   // ponizej tego szukamy czegos lepszego
constexpr int kRssiRoamGain = 8;      // ...i przenosimy sie tylko przy realnym zysku

// Zdarzenie leci z zadania systemowego WiFi — nie wolno tu nic robic dlugo.
// Tylko podnosimy flage; przenosinami zajmie sie netTask.
void onRssiLow(void*, esp_event_base_t, int32_t, void*) {
  gRoamWanted = true;
}

// ------------------------------------------------------------ PIR: pomiar ----
// AM312 na GPIO13, przerwanie na OBU zboczach (CHANGE). Odpytywanie co 250 ms w loop()
// zostaje, ale daje tylko fakt "teraz HIGH" — ani dokladnego momentu, ani SZEROKOSCI
// impulsu. A to szerokosc rozstrzyga empirycznie, co tam naprawde jest wlutowane i czy
// modul retriguje. Wyniki ida w liczniki w RTC, NIE do LOG() — patrz komentarz przy
// PirRtc w Log.h (log to okno ~6 minut, pomiar ma trwac tydzien).
//
// To jest POMIAR i tylko pomiar: PIR nie steruje podswietleniem i sterowac nie bedzie
// (rzadzi nim sam LDR, v103+). Nie ma tu wiec zadnego "timeoutu do dobrania" — jest
// pytanie wlasciciela: kto i kiedy realnie korzysta z lazienki.
//
// CO TU WOLNO — sprawdzone w rdzeniu 3.3.10 na tej maszynie, nie "z pamieci":
//  * gpio_ll_get_level() — `static inline __attribute__((always_inline))`
//    (hal/esp32s3/include/hal/gpio_ll.h:350), wiec wchodzi w cialo TEJ funkcji i czyta
//    goly rejestr GPIO.in. UWAGA, wbrew intuicji: gpio_get_level() z drivera IRAM-safe
//    NIE JEST — laduje w sekcji .text.gpio_get_level, a sections.ld nie przenosi
//    libesp_driver_gpio.a do IRAM (nie ma go ani w .iram0.text, ani w EXCLUDE_FILE
//    .flash.text). digitalRead() jest jeszcze gorszy: wola perimanGetPinBus() i przy
//    niezgodnosci robi log_w() (esp32-hal-gpio.c:189).
//  * millis() — bezpieczne, ale nie z tego powodu, co sie wydaje; patrz nizej.
// CZEGO TU NIE MA I BYC NIE MOZE: LOG()/logPrintf (bierze portMUX i robi vsnprintf),
// Serial, snprintf, malloc, mutexy, a takze diag() — to skok do innej jednostki
// kompilacji, wiec wskaznik na Diag lapiemy RAZ w setup() (gDiagIsr).
//
// millis() a IRAM — zweryfikowane, bo wynik wychodzi inny niz "oczywisty":
// esp_timer_get_time() faktycznie SIEDZI w IRAM (libesp_timer.a, esp_timer_impl_systimer
// .c.obj, sekcja .iram1.1 — sprawdzone objdumpem), ale opakowanie millis() ma
// ARDUINO_ISR_ATTR (esp32-hal-misc.c:208), a to makro jest PUSTE, bo w sdkconfig tej
// wersji stoi "# CONFIG_ARDUINO_ISR_IRAM is not set" (esp32-hal.h:74-81). Czyli samo
// millis() lezy we FLASHU. Mimo to jest tu bezpieczne i to nie przypadek: ten SAM
// wylaczony przelacznik sprawia, ze attachInterrupt() instaluje serwis przez
// gpio_install_isr_service(ARDUINO_ISR_FLAG) z ARDUINO_ISR_FLAG == 0 (esp32-hal-gpio.c
// :224), czyli BEZ ESP_INTR_FLAG_IRAM — a takie przerwanie IDF sam maskuje na czas, gdy
// cache flasha jest odlaczony (zapis NVS, OTA). To przerwanie nie ma wiec jak wystrzelic
// z zimnym cache i skok do flasha nie ma jak trafic w dziure.
// Konsekwencja, ktora trzeba powiedziec wprost: IRAM_ATTR ponizej NIE czyni calego
// lancucha IRAM-safe, bo dyspozytor __onPinInterrupt() (esp32-hal-gpio.c:202) ma ten sam
// pusty ARDUINO_ISR_ATTR i tez jest we flashu. Przez Arduinowe attachInterrupt() w tej
// konfiguracji rdzenia po prostu NIE DA sie dostac ISR-a w IRAM. IRAM_ATTR zostaje, bo
// nic nie kosztuje poza ~200 B IRAM i domyka calosc, gdyby CONFIG_ARDUINO_ISR_IRAM
// kiedys wlaczyc: wtedy dyspozytor i millis() same wskakuja do IRAM.
Diag* gDiagIsr = nullptr;           // zlapane raz w setup(), zeby ISR nie wolal diag()

// Liczniki pomiaru w pamieci RTC — przezywaja OTA. Po co, gdzie to lezy (RTC SLOW, nie
// FAST!) i dlaczego ISR moze tu pisac — patrz PirRtc w Log.h.
// RTC_NOINIT_ATTR => sekcja .rtc_noinit jest NOLOAD: nikt tego nie inicjalizuje ani przy
// starcie, ani po resecie. Dlatego BEZ inicjalizatora (byloby zignorowane) i dlatego
// pirRtcBegin()/ldrRtcBegin() musza same sprawdzic znacznik waznosci.
//
// !!! KOLEJNOSC TYCH TRZECH LINII JEST CZESCIA KONTRAKTU — gPir MA STAC NA KONCU !!!
//
// To NIE jest kwestia stylu, tylko ADRESU, a adres decyduje o tym, czy trwajacy tydzien
// pomiaru PIR przezyje te aktualizacje:
//  * RTC_NOINIT_ATTR to `_SECTION_ATTR_IMPL(".rtc_noinit", __COUNTER__)` (esp_attr.h:98),
//    czyli KAZDA zmienna dostaje WLASNA sekcje `.rtc_noinit.<licznik>`.
//  * Linker sklada je przez `*(.rtc_noinit .rtc_noinit.*)` (sections.ld:122) BEZ SORT(),
//    czyli w kolejnosci, w jakiej wypadna z pliku .o.
//  * GCC emituje te sekcje w KOLEJNOSCI ODWROTNEJ do deklaracji. To nie jest gwarancja
//    jezyka ani ABI — to OBSERWACJA, sprawdzona przez zamiane tych dwoch linii miejscami
//    i porownanie `nm` (adresy zamienily sie symetrycznie). Dlatego zmienna zadeklarowana
//    JAKO OSTATNIA laduje w RTC JAKO PIERWSZA, pod 0x50000200.
//
// Konsekwencja, ktora kosztowala pierwsze podejscie do v108: przy gLdr zadeklarowanym pod
// gPir-em gPir pojechal z 0x50000200 na 0x500002d8 — czyli nowy kod czytalby PirRtc spod
// adresu, pod ktorym nikt nigdy nic nie zapisal. Magic by sie nie zgodzil, pirRtcBegin()
// zrobilby zimny start i SKASOWAL zbierany tydzien. Zlapal to `nm`, a nie test na biurku,
// bo na biurku to wyglada dokladnie jak poprawny zimny start — i to jest cala pointa
// wymogu sprawdzania `nm` przy kazdej zmianie w RTC.
//
// Stan potwierdzony w tym obrazie: gPir @ 0x50000200 (tak samo, jak w dzialajacym v107 —
// dane przezyja OTA), gLdr @ 0x500002c0, czyli tuz ZA nim, na pamieci, ktorej nikt nigdy
// nie pisal. Magic gLdr sie nie zgodzi i pomiar LDR wystartuje od zera — i o to chodzi,
// on jest nowy.
//
// TRZECIA ZMIENNA RTC (v113): gPvRtc, wstawiona DOKLADNIE wg powyzszej zasady — NAD gLdr,
// gPir zostaje ostatni. Liczy PRZYCZYNY porazek Modbusa do falownika (PvRtc, patrz Log.h) —
// bez tego rozbicia noc 19/20 lipca 2026 (padl kanal Modbus, reczny restart o 00:15
// skasowal wszystko, co moglo powiedziec dlaczego) nie zostawila ani jednego uzytecznego
// licznika. Nazwa `gPv` jest juz zajeta (PvModel gPv{} nizej w tym pliku — biezacy odczyt
// do UI/MQTT), stad `gPvRtc`.
//
// Adres gPvRtc NIE zostal w tej sesji potwierdzony realnym `nm` (nie bylo tu dzialajacego
// zestawu narzedzi ESP32 — patrz notatka w PR/diffie). Jest WYLICZONY z sizeof(LdrRtc) =
// 216 B (4 pola * 4 B + hist[16] + levelS[3] + events[8] * 3 pola + evHead/evCount/
// candStartS/candStartEpoch/candLastPirS/candActive/evSlot): gLdr @ 0x500002c0 + 0xD8 =
// 0x50000398. ZWERYFIKUJ `nm` na zbudowanym ELF-ie PRZED wydaniem (patrz automatyczna
// bariera na adres gPir w tools/release.sh — ten sam pomysl, ale ten skrypt pilnuje
// TYLKO gPir; gLdr i gPvRtc trzeba sprawdzic recznie).
//
// ZANIM DOLOZYSZ CZWARTA ZMIENNA RTC: wstaw ja NAD gPvRtc (gPvRtc, gLdr, gPir zostaja w
// tej kolejnosci) i SPRAWDZ `nm`, ze cala trojka nadal stoi pod tymi samymi adresami.
// Jesli kiedys ta obserwacja o GCC przestanie sie trzymac, wlasciwym lekiem jest JEDNA
// struktura z podstrukturami (`struct { PirRtc pir; LdrRtc ldr; PvRtc pv; }` z osobnymi
// magicami w srodku), bo ukladu pol WEWNATRZ struktury pilnuje ABI, a nie szczescie w
// kolejnosci emisji sekcji.
RTC_NOINIT_ATTR PvRtc gPvRtc;   // nowa (v113) — dostaje NAJWYZSZY adres: zadeklarowana
                                // JAKO PIERWSZA z trojki, wiec GCC wyemituje jej sekcje
                                // JAKO OSTATNIA (patrz "KOLEJNOSC ODWROTNA" wyzej)
RTC_NOINIT_ATTR LdrRtc gLdr;    // zostaje 0x500002c0 — NIETKNIETE dolozeniem gPvRtc
RTC_NOINIT_ATTR PirRtc gPir;    // zostaje 0x50000200 — pilnuje automat w tools/release.sh

// Stan chwilowy ISR-a — ZOSTAJE W DRAM, celowo. To znaczniki millis(), a millis() zeruje
// sie przy restarcie: w RTC przezylyby OTA jako liczby bez znaczenia i pierwsza przerwa
// po aktualizacji wyszlaby losowa (now - gPirFallAt sprzed restartu). W DRAM zeruja sie
// same, wiec pierwszy impuls po restarcie po prostu nie ma od czego liczyc przerwy i nie
// jest liczony — co jest prawda, bo tej przerwy naprawde nie zmierzylismy.
volatile uint32_t gPirRiseAt = 0;   // millis() otwartego impulsu (0 = zaden nie trwa)
volatile uint32_t gPirFallAt = 0;   // millis() konca poprzedniego impulsu (0 = nie bylo)

// Ile zbocz w gore loop() juz zaksiegowal w gPir.byHour. Seed w setup() z gPir.rises:
// po OTA licznik w RTC jest juz duzy, a to zero — bez seeda pierwszy obrot loop()
// wrzucilby CALA dotychczasowa historie w biezaca godzine i zmyslil pik.
uint32_t gPirBookedRises = 0;
uint32_t gPirTickMs = 0;            // millis() ostatniego doliczenia collectedS

void pirRtcBegin() {
  // Po odlaczeniu pradu RTC zawiera smieci. Bez tej proby wczytalibysmy je jako dane —
  // dlatego rozstrzyga znacznik, a nie "wyglada rozsadnie".
  if (gPir.magic == PIR_RTC_MAGIC) {
    ++gPir.boots;
    LOG("PIR: liczniki z RTC przezyly restart — zbieram od %lu s, start #%lu\n",
        (unsigned long)gPir.collectedS, (unsigned long)gPir.boots);
  } else {
    memset(&gPir, 0, sizeof(gPir));
    gPir.minMs = 0xFFFFFFFF;   // "nie bylo zadnego impulsu"; 0 znaczyloby "impuls 0 ms"
    gPir.boots = 1;
    gPir.magic = PIR_RTC_MAGIC;
    LOG("PIR: RTC puste lub z innego ukladu pol — zimny start, liczniki wyzerowane\n");
  }
  gPirBookedRises = gPir.rises;
  gPirTickMs = millis();
}

// --- LDR: pomiar dlugoterminowy w RTC (v108) — patrz LdrRtc w Log.h ---
// Sam obiekt gLdr stoi na gorze, tuz NAD gPir: jego miejsce w pliku decyduje o ADRESIE
// w RTC i o tym, czy pomiar PIR przezyje te aktualizacje (patrz komentarz przy gPir).
//
// Ile zbocz w gore detektor zdarzen juz "zobaczyl". Osobny licznik od gPirBookedRises, bo
// tamten ksieguje byHour i jest przesuwany w INNYM miejscu petli — wspoldzielenie znaczyloby,
// ze ktokolwiek pierwszy go przesunie, ukradnie drugiemu informacje o ruchu.
// W DRAM (nie w RTC), bo to tylko migawka do liczenia PRZYROSTU: po restarcie seedujemy ja
// z gPir.rises w ldrRtcBegin(). Ruch, ktory wypadl dokladnie na czas restartu, przepada —
// i tak ma byc, bo naprawde go nie widzielismy (patrz nota o uczciwosci pomiaru w Log.h).
uint32_t gLdrBookedRises = 0;
uint32_t gLdrLevelTickMs = 0;   // millis() ostatniego doliczenia ldr_level_s
uint32_t gLdrLevelAccMs = 0;    // reszta ponizej sekundy — bez niej gubilibysmy po ~33 ms
                                // na KAZDY obrot petli 30 fps, czyli prawie caly pomiar

void ldrRtcBegin() {
  if (gLdr.magic == LDR_RTC_MAGIC) {
    ++gLdr.boots;
    LOG("LDR: histogram z RTC przezyl restart — zbieram od %lu s, start #%lu, zdarzen %lu\n",
        (unsigned long)gLdr.collectedS, (unsigned long)gLdr.boots, (unsigned long)gLdr.evCount);
  } else {
    memset(&gLdr, 0, sizeof(gLdr));
    // 0xFFFFFFFF = "zaden slot nie jest otwarty". 0 znaczyloby "otwarty jest slot 0" i
    // pierwsza ciaglosc po zimnym starcie dopisywalaby durS do nieistniejacego zdarzenia.
    gLdr.evSlot = 0xFFFFFFFF;
    gLdr.boots = 1;
    gLdr.magic = LDR_RTC_MAGIC;
    LOG("LDR: RTC puste lub z innego ukladu pol — zimny start, histogram wyzerowany\n");
  }
  // Seed z RTC, nie z zera: po OTA gPir.rises jest juz duze, a gdyby ten licznik startowal
  // od zera, pierwszy obrot petli zobaczylby ROZNICE rowna calej historii ruchu i uznal to
  // za "wlasnie ktos wszedl" — czyli zamknalby kandydata, ktory przezyl aktualizacje.
  gLdrBookedRises = gPir.rises;
  gLdrLevelTickMs = millis();
}

// --- PV: liczniki przyczyn porazek Modbusa, PRZEZYWAJA restart (patrz PvRtc w Log.h) ---
// Prostsza od pirRtcBegin()/ldrRtcBegin(): kazde pole PvRtc to licznik, dla ktorego 0
// uczciwie znaczy "jeszcze sie nie zdarzylo" — w odroznieniu od PIR/LDR nie ma tu zadnego
// pola typu "0xFFFFFFFF = brak danych", wiec po memset nie trzeba nic dodatkowo ustawiac.
void pvRtcBegin() {
  if (gPvRtc.magic == PV_RTC_MAGIC) {
    ++gPvRtc.boots;
    LOG("PV: liczniki przyczyn Modbusa z RTC przezyly restart — zbieram od %lu s, start #%lu\n",
        (unsigned long)gPvRtc.collectedS, (unsigned long)gPvRtc.boots);
  } else {
    memset(&gPvRtc, 0, sizeof(gPvRtc));
    gPvRtc.boots = 1;
    gPvRtc.magic = PV_RTC_MAGIC;
    LOG("PV: RTC puste lub z innego ukladu pol — zimny start, liczniki Modbusa wyzerowane\n");
  }
}

// Krawedzie koszy sa POWTORZONE tekstem w /api/diag (ldr_hist_bins) — tak samo, jak przy
// PIR-ze i z tego samego powodu: ten plik ma juz za soba trzy komentarze, ktore rozjechaly
// sie z kodem, a opis lecacy OBOK DANYCH rozjezdza sie trudniej.
// Bez IRAM_ATTR, w odroznieniu od pirWidthBucket: to wola loop(), nigdy ISR.
uint8_t ldrHistBucket(uint16_t mv) {
  if (mv < 8) return 0;      // oba zgaszone: zmierzone 4-5 mV siedzi TUTAJ
  if (mv < 16) return 1;
  if (mv < 32) return 2;     // "prawdziwa ciemnosc" z v104: 17-26 mV
  if (mv < 64) return 3;
  if (mv < 128) return 4;
  if (mv < 256) return 5;
  if (mv < 384) return 6;    // od tego kosza zaczyna sie strefa sporna; 251 mV ("zmierzch",
                             // ktory v103 wzialo za ciemnosc) siedzi w koszu obok, <256
  if (mv < 512) return 7;
  if (mv < 640) return 8;    // sam prysznic: zmierzone 603-617 mV
  if (mv < 768) return 9;    // dzisiejszy prog LDR_DIM_UP_MV = 650 przecina TEN kosz
  if (mv < 1024) return 10;
  if (mv < 1536) return 11;
  if (mv < 2048) return 12;
  if (mv < 2560) return 13;
  if (mv < 3072) return 14;  // kontroler: zmierzone 2576-3164 rozklada sie na kosze 14 i 15
  return 15;
}

// --- detektor "zostawione swiatlo" — wolany RAZ NA SEKUNDE z loop() ---
// Definicja zdarzenia: ldr_mv >= LDR_EVENT_ON_MV NIEPRZERWANIE i ZERO ruchu PIR, dluzej
// niz LDR_EVENT_MIN_S. Uzasadnienie obu liczb stoi przy stalych w Log.h.
//
// Dlaczego ruch bierzemy z PRZYROSTU gPir.rises, a nie z diag().pirState: pirState jest
// odswiezany co 250 ms i pokazuje POZIOM. Impuls AM312 trwa ~2 s, wiec poziom zlapalby
// wiekszosc ruchow — ale nie ma gwarancji, ze zlapie kazdy, a przy ruchu na granicy okna
// martwego mozna go przegapic. Zbocza liczy ISR i one nie gina. To jest detektor
// dwudziestominutowej CISZY: przegapiony ruch to falszywe zdarzenie, czyli dokladnie ten
// blad, ktorego ta wersja ma nie robic.
void ldrEventTick() {
  const uint32_t rises = gPir.rises;
  const bool motion = (rises != gLdrBookedRises);
  gLdrBookedRises = rises;

  const bool light = diag().ldrMv >= LDR_EVENT_ON_MV;

  if (!light || motion) {
    // Zamkniecie: swiatlo zgaslo albo ktos wszedl. Zdarzenia NIE trzeba nigdzie
    // "dopisywac" — jesli dobilo 20 minut, siedzi juz w pierscieniu z aktualnym durS
    // (dopisywanym co sekunde nizej). Ta kolejnosc jest celowa: zdarzenie jest widoczne
    // w /api/diag JUZ W TRAKCIE trwania, a nie dopiero po zgasnieciu swiatla — czyli
    // wtedy, kiedy jest komukolwiek do czegokolwiek potrzebne.
    if (gLdr.candActive && gLdr.evSlot != 0xFFFFFFFF) {
      LOG("LDR: koniec zdarzenia — swiatlo %s po %lu s\n",
          motion ? "przerwane ruchem" : "zgaszone",
          (unsigned long)(gLdr.collectedS - gLdr.candStartS));
    }
    gLdr.candActive = 0;
    gLdr.evSlot = 0xFFFFFFFF;
    return;
  }

  if (!gLdr.candActive) {
    gLdr.candActive = 1;
    gLdr.candStartS = gLdr.collectedS;
    gLdr.evSlot = 0xFFFFFFFF;

    const time_t tt = time(nullptr);
    // Bez NTP godziny NIE ZNAMY. 0 => /api/diag pokaze null. Ta sama zasada, co przy
    // pir_by_hour: lepiej stracic znacznik czasu, niz zmyslic polnoc.
    gLdr.candStartEpoch = (tt > 1700000000) ? static_cast<uint32_t>(tt) : 0;

    // Migawka "ile temu byl ruch" — liczona TERAZ, bo za 20 minut pirLastAt bedzie juz
    // opisywac inna chwile. pirLastAt to millis(), wiec zeruje sie przy restarcie:
    // 0 znaczy "od startu urzadzenia nie bylo ANI JEDNEGO ruchu", czyli "nie wiem".
    const uint32_t pirAt = diag().pirLastAt;
    gLdr.candLastPirS = (pirAt == 0) ? 0xFFFFFFFF : (millis() - pirAt) / 1000;
  }

  // Dlugosc liczona w collectedS, NIE w epochu — restart pauzuje ciaglosc zamiast
  // ja zrywac i zamiast doliczac czas, ktorego nie widzielismy (patrz LdrRtc).
  const uint32_t dur = gLdr.collectedS - gLdr.candStartS;
  if (dur < LDR_EVENT_MIN_S) return;

  if (gLdr.evSlot == 0xFFFFFFFF) {
    // Ciaglosc wlasnie dobila progu — bierzemy slot. Pierscien nadpisuje NAJSTARSZE.
    gLdr.evSlot = gLdr.evHead;
    gLdr.evHead = (gLdr.evHead + 1) % 8;
    ++gLdr.evCount;
    gLdr.events[gLdr.evSlot].startEpoch = gLdr.candStartEpoch;
    gLdr.events[gLdr.evSlot].lastPirBeforeS = gLdr.candLastPirS;
    LOG("LDR: zdarzenie #%lu — swiatlo %u mV, bez ruchu od %lu s\n",
        (unsigned long)gLdr.evCount, (unsigned)diag().ldrMv,
        (unsigned long)(gLdr.candLastPirS == 0xFFFFFFFF ? 0 : gLdr.candLastPirS) + dur);
  }
  gLdr.events[gLdr.evSlot].durS = dur;
}

// Granice koszy opisane przy PirRtc::widthHist/gapHist w Log.h i powtorzone w /api/diag
// (pir_width_bins/pir_gap_bins) — zeby czytajacy JSON nie musial zgadywac ani ufac
// komentarzowi. IRAM_ATTR jawnie: przy -Os te dwie i tak by sie wkleily, ale nie chce,
// zeby bezpieczenstwo ISR zalezalo od humoru inlinera.
uint8_t IRAM_ATTR pirWidthBucket(uint32_t ms) {
  if (ms < 100) return 0;
  if (ms < 1000) return 1;
  if (ms < 3000) return 2;
  if (ms < 10000) return 3;
  if (ms < 60000) return 4;
  return 5;
}

uint8_t IRAM_ATTR pirGapBucket(uint32_t ms) {
  if (ms < 2000) return 0;
  if (ms < 5000) return 1;
  if (ms < 15000) return 2;
  if (ms < 60000) return 3;
  if (ms < 300000) return 4;
  if (ms < 1800000) return 5;
  return 6;
}

// Liczniki pomiaru ida do gPir (RTC SLOW), pirLastAt zostaje w Diag (millis(), patrz
// komentarze przy obu). gPir jest globalem TEJ jednostki kompilacji, wiec kompilator
// adresuje go absolutnie (l32r + s32i) — tak samo, jak dotad adresowal gDiagIsr.
// Zadnego skoku, zadnego wywolania: struktura dostepu sie NIE zmienila.
void IRAM_ATTR pirIsr() {
  Diag* d = gDiagIsr;
  if (d == nullptr) return;   // przerwanie podpinamy dopiero po ustawieniu wskaznika

  const uint32_t now = millis();
  const bool high = gpio_ll_get_level(GPIO_LL_GET_HW(GPIO_PORT_0), cfg::PIN_PIR) != 0;
  ++gPir.edges;

  if (high) {
    // Zbocze w gore: zaczyna sie impuls. Przerwe liczymy od KONCA poprzedniego, bo to
    // ona jest rozkladem, ktory rozstrzyga "ktos tu jeszcze jest" kontra "wyszedl".
    // gPirFallAt == 0 => to pierwszy impuls od startu (albo pierwszy po restarcie) i nie
    // ma od czego liczyc.
    if (gPirFallAt != 0) ++gPir.gapHist[pirGapBucket(now - gPirFallAt)];
    gPirRiseAt = now;
    ++gPir.rises;      // jednostka rytmu doby; ksieguje ja loop() do gPir.byHour
    d->pirLastAt = now;
    return;
  }

  // Zbocze w dol: domykamy impuls. gPirRiseAt == 0 => pierwsze, co widzimy, to opadanie
  // (czujnik byl juz HIGH, gdy podpinalismy przerwanie, np. rozgrzewka po starcie) —
  // szerokosci nie ma z czego policzyc, wiec liczymy to tylko jako zbocze.
  if (gPirRiseAt == 0) return;
  const uint32_t w = now - gPirRiseAt;   // uint32 na millis(): odejmowanie znosi zawiniecie
  gPirFallAt = now;
  gPirRiseAt = 0;

  ++gPir.pulses;
  gPir.lastMs = w;
  if (w < gPir.minMs) gPir.minMs = w;
  if (w > gPir.maxMs) gPir.maxMs = w;
  gPir.totalMs += w;
  ++gPir.widthHist[pirWidthBucket(w)];
}

// gRadarWantMem/gRadarMemReady usuniete razem z mechanizmem oddawania bufora —
// patrz komentarz przy pobieraniu radaru. releaseBuffer() zostaje: uzywa go OTA
// (tam zwolnienie 129 kB PSRAM ma sens, bo Update alokuje wlasny bufor).
volatile int gWifiAttempt = 0;
char gBootMsg[48] = "Łączenie z WiFi...";

// Ekran "połączono" z adresem IP, pokazywany raz po starcie.
constexpr uint32_t NET_INFO_MS = 10000;
volatile uint32_t gNetInfoUntil = 0;
bool gNetInfoDone = false;
char gIpStr[20] = "0.0.0.0";
volatile int gRssi = 0;

WeatherModel uiWeather{};
PvModel uiPv{};
PvHistory uiHist{};
FlightModel uiFlights{};
AirModel uiAir{};   // kopia dla rdzenia rysujacego — patrz ui.setAir() w setup()

// --- MODELE POSREDNIE dla warstwy rysowania (v126) ---------------------------
// Wszystko powyzej to MIGAWKI modeli, ktore wypelnia warstwa sieciowa. Dwa ekrany
// takiego modelu nie mialy i placily za to tym, ze ich funkcje rysujace podejmowaly
// decyzje o DANYCH:
//
//   W DOMU  — czytal singleton ble:: na zywo, sam szukal nazwy pokoju przez
//             settings().bleFind(), wolal millis() (trzy razy na kafelek, mimo ze
//             nowMs stal w sygnaturze paintFrame) i sam arbitrazowal RSSI miedzy
//             wlasnym radiem a bramka, z progiem swiezosci zaszytym w petli.
//   RADAR   — czytal singleton radarmap::, wybieral klatke animacji, liczyl wektor
//             przesuniecia chmur z wiatru i — najgorsze — PISAL do globalnej
//             diagnostyki diag().radarFrame, z dwoch watkow naraz (ekran na jednym
//             rdzeniu, zrzut BMP na drugim).
//
// Ponizsze dwie struktury domykaja luke: rysowanie dostaje gotowe wiersze i gotowe
// liczby, a wymiana calego interfejsu nie wymaga ruszania tego pliku. Koszt: 196 B
// + 32 B statycznego RAM-u przy barierze 76 000 B — dlatego wiersze niosa WSKAZNIKI
// (nazwa pokoju, raster klatki), a nie kopie.
RoomModel uiRoomModel{};
RadarViewModel uiRadarModel{};

// Zbiera gotowe wiersze ekranu W DOMU. Kolejnosc i filtrowanie sa DOKLADNIE takie,
// jak w petli, ktora stala do v125 w drawViewHome — inaczej kafelki zmienilyby
// kolejnosc na ekranie.
static void buildRoomModel(RoomModel& m, uint32_t nowMs) {
  // Zrodlo starsze niz to nie liczy sie w ogole: lepszy slaby sygnal TERAZ niz
  // swietny sprzed pol godziny. Do v125 byl to goly literal 90000 w srodku petli
  // rysujacej; tutaj ma nazwe, bo to jest regula o danych, nie o pikselach.
  constexpr uint32_t kRssiFreshMs = 90000;

  const int n = ble::count();
  m.sensorCount = static_cast<uint8_t>(n > RoomModel::ROWS ? RoomModel::ROWS : n);
  m.count = 0;

  for (int i = 0; i < n && m.count < RoomModel::ROWS; ++i) {
    const ble::Sensor s = ble::get(i);
    if (!s.valid) continue;

    RoomRow& r = m.rows[m.count];

    // Nazwa: wpis z ustawien, a gdy go nie ma albo jest bez nazwy — MAC czujnika.
    // Slot rzadzi kolorem kafelka i wierszem historii; sloty 6-7 z Settings nie
    // maja ani jednego, ani drugiego, wiec dostaja -1 (wyglad V1 je pomija, V2
    // rysuje je z MAC-iem — obie reguly zostaja w rysowaniu, bo to uklad).
    const Settings::BleCfg* cfg = settings().bleFind(s.mac);
    int slot = -1;
    if (cfg != nullptr) {
      for (int k = 0; k < RoomHistory::ROOMS; ++k) {
        if (&settings().ble[k] == cfg) slot = k;
      }
    }
    r.slot = static_cast<int8_t>(slot);
    // ble::macOf(i), a nie s.mac: `s` to KOPIA, ktora ginie na koncu obrotu petli,
    // a wiersz trzyma sam wskaznik (patrz RoomData.h).
    r.name = (cfg != nullptr && cfg->name[0]) ? cfg->name : ble::macOf(i);

    // Wybieramy LEPSZE ze zrodel, a nie ostatni zapis. Pole `s.rssi` niesie to,
    // co przyszlo ostatnie — dlatego Schody potrafily pokazywac -90 z wlasnego
    // radia, choc bramka slyszy je z -56. Odczyt (temperatura) jest identyczny
    // z obu zrodel, wiec wybor dotyczy wylacznie tego, ktora liczbe pokazac.
    //
    // nowMs, nie millis(): stara wersja pytala zegara osobno dla wlasnego radia,
    // osobno dla bramki i osobno dla wieku probki — trzy rozne "teraz" w jednym
    // kafelku. Roznica jest mikroskopijna, ale to jest dokladnie ten rodzaj
    // niescislosci, przez ktory nie da sie potem odtworzyc, co widzial ekran.
    const bool ownFresh =
        s.rssiOwn != 0 && s.ownAt != 0 && (nowMs - s.ownAt) < kRssiFreshMs;
    const bool gwFresh =
        s.rssiGw != 0 && s.gwAt != 0 && (nowMs - s.gwAt) < kRssiFreshMs;
    int best = 0;
    bool bestGw = false;
    if (ownFresh && gwFresh) {
      bestGw = s.rssiGw > s.rssiOwn;
      best = bestGw ? s.rssiGw : s.rssiOwn;
    } else if (ownFresh) {
      best = s.rssiOwn;
    } else if (gwFresh) {
      best = s.rssiGw;
      bestGw = true;
    }
    r.rssi = static_cast<int16_t>(best);
    r.viaGw = bestGw;

    r.ageS = s.seenAt ? (nowMs - s.seenAt) / 1000 : 9999;
    r.tempC = s.tempC;
    r.humidity = s.humidity;
    r.hasTemp = s.hasTemp;
    r.hasHum = s.hasHum;
    // Przyciecie do 0..100: procent baterii to zwykly bajt ramki, wiec uszkodzona
    // ramka potrafi wstawic 200 — a to w int8_t zrobiloby liczbe UJEMNA. Ujemna
    // bateria na ekranie wyglada na blad kodu, nie na blad transmisji.
    const int bp = s.batteryPct < 0 ? 0 : (s.batteryPct > 100 ? 100 : s.batteryPct);
    r.batteryPct = static_cast<int8_t>(bp);
    r.valid = true;
    ++m.count;
  }
}

// Wybiera klatke radaru, jej wiek i wektor przesuniecia chmur. Kod przeniesiony
// z drawViewRadar bez zmian rachunkowych — razem z komentarzami, bo to one niosa
// wyprowadzenie znaku wektora.
static void buildRadarModel(RadarViewModel& m, const WeatherModel& w, uint32_t nowMs) {
  m = RadarViewModel{};
  m.frames = radarmap::count();
  m.hasRain = radarmap::hasRain();
  m.error = radarmap::lastError();
  // Brak klatek: nie ma czego wybierac i — tak jak przed refaktorem, gdzie
  // drawViewRadar wychodzil przed zapisem — diag().radarFrame zostaje nietkniety.
  if (m.frames == 0) return;

  // Klatka animacji: cyklicznie, z krotka pauza na ostatniej (najnowszej) —
  // inaczej oko nie zdazy zauwazyc, gdzie pada TERAZ.
  const int steps = m.frames + 2;   // 2 dodatkowe "przystanki" na koncu
  const int step = static_cast<int>((nowMs / cfg::RADAR_FRAME_MS) % steps);
  const int fi = step >= m.frames ? m.frames - 1 : step;

  const radarmap::Frame& fr = radarmap::frame(fi);
  m.frameIdx = fi;
  m.frameMin = fr.offsetMin;
  m.frameEpoch = fr.epoch;
  m.raster = radarmap::raster(fi);
  m.rw = radarmap::W;
  m.rh = radarmap::H;

  // Diagnostyka animacji dla /api/diag. JEDYNE miejsce, w ktorym te dwa pola sie
  // zapisuja — do v125 robila to funkcja rysujaca, czyli kod wolany z dwoch watkow.
  diag().radarFrame = fi;
  diag().radarFrameMin = fr.offsetMin;

  // --- wektor przesuniecia chmur MIEDZY klatkami (z wiatru) ---
  // Zamiast trzymac klatke `fi` nieruchomo przez cale RADAR_FRAME_MS, przesuwamy
  // jej PROBKOWANIE stopniowo w kierunku wiatru: przy frameT=0 stoi w miejscu,
  // przy frameT->1 dojezdza o caly wektor (dx,dy) — czyli mniej wiecej tam, gdzie
  // za chwile stanie fi+1. Skok na granicy klatek wychodzi wiec maly, zamiast
  // calego kroku ~24 px naraz (to byla POLOWA problemu "latania"; druga polowa to
  // 7->13 klatek w RadarMap).
  //
  // Liczone RAZ na cykl (nie w petli pikseli!): sin/cos i dzielenie to jedyny
  // "ciezki" rachunek tego ekranu, a budzet calej klatki to 21 ms (gfx.draw_us
  // w /api/diag) przy 19 fps — tego nie wolno ruszyc.
  if (fi + 1 < m.frames) {
    // Najnowsza klatka (fi == frames-1, "teraz") nie ma nastepnej, do ktorej
    // "dojezdzac" — zostaje nieruchoma przez cala pauze na koncu animacji (patrz
    // `steps` wyzej, fi==frames-1 trzyma sie tam przez trzy odslony: ostatni
    // normalny krok + 2 przystanki).
    //
    // Swiezosc pogody: bez tego martwy WiFi (stary odczyt wiatru sprzed godzin, z
    // zupelnie innej sytuacji synoptycznej) animowalby chmury w przypadkowym
    // kierunku. `ageMs` liczone jako signed — ten sam idiom co ago() w Portal.cpp
    // (apiDiag()): (nowMs - okAt) na uint32 przekreca sie po ~49 dniach dzialania,
    // a diag().weatherOkAt pisze netTask, inny watek niz ten. Ujemny wiek
    // traktujemy jako "swiezy", tak samo jak ago().
    const uint32_t okAt = diag().weatherOkAt;
    const int32_t ageMs = static_cast<int32_t>(nowMs - okAt);
    const bool wxFresh =
        okAt != 0 && ageMs < static_cast<int32_t>(2 * cfg::WEATHER_REFRESH_MS);

    // windKmh == 0 (cisza) albo dane nieswieze -> dx=dy=0, czyli shiftX/Y zostaja
    // zerem: zachowanie jak dzis, zwykly skok bez interpolacji. Bezpieczny
    // fallback — nie zgadujemy kierunku, gdy nie mamy z czego.
    if (wxFresh && w.current.windKmh > 0.05f) {
      // v110: skala PRZY MAPIE (gmapr::M_PER_PX w MapDataRadar.h), nie zaszyta tu
      // jako literal. Ta mapa pokrywa ~300 km na 320 px, wiec kazdy piksel to
      // ~937 m (2,7x wiecej niz gmapw/349) — literal kazalby stepPx wyjsc 2,7x za
      // maly, czyli chmury plynelyby 2,7x za szybko wzgledem kolejnej klatki.
      // Klatka co RADAR_MAP_REFRESH_MS (10 min) — liczone ze stalej, a nie z
      // wpisanej na sztywno "10", zeby wzor sam nadazyl, gdyby cykl sie zmienil.
      constexpr float kFrameMin = static_cast<float>(cfg::RADAR_MAP_REFRESH_MS) / 60000.f;

      // Predkosc na wysokosci, na ktorej faktycznie plynie echo (patrz komentarz
      // przy RADAR_FLOW_GAIN w Config.h — to jawne przyblizenie, nie pomiar).
      const float speedKmh = w.current.windKmh * cfg::RADAR_FLOW_GAIN;
      // V[km/h] * czas[h] * 1000[m/km] / (m/px) = przesuniecie w px na klatke.
      // Np. 50 km/h, gain=1: 50 * (10/60) * 1000 / 937,5 = 8,9 px na 10 min.
      const float stepPx = speedKmh * (kFrameMin / 60.f) * 1000.f / gmapr::M_PER_PX;

      // KIERUNEK — wyprowadzenie (najlatwiejsze miejsce na blad znaku):
      // windDir to kierunek, SKAD wieje wiatr (konwencja meteo/Open-Meteo), NIE
      // dokad. Dla azymutu theta od polnocy zgodnie z ruchem wskazowek (0=N, 90=E,
      // 180=S, 270=W) wektor jednostkowy w ukladzie (East,North) to
      // (sin(theta), cos(theta)) — sprawdzenie: theta=90 (E) daje (1,0), czysty
      // wschod. Chmury plyna w kierunku PRZECIWNYM do windDir, czyli azymutem
      // (windDir+180), a sin/cos(x+180) = -sin/cos(x), wiec ruch chmur:
      //   East  = -sin(windDir)
      //   North = -cos(windDir)
      // (to dokladnie standardowy wzor meteo na skladowe wiatru:
      // u = -V*sin(kierunek), v = -V*cos(kierunek)).
      // Na ekranie +x = East (LON rosnie w prawo), ale +y jest W DOL, a North to
      // "w gore" ekranu, wiec North trzeba jeszcze zanegowac:
      //   dx =  East  =  -sin(windDir)
      //   dy = -North = -(-cos(windDir)) = cos(windDir)
      // Ten sam uklad (East=+x, North=-y) uzywa strzalka kierunku lotu w
      // WeatherUi.cpp (tx=x+sin(a)*R, ty=y-cos(a)*R dla `track`, czyli kierunku
      // DOKAD leci samolot) — windDir to `track+180`, stad negacja obu skladowych.
      // Kontrola na dwoch kierunkach:
      //   wiatr z zachodu (windDir=270): dx = -sin(270) = +1 -> na wschod (+x). OK.
      //   wiatr z poludnia (windDir=180): dy =  cos(180) = -1 -> w gore (-y). OK —
      //   wiatr Z poludnia niesie NA polnoc, czyli w gore ekranu.
      const float rad =
          static_cast<float>(w.current.windDir) * static_cast<float>(M_PI) / 180.f;
      const float dx = -sinf(rad) * stepPx;
      const float dy = cosf(rad) * stepPx;

      // `frameT` to INNA faza animacji niz `t` w funkcjach rysujacych: `t` to
      // wejscie na EKRAN radaru (przejscie miedzy widokami), `frameT` to pozycja
      // WEWNATRZ pojedynczej klatki radaru — nie mylic.
      const float frameT = static_cast<float>(nowMs % cfg::RADAR_FRAME_MS) /
                           static_cast<float>(cfg::RADAR_FRAME_MS);
      // int16_t wystarcza z ogromnym zapasem: przy 200 km/h stepPx to ~38 px.
      m.shiftX = static_cast<int16_t>(lroundf(dx * frameT));
      m.shiftY = static_cast<int16_t>(lroundf(dy * frameT));
    }
  }
}

AlertKind lastAlertKind = AlertKind::NONE;
// Rozmiar z wartownika enuma, nie z literalu. Zaszyta osemka byla DOKLADNIE rowna
// liczbie pozycji AlertKind — czyli tablica wygladala na zapasowa, a byla pelna.
uint32_t lastAlertAt[static_cast<int>(AlertKind::COUNT)] = {};

static void setBootMsg(const char* m) {
  strncpy(gBootMsg, m, sizeof(gBootMsg) - 1);
  gBootMsg[sizeof(gBootMsg) - 1] = '\0';
}

// ----------------------------------------------------------------- WiFi/NTP --

static bool connectWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }
  if (!settings().hasWifi()) {
    return false;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  // Domyslnie ESP32 laczy sie z PIERWSZYM punktem, ktory odpowie na dana nazwe
  // (WIFI_FAST_SCAN) — a nie z najmocniejszym. Przy dwoch punktach o tej samej
  // nazwie potrafilo to zlapac ten dalszy: w logach mielismy raz -49 dBm, raz
  // -78 dBm w tym samym miejscu. Kazemy przeskanowac WSZYSTKIE kanaly i wybrac
  // najsilniejszy sygnal. Kosztuje to ~1 s dluzszego laczenia przy starcie.
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);

  WiFi.begin(settings().ssid, settings().pass);
  ++gWifiAttempt;
  setBootMsg("Łączenie z WiFi...");

  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 12000) {
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  // Uzbrajamy sprzetowy prog. Zdarzenie jest JEDNORAZOWE — po kazdym wyzwoleniu
  // trzeba je uzbroic ponownie (robimy to po probie przenosin).
  esp_wifi_set_rssi_threshold(kRssiRoamBelow);

  strncpy(gIpStr, WiFi.localIP().toString().c_str(), sizeof(gIpStr) - 1);
  gRssi = WiFi.RSSI();
  ++diag().wifiConnects;
  LOG("WiFi OK, IP: %s (%d dBm), heap %lu\n", gIpStr, gRssi,
      (unsigned long)ESP.getFreeHeap());

  if (!gNetInfoDone) {
    gNetInfoDone = true;
    gNetInfoUntil = millis() + NET_INFO_MS;
  }

  setBootMsg("Synchronizacja czasu...");
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();
  for (int i = 0; i < 40 && time(nullptr) < 1700000000; ++i) {
    vTaskDelay(pdMS_TO_TICKS(250));
  }
  portal::beginSta();
  return true;
}

// ------------------------------------------------------------ zadanie sieci --

static void netTask(void*) {
  uint32_t nextWeatherAt = 0;
  uint32_t nextPvAt = 0;
  uint32_t nextFlightAt = 0;
  uint32_t nextOtaAt = 45000;   // pierwsze sprawdzenie po 45 s od startu
  uint32_t nextStoreAt = 0;
  uint32_t nextRadarAt = 0;
  uint32_t nextBleAt = 20000;  // po WiFi i pierwszej pogodzie
  uint32_t nextGwAt = 12000;
  uint32_t nextRoomSaveAt = 0;
  uint32_t nextRoamAt = 120000;   // pierwszy przeglad po 2 min od startu
  uint32_t nextRadarMapAt = 25000;
  uint32_t nextViAt = 35000;
  uint32_t nextAirAt = 5000;   // jakosc powietrza (v117) — pierwsza proba wczesnie,
                               // ale i tak odpuszcza, dopoki NTP nie da czasu (patrz nizej)
  bool firstWeather = false;

  for (;;) {
    // Okres próbny po OTA. MUSI tykać w każdej iteracji — także wtedy, gdy nie ma
    // WiFi — bo brak sieci to jeden z powodów, dla których wersję trzeba cofnąć.
    // Dlatego stoi PRZED wszystkimi "continue" poniżej.
    otaGuardTick();

    // ---- polnoc: profile doby (palnik I fotowoltaika) przestaja byc "dzis" ----
    // MUSI tykac niezaleznie od odpytu pieca i falownika — i dlatego stoi TU, przed
    // wszystkimi "continue" ponizej, obok otaGuardTick(). Bez WiFi tez.
    //
    // OBIE historie mialy DOKLADNIE ten sam blad, bo maja dokladnie ta sama budowe:
    // reset() jest wolany WYLACZNIE z push(), a push() leci tylko po UDANYM fetchu.
    // Nie ma fetchu — nie ma push() — nie ma resetu — `day` zostaje wczorajszy,
    // a 144 sloty trzymaja wczorajsze slupki. Oba wykresy sa podpisane "DZIS"
    // (WeatherUi.cpp:575 — stopka wykresu PV, WeatherUi.cpp:2313 — "PRACA PALNIKA
    // DZIŚ") i ZADEN z nich nie sprawdza doby. Ufaja tym strukturom.
    //
    // PALNIK: profil przezywa restart w NVS, wiec restart o 00:10 odtwarzal profil
    // WCZORAJSZY z wczorajszym `day`. Do pierwszego udanego odpytu — minutami.
    // A gdy piec milczy (token wygasl, API lezy, nie ma WiFi) — godzinami.
    //
    // FOTOWOLTAIKA: gorzej, bo tu nie trzeba ZADNEGO restartu. pvClient.fetch()
    // zwraca ok == false przez cala noc (Huawei wylacza Modbus TCP po zachodzie —
    // patrz PvModel::asleep), wiec: asleep => ok == false => push nie leci => reset
    // nie leci. Co noc, od polnocy do pierwszego udanego odczytu po wschodzie
    // slonca, ekran fotowoltaiki pokazywal WCZORAJSZA krzywa pod napisem "DZIS".
    // Zima to 8 godzin dziennie. To nie byl blad restartu, to bylo codzienne
    // klamstwo. Store co 5 min utrwalal je jeszcze do NVS.
    //
    // DLACZEGO KASUJEMY, A NIE PODPISUJEMY "WCZORAJ": pusty wykres o 03:00 jest
    // PRAWDZIWY — dzisiejsza produkcja naprawde wynosi wtedy zero. Wczorajsza
    // krzywa podpisana "dzis" to nieprawda, nawet jesli ladniejsza od pustki.
    // Ta sama klasa bledu, co "wilgotnosc pokazana jako temperatura".
    //
    // Warunki, obydwa istotne (dla obu historii tak samo):
    //   day >= 0        — nie bylo jeszcze ZADNEGO profilu, nie ma czego kasowac;
    //   day != tm_yday  — profil odtworzony z NVS w TEJ SAMEJ dobie zostaje
    //                     nietkniety, po to go zapisujemy (kapiel 8:00, restart
    //                     14:00, slupek ma byc).
    // Zegar musi byc pewny (NTP), inaczej skasowalibysmy dobry profil na podstawie
    // daty z 1970. Sam reset to 144 sloty w RAM — wolno go zrobic pod gLock, bo to
    // ani siec, ani NVS.
    //
    // GasHistory (gGas) NIE jest tu ruszana i to jest swiadome: to bufor 120 dni,
    // ktory przewija sie sam przez advance(epoch) zerujac przespane doby, a jego
    // `head` zawsze wskazuje wlasciwy dzien, bo push() leci tylko tuz po udanym
    // advance(). Nie ma tam "dzis", ktore moglo by sklamac — czyta go wylacznie
    // sumBetween() po jawnym zakresie dat.
    {
      const time_t tt = time(nullptr);
      if (tt > 1700000000) {
        struct tm tmv{};
        localtime_r(&tt, &tmv);
        xSemaphoreTake(gLock, portMAX_DELAY);
        const bool burnerRolled = (gBurner.day >= 0 && gBurner.day != tmv.tm_yday);
        if (burnerRolled) {
          gBurner.reset(tmv.tm_yday);
        }
        const bool pvRolled = (gHist.day >= 0 && gHist.day != tmv.tm_yday);
        if (pvRolled) {
          gHist.reset(tmv.tm_yday);
        }
        xSemaphoreGive(gLock);
        // Pusty profil trafi do NVS przy najblizszym zapisie (co 5 min, czyli o 00:05)
        // i tak ma byc: nowa doba NAPRAWDE jest pusta. Guard `snap.day >= 0` przy
        // zapisie tego nie blokuje i nie ma blokowac — po reset(tm_yday) `day` jest
        // >= 0, bo profil ISTNIEJE, tylko jest (zgodnie z prawda) pusty. Ten guard
        // odsiewa co innego: day == -1, czyli "nie bylo jeszcze ANI JEDNEGO odczytu",
        // kiedy nadpisanie NVS skasowaloby profil odtworzony przy starcie.
        // Wczorajszej doby i tak nie umiemy pokazac — obie struktury trzymaja jedna.
        if (burnerRolled) {
          LOG("Piec: nowa doba — profil palnika wyzerowany (bez czekania na odpyt)");
        }
        if (pvRolled) {
          LOG("PV: nowa doba — profil produkcji wyzerowany (bez czekania na falownik)");
        }
      }
    }

    if (!settings().hasWifi()) {
      gBooting = false;
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    // Panel testuje wlasnie nowe dane WiFi — nie wchodzimy mu w droge. Nasze
    // WiFi.begin() poszloby ze STARYMI danymi z NVS, zabiloby jego probe, a panel
    // uznalby powrot na stara siec za dowod, ze nowe haslo dziala. Flaga wygasa
    // sama (portal::wifiConfigBusy), wiec nie da sie tu zawisnac na stale.
    if (portal::wifiConfigBusy()) {
      gWifiOk = (WiFi.status() == WL_CONNECTED);
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }

    if (WiFi.status() != WL_CONNECTED) {
      gWifiOk = false;
      if (!connectWifi()) {
        vTaskDelay(pdMS_TO_TICKS(cfg::WIFI_RETRY_MS));
        continue;
      }
    }
    gWifiOk = true;
    const uint32_t now = millis();

    // ---- MQTT: utrzymanie sesji + telemetria urzadzenia ----
    // Brak brokera nie moze zatrzymac reszty: proby laczenia maja krotki timeout
    // i backoff, wiec ta linijka albo kosztuje mikrosekundy, albo raz na jakis
    // czas ~2 s (nieudany TCP connect). Nigdy nie blokuje na dluzej.
    mqttha::loop();

    // ---- pogoda ----
    if (static_cast<int32_t>(now - nextWeatherAt) >= 0) {
      if (!firstWeather) {
        setBootMsg("Pobieram prognozę...");
      }
      WeatherModel tmp{};
      if (weatherClient.fetch(tmp)) {
        xSemaphoreTake(gLock, portMAX_DELAY);
        // radar ma własny cykl — nie kasuj go świeżą prognozą
        tmp.radarLevel = gWeather.radarLevel;
        tmp.radarAgeSec = gWeather.radarAgeSec;
        tmp.radarValid = gWeather.radarValid;
        gWeather = tmp;
        xSemaphoreGive(gLock);
        nextWeatherAt = millis() + cfg::WEATHER_REFRESH_MS;
        firstWeather = true;
        diag().weatherOkAt = millis();
        diag().weatherErr[0] = '\0';
        LOG("Pogoda OK: %.1f C, kod %d, heap %lu\n", tmp.current.tempC,
            tmp.current.weatherCode, (unsigned long)ESP.getFreeHeap());
        mqttha::publishWeather(tmp);
      } else {
        strncpy(diag().weatherErr, tmp.errorMsg, sizeof(diag().weatherErr) - 1);
        LOG("Pogoda BLAD: %s (heap %lu)\n", tmp.errorMsg, (unsigned long)ESP.getFreeHeap());
        nextWeatherAt = millis() + 30000;
      }
    }

    // ---- jakosc powietrza (ARMAAG/sensorbox, GA17 z zapasem GA24 — patrz AirClient.h) ----
    // Co ~15 min: dane to srednie GODZINOWE (nowa probka raz na godzine), wiec czesciej
    // pytalibysmy cudzy serwer bez ANI JEDNEJ nowej wartosci do pokazania.
    if (static_cast<int32_t>(now - nextAirAt) >= 0) {
      const time_t ntpNow = time(nullptr);
      if (ntpNow <= 1700000000) {
        // Bez czasu z NTP zapytanie nie ma sensu (patrz AirClient::fetch — sam tez
        // to sprawdza i odmowi). To NIE jest porazka sieci, wiec nie liczymy tego
        // jako blad ani nie czekamy pelne 15 minut — probujemy ponownie za chwile,
        // gdy tylko zegar bedzie gotowy (zwykle sekundy po polaczeniu WiFi).
        nextAirAt = millis() + 5000;
      } else {
        AirModel tmp{};
        if (airClient.fetch(tmp)) {
          xSemaphoreTake(gLock, portMAX_DELAY);
          gAir = tmp;
          xSemaphoreGive(gLock);
          nextAirAt = millis() + cfg::AIR_REFRESH_MS;
          diag().airOkAt = millis();
          diag().airErr[0] = '\0';
          diag().airFallback = tmp.usingFallback;
          strncpy(diag().airStation, tmp.stationName, sizeof(diag().airStation) - 1);
          diag().airHasPm10 = tmp.hasPm10;
          diag().airPm10 = tmp.pm10;
          diag().airHasPm25 = tmp.hasPm25;
          diag().airPm25 = tmp.pm25;
          diag().airIndex = tmp.index;
          diag().airSampleEpoch = tmp.sampleEpoch;
          LOG("Powietrze OK: %s%s PM10=%.1f PM2.5=%.1f idx=%d (%s)\n", tmp.stationName,
              tmp.usingFallback ? " [ZAPAS]" : "", tmp.pm10, tmp.pm25, tmp.index,
              airIndexName(tmp.index));
        } else {
          // Blad sieci ALBO obie stacje bez swiezych danych — w obu przypadkach NIE
          // dotykamy gAir: ostatnia (nadal prawdziwa) probka ma zostac na ekranie,
          // dokladnie jak przy pogodzie/PV wyzej. "Wiek danych" na ekranie sam
          // powie, jak bardzo sie zestarzala — to uczciwsze niz czarny ekran.
          strncpy(diag().airErr, tmp.errorMsg, sizeof(diag().airErr) - 1);
          LOG("Powietrze BLAD: %s\n", tmp.errorMsg);
          nextAirAt = millis() + 30000;
        }
      }
    }

    // ---- fotowoltaika ----
    if (static_cast<int32_t>(now - nextPvAt) >= 0) {
      // Czy falownikowi wolno teraz spać? Godziny wschodu/zachodu mamy z prognozy.
      char sunrise[6], sunset[6];
      xSemaphoreTake(gLock, portMAX_DELAY);
      memcpy(sunrise, gWeather.sunrise, sizeof(sunrise));
      memcpy(sunset, gWeather.sunset, sizeof(sunset));
      xSemaphoreGive(gLock);
      const bool maySleep = pvMayBeAsleep(sunrise, sunset, localMinutesNow());

      PvModel tmp{};
      const bool ok = pvClient.fetch(tmp, maySleep);
      xSemaphoreTake(gLock, portMAX_DELAY);
      gPv = tmp;
      if (ok) {
        const time_t t = time(nullptr);
        if (t > 1700000000) {
          struct tm tmv{};
          localtime_r(&t, &tmv);
          const int prevDay = gHist.day;
          gHist.push(tmv.tm_yday, tmv.tm_hour, tmv.tm_min, tmp.data.powerAcW,
                     tmp.data.houseLoadW);
          if (prevDay != gHist.day) {
            // nowy dzień — profil wyczyszczony, zapisz od razu
            nextStoreAt = 0;
          }
        }
      }
      xSemaphoreGive(gLock);

      // Śpiący falownik odpytujemy co 5 minut zamiast co 30 s. Gdy tylko odpowie
      // (a potrafi odpowiadać jeszcze po zachodzie), wracamy do normalnego tempa.
      nextPvAt = millis() + (tmp.asleep ? cfg::PV_REFRESH_NIGHT_MS : cfg::PV_REFRESH_MS);

      if (ok) {
        diag().pvOkAt = millis();
        diag().pvErr[0] = '\0';
        diag().pvAsleep = false;
        LOG("PV: AC=%ldW DC=%ldW siec=%ldW dzis=%.2fkWh st=0x%04X\n",
                      static_cast<long>(tmp.data.powerAcW),
                      static_cast<long>(tmp.data.powerDcW),
                      static_cast<long>(tmp.data.gridPowerW), tmp.data.energyTodayKwh,
            tmp.data.statusCode);
      } else if (tmp.asleep) {
        // Stan neutralny: to nie jest błąd, więc nie zaśmiecamy nim diagnostyki
        // ani ekranu statystyk (kropka szara, nie czerwona).
        if (!diag().pvAsleep) {
          LOG("PV: falownik usypia (po zachodzie) — odpytuje co %lu min\n",
              static_cast<unsigned long>(cfg::PV_REFRESH_NIGHT_MS / 60000));
        }
        diag().pvErr[0] = '\0';
        diag().pvAsleep = true;
      } else {
        strncpy(diag().pvErr, tmp.errorMsg, sizeof(diag().pvErr) - 1);
        diag().pvAsleep = false;
        LOG("PV BLAD: %s\n", tmp.errorMsg);
      }
      // Do HA leci takze nieudany odczyt — jako moce 0 W i status "Offline"
      // (liczniki energii zostaja na ostatniej znanej wartosci, patrz MqttClient.cpp).
      mqttha::publishPv(tmp, ok);
    }

    // ---- radar opadowy (realny pomiar; model bywa ślepy na lokalne ulewy) ----
    if (static_cast<int32_t>(millis() - nextRadarAt) >= 0) {
      // USUNIETE: proszenie rdzenia 1 o oddanie bufora ekranu przed dekodowaniem PNG.
      //
      // Ten mechanizm zamrazal ekran na CALY czas pobierania radaru (nie na 3 s —
      // 3000 ms to bylo tylko czekanie na potwierdzenie z rdzenia 1; `fetch()` leci
      // dopiero potem, a `gRadarWantMem` wraca na false po nim). Co 5 minut, bo
      // warunek `getMaxAllocHeap() < 48000` jest na tym urzadzeniu spelniony ZAWSZE
      // (zmierzone: largest_block ~39-43 kB).
      //
      // I nie mogl pomoc, bo mierzyl co innego, niz zwalnial:
      //   - getMaxAllocHeap() to MALLOC_CAP_INTERNAL, czyli SRAM,
      //   - a bufor sprite'a (129 kB) siedzi w PSRAM. Nie przez TFT_eSPI — jego
      //     `#if defined(CONFIG_SPIRAM_SUPPORT)` jest FALSZYWY, bo rdzen 3.3.10
      //     definiuje CONFIG_SPIRAM, nie CONFIG_SPIRAM_SUPPORT. Trafia tam przez
      //     CONFIG_SPIRAM_USE_MALLOC=1 z CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096:
      //     kazda alokacja > 4 kB idzie do PSRAM sama.
      // Czyli: oddawalismy PSRAM, zeby zrobic miejsce w SRAM-ie.
      //
      // Prawdziwy straznik jest w RadarClient (prog 64 kB, licznik radar.skips_low_ram)
      // i na urzadzeniu pokazuje ZERO — dekoder nigdy nie odpuscil z braku pamieci.
      RadarSnapshot rs{};
      const bool radarOk = radarClient.fetch(rs);

      if (radarOk) {
        xSemaphoreTake(gLock, portMAX_DELAY);
        gWeather.radarLevel = rs.level;
        gWeather.radarAgeSec = rs.ageSec;
        gWeather.radarValid = true;
        xSemaphoreGive(gLock);
        diag().radarOkAt = millis();
        diag().radarLevel = rs.level;
        diag().radarAgeSec = rs.ageSec;
        diag().radarErr[0] = '\0';
        nextRadarAt = millis() + cfg::RADAR_REFRESH_MS;
      } else {
        strncpy(diag().radarErr, rs.errorMsg, sizeof(diag().radarErr) - 1);
        LOG("Radar BLAD: %s\n", rs.errorMsg);
        nextRadarAt = millis() + 60000;
      }
    }

    // ---- animowana mapa opadow (7 kafelkow, 2 h wstecz) ----
    // Idzie PO radarze punktowym i PRZED lotami: sciaga 7 obrazkow, wiec nie chcemy
    // tego robic czesto ani rownolegle z niczym ciezkim.
    if (radarmap::wantsFetch() || static_cast<int32_t>(millis() - nextRadarMapAt) >= 0) {
      if (radarmap::fetch()) {
        nextRadarMapAt = millis() + cfg::RADAR_MAP_REFRESH_MS;
      } else {
        LOG("Radar mapa BLAD: %s", radarmap::lastError());
        nextRadarMapAt = millis() + 120000;
      }
    }

    // ---- piec Viessmann (chmura ViCare) ----
    // Co 3 minuty = 480 zapytan na dobe. Limit Viessmanna to 1450/dobe i 120 na
    // 10 minut, wiec mamy zapas takze na odswiezanie tokena (co ~55 min).
    if (settings().hasViessmann() && static_cast<int32_t>(millis() - nextViAt) >= 0) {
      vi::Model tmp{};
      const bool ok = vi::fetch(tmp);
      bool gasWantSave = false;
      xSemaphoreTake(gLock, portMAX_DELAY);
      if (ok) {
        gVi = tmp;
        diag().viOkAt = millis();
        diag().viErr[0] = '\0';
        diag().viDhwC = tmp.dhwTempC;
        diag().viSupplyC = tmp.supplyTempC;

        // Surowe liczniki do /api/diag (patrz Log.h). Przepisujemy je BEZ zadnej
        // obrobki — cala wartosc tych pol polega na tym, ze pokazuja to, co
        // naprawde przyszlo z pieca.
        // Kazda liczba pod WLASNA flaga: `hours` i `starts` to osobne wlasciwosci
        // i moga przyjsc niezaleznie. Przepisujemy tylko to, co naprawde doszlo —
        // inaczej polowiczna odpowiedz zostawialaby w diagnostyce ostatnia dobra
        // wartosc podpisana swieza flaga.
        diag().viHasBurnerHours = tmp.hasBurnerHours;
        diag().viHasBurnerStarts = tmp.hasBurnerStarts;
        if (tmp.hasBurnerHours) diag().viBurnerHours = tmp.burnerHours;
        if (tmp.hasBurnerStarts) diag().viBurnerStarts = tmp.burnerStarts;
        diag().viBurnerActive = tmp.burnerActive;
        // "Ostatnia ZLAPANA": nadpisujemy tylko, gdy modulacja faktycznie doszla.
        // Brakujaca cecha zapisalaby tu zero, czyli "palnik stoi" — a to jest dokladnie
        // to klamstwo, ktore mamy zmierzyc, a nie powielic.
        if (tmp.hasModulation) diag().viModulation = tmp.modulationPct;
        diag().viHasGas = tmp.hasGas;
        if (tmp.hasGas) diag().viGasDayM3 = tmp.gasDhwM3 + tmp.gasHeatM3;

        // Profil doby palnika + wlasny log zuzycia gazu. Log jest wlasny, bo
        // liczniki miesieczne/roczne pieca sa zepsute (currentMonth < lastSevenDays,
        // currentYear = 5.3 m3 po 4 latach) — ufamy tylko currentDay.
        const time_t tt = time(nullptr);
        if (tt > 1700000000) {
          struct tm tmv{};
          localtime_r(&tt, &tmv);
          // Do historii wpisujemy TYLKO to, co model potwierdza flaga. Bez tego
          // brak cechy "heating.burners.0" w odpowiedzi API zapisywal "palnik nie
          // pracowal" w slocie, w ktorym pracowal — a to jest zapis TRWALY, ktory
          // potem klamie na wykresie doby. Zero jest tu nieodroznialne od pomiaru.
          if (tmp.hasBurnerState) {
            gBurner.push(tmv.tm_yday, tmv.tm_hour, tmv.tm_min,
                         tmp.hasModulation ? tmp.modulationPct : 0, tmp.burnerActive);
          }
          if (tmp.hasGas) {
            const uint32_t prevDay = gGas.lastDay;
            if (gGas.advance(static_cast<uint32_t>(tt))) {
              gGas.push(tmp.gasDhwM3 + tmp.gasHeatM3);
              // Zapis RAZ NA DOBE, przy przewinieciu dnia — nie co 3 minuty.
              // Doba wlasnie sie zamknela, wiec jej suma jest juz ostateczna.
              // Snapshot na stos (248 B) i zapis POZA mutexem: NVS nigdy pod gLock.
              if (prevDay != 0 && gGas.lastDay != prevDay) gasWantSave = true;
            }
          }
        }
      } else {
        gVi.valid = false;
        snprintf(diag().viErr, sizeof(diag().viErr), "%s", tmp.err);
        LOG("Piec BLAD: %s", tmp.err);
      }

      // Snapshot pod mutexem, zapis POZA nim — NVS nigdy pod gLock (rdzen 1
      // renderuje 30 klatek/s i czeka na te sama blokade).
      GasHistory gasSnap;
      if (gasWantSave) gasSnap = gGas;
      xSemaphoreGive(gLock);
      if (gasWantSave) {
        gasHistorySave(gasSnap);
        LOG("Gaz: zamknieta doba, log zapisany");
      }
      nextViAt = millis() + (ok ? 180000UL : 120000UL);
    }

    // ---- bramka BLE (Shelly slyszy to, czego my nie slyszymy) ----
    // Co 20 s, bo to tanie: jeden GET po WiFi, kilkaset bajtow. Nasze wlasne radio
    // dziala dalej — bramka tylko dokłada uszu, nie zastepuje ich.
    if (settings().bleGwHost[0] != '\0' && static_cast<int32_t>(millis() - nextGwAt) >= 0) {
      blegw::poll();
      nextGwAt = millis() + 20000;
    }

    // ---- nasłuch czujników BLE (Xiaomi) ----
    // Skanujemy z przerwami, a nie ciągle: radio 2,4 GHz jest jedno i dzieli je
    // WiFi. Czujnik nadaje co kilka-kilkanaście sekund, więc 6 s nasłuchu co
    // 45 s spokojnie wystarczy, a WiFi (panel, OTA, MQTT) tego nie odczuwa.
    if (ble::ready() && static_cast<int32_t>(now - nextBleAt) >= 0) {
      ble::scan(4);
      mqttha::publishBle();

      // Historia 24 h: przewijamy okno i dopisujemy biezace odczyty.
      const time_t tt = time(nullptr);
      xSemaphoreTake(gLock, portMAX_DELAY);
      if (gRooms.advance(static_cast<uint32_t>(tt))) {
        for (int i = 0; i < ble::count(); ++i) {
          const ble::Sensor bs = ble::get(i);
          if (!bs.valid) continue;
          const Settings::BleCfg* bc = settings().bleFind(bs.mac);
          if (bc == nullptr) continue;
          // ROOMS, nie "4" — Settings ma osiem slotow BLE, historia szesc.
          // Zaszyta czworka oznaczala, ze piaty czujnik dalo sie skonfigurowac
          // w panelu, ale nie trafial ani do historii, ani na ekran.
          for (int k = 0; k < RoomHistory::ROOMS; ++k) {
            if (&settings().ble[k] == bc) {
              gRooms.push(k, bs.hasTemp, bs.tempC);
            }
          }
        }
        if (static_cast<int32_t>(millis() - nextRoomSaveAt) >= 0) {
          RoomHistory snap = gRooms;
          xSemaphoreGive(gLock);
          roomHistorySave(snap);          // NVS poza mutexem — zapis trwa
          nextRoomSaveAt = millis() + 600000;
          xSemaphoreTake(gLock, portMAX_DELAY);
        }
      }
      xSemaphoreGive(gLock);

      nextBleAt = millis() + 20000;
    }

    // ---- roaming: czy nie wisimy na slabszym punkcie dostepowym? ----
    // ESP32 sam NIE przechodzi miedzy punktami — raz zlapany trzyma do konca.
    // Gdy sygnal jest kiepski, skanujemy i przenosimy sie na wyraznie lepszy.
    // Warunek "wyraznie" (>= 8 dB) chroni przed skakaniem tam i z powrotem.
    const bool roamNow = gWifiOk && (gRoamWanted ||
                                     static_cast<int32_t>(now - nextRoamAt) >= 0);
    if (roamNow) {
      gRoamWanted = false;
      nextRoamAt = millis() + 180000;
      const int cur = WiFi.RSSI();
      if (cur < kRssiRoamBelow - 1) {
        // Bufor wynikow skanu jest w rdzeniu JEDEN i wspolny z apiScan ("Wyszukaj
        // sieci" w panelu), a scanNetworks() zaczyna od jego zwolnienia. Blokada musi
        // objac skan, odczyt wynikow i scanDelete() — inaczej klikniecie skanu w tej
        // chwili wywracalo urzadzenie. To, co potrzebne dalej (BSSID, kanal),
        // kopiujemy do zmiennych lokalnych JESZCZE pod blokada; samo przelaczanie
        // (disconnect + begin, kilka sekund) idzie juz bez niej.
        uint8_t bssid[6] = {};
        int32_t ch = 0;
        int bestRssi = 0;
        bool move = false;
        const bool scanned = portal::scanLock(20000);
        if (scanned) {
          const int found = WiFi.scanNetworks(false, false, false, 250);
          int best = -1;
          bestRssi = cur + kRssiRoamGain;
          for (int i = 0; i < found; ++i) {
            if (WiFi.SSID(i) != settings().ssid) continue;
            if (WiFi.RSSI(i) > bestRssi) {
              bestRssi = WiFi.RSSI(i);
              best = i;
            }
          }
          if (best >= 0) {
            memcpy(bssid, WiFi.BSSID(best), 6);
            ch = WiFi.channel(best);
            move = true;
          }
          WiFi.scanDelete();
          portal::scanUnlock();
        }

        if (!scanned) {
          LOG("WiFi: skan zajety przez panel — przeglad roamingowy pomijam");
        } else if (move) {
          LOG("WiFi: przenosze sie na mocniejszy punkt (%d -> %d dBm)", cur, bestRssi);
          WiFi.disconnect();
          WiFi.begin(settings().ssid, settings().pass, ch, bssid);
          for (int i = 0; i < 60 && WiFi.status() != WL_CONNECTED; ++i) {
            vTaskDelay(pdMS_TO_TICKS(100));
          }
          diag().wifiRoams++;
          LOG("WiFi: po przenosinach %d dBm", WiFi.RSSI());
        } else {
          LOG("WiFi: sygnal %d dBm, ale nic lepszego nie ma — zostajemy", cur);
        }
      }
      // Prog jest jednorazowy: po wyzwoleniu trzeba go uzbroic od nowa.
      esp_wifi_set_rssi_threshold(kRssiRoamBelow);
    }

    // ---- ile stosu naprawde zuzywaja zadania (do przyciecia 2 x 16 kB) ----
    if (gNetTask != nullptr) {
      diag().stackNet = uxTaskGetStackHighWaterMark(gNetTask) * sizeof(StackType_t);
    }
    if (gWebTask != nullptr) {
      diag().stackWeb = uxTaskGetStackHighWaterMark(gWebTask) * sizeof(StackType_t);
    }

    // ---- zapis profili doby do NVS: produkcja PV + palnik ----
    // Oba profile jada na jednym zegarze (co 5 min), bo maja identyczny problem:
    // trzymaja JEDNA dobe, ktora jest kasowana o polnocy, wiec jedyne, co moze je
    // uratowac przed restartem, to zapis W TRAKCIE doby.
    //
    // DLACZEGO PALNIK NIE ZAPISUJE SIE "RAZ NA DOBE PRZY ZMIANIE day", JAK GAZ:
    // bo dla tej struktury to nie dziala. GasHistory to bufor 120 dni — zamknieta
    // doba zostaje w nim na zawsze, wiec zapis na przewinieciu dnia utrwala gotowa
    // sume. BurnerHistory ma tylko biezaca dobe i BurnerHistory::reset() zeruje
    // wszystkie 144 sloty przy zmianie tm_yday. Zapis dokladnie w tym momencie
    // wpisalby do NVS PUSTY profil, a restart o 14:00 odtworzylby z niego zero —
    // czyli ten sam objaw, ktory naprawiamy ("wykres pieca nie pamieta po resecie").
    // PvHistory ma to samo ograniczenie i z tego samego powodu zapisuje sie co 5 min.
    // To jest CALY powod, dla ktorego "fotowoltaika pamieta, a piec nie".
    //
    // Koszt: 296 B blobu (struktura na stosie ma 292 — reszta to `ver` i wyrownanie)
    // co 5 min, obok istniejacych 584 B (PV) w tym samym takcie i
    // 1736 B co 10 min (pokoje). NVS jest wear-levelowane, a to najmniejszy z tych
    // trzech zapisow — nie zmienia rzedu wielkosci zuzycia.
    if (static_cast<int32_t>(now - nextStoreAt) >= 0) {
      // Osobne zakresy: dwa snapshoty (724 B + 292 B) nie musza zyc naraz na stosie
      // netTask. Zapis ZAWSZE poza gLock — rdzen 1 czeka na te sama blokade z klatka.
      {
        xSemaphoreTake(gLock, portMAX_DELAY);
        PvHistory snap = gHist;
        xSemaphoreGive(gLock);
        if (snap.day >= 0) {
          pvHistorySave(snap);
        }
      }
      {
        xSemaphoreTake(gLock, portMAX_DELAY);
        BurnerHistory snap = gBurner;
        xSemaphoreGive(gLock);
        // day < 0 = nie bylo jeszcze ANI JEDNEGO odczytu pieca (piec wylaczony,
        // brak autoryzacji, same bledy). Pusty profil nadpisalby ten z NVS
        // i skasowal to, co wlasnie odtworzylismy przy starcie.
        if (snap.day >= 0) {
          burnerHistorySave(snap);
        }
      }
      nextStoreAt = millis() + cfg::PV_STORE_MS;
    }

    // ---- samoloty ----
    if (gFlightsNeeded && static_cast<int32_t>(millis() - nextFlightAt) >= 0) {
      FlightModel tmp{};
      if (flightClient.fetch(tmp)) {
        xSemaphoreTake(gLock, portMAX_DELAY);
        gFlights = tmp;
        xSemaphoreGive(gLock);
        nextFlightAt = millis() + cfg::FLIGHT_REFRESH_MS;
        diag().flightOkAt = millis();
        diag().flightsTotal = tmp.total;
        diag().flightErr[0] = '\0';
        LOG("Loty: %d w zasiegu, na liscie %d\n", tmp.total, tmp.count);
      } else {
        nextFlightAt = millis() + 20000;
        strncpy(diag().flightErr, tmp.errorMsg, sizeof(diag().flightErr) - 1);
        LOG("Loty BLAD: %s\n", tmp.errorMsg);
      }
    }

    // ---- OTA (jedyne miejsce, gdzie dotykamy obiektu Update) ----
    const bool otaAsked = takeOtaRequest();
    if (settings().otaEnabled &&
        (otaAsked || static_cast<int32_t>(millis() - nextOtaAt) >= 0)) {
      // otaAsked = kliknięcie w panelu; tylko wtedy wolno wymusić wersję, która
      // wcześniej nie przeżyła okresu próbnego.
      ota.checkAndUpdate(otaAsked);  // przy sukcesie restartuje urządzenie
      nextOtaAt = millis() + cfg::OTA_CHECK_MS;
    }

    if (firstWeather && gBooting) {
      gBooting = false;
    }
    if (gBooting && millis() > 35000) {
      gBooting = false;
    }

    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

// -------------------------------------------------------------- zadanie web --

static void webTask(void*) {
  for (;;) {
    portal::loop();
    portal::serialConsole();
    vTaskDelay(pdMS_TO_TICKS(8));
  }
}

// ---------------------------------------------------------------- alerty ----

static Alert buildAlert(const WeatherModel& w, const PvModel& pv) {
  Alert a{};

  if (pv.online && pvStatusIsFault(pv.data.statusCode)) {
    a.kind = AlertKind::PV_FAULT;
    strncpy(a.title, "Awaria falownika", sizeof(a.title) - 1);
    snprintf(a.text, sizeof(a.text), "Status 0x%04X - sprawdź instalację",
             pv.data.statusCode);
    a.color = col::ERR;
    a.iconCode = -1;
    return a;
  }

  if (!w.ready) {
    return a;
  }

  int stormIn = -1;
  int rainIn = -1;
  float maxWind = w.current.windKmh;
  float minTemp = w.current.tempC;
  float maxTemp = w.current.tempC;
  float maxRain = 0.f;

  for (int i = 0; i < WX_HOURS; ++i) {
    const HourSlot& s = w.hours[i];
    if (!s.valid) continue;
    if (stormIn < 0 && s.data.weatherCode >= 95) stormIn = s.offsetHours;
    if (s.data.precipMm >= 4.f && rainIn < 0) rainIn = s.offsetHours;
    if (s.data.precipMm > maxRain) maxRain = s.data.precipMm;
    if (s.data.windKmh > maxWind) maxWind = s.data.windKmh;
    if (s.data.tempC < minTemp) minTemp = s.data.tempC;
    if (s.data.tempC > maxTemp) maxTemp = s.data.tempC;
  }

  if (w.current.weatherCode >= 95 || stormIn >= 0) {
    a.kind = AlertKind::STORM;
    strncpy(a.title, "Burza", sizeof(a.title) - 1);
    if (w.current.weatherCode >= 95) {
      snprintf(a.text, sizeof(a.text), "Burza nad %s - teraz", settings().city);
    } else {
      snprintf(a.text, sizeof(a.text), "Prognozowana za %d h", stormIn);
    }
    a.color = col::STORM;
    a.iconCode = 95;
    return a;
  }

  if (maxWind >= 60.f) {
    a.kind = AlertKind::WIND;
    strncpy(a.title, "Silny wiatr", sizeof(a.title) - 1);
    snprintf(a.text, sizeof(a.text), "Do %.0f km/h w ciągu 12 h", maxWind);
    a.color = col::WARN;
    a.iconCode = -1;
    return a;
  }

  if (rainIn >= 0) {
    a.kind = AlertKind::HEAVY_RAIN;
    strncpy(a.title, "Ulewa", sizeof(a.title) - 1);
    snprintf(a.text, sizeof(a.text), "Do %.1f mm/h - za %d h", maxRain, rainIn);
    a.color = col::RAIN;
    a.iconCode = 65;
    return a;
  }

  if (minTemp <= -2.f) {
    a.kind = AlertKind::FROST;
    strncpy(a.title, "Mróz", sizeof(a.title) - 1);
    snprintf(a.text, sizeof(a.text), "Do %.0f°C w ciągu 12 h", minTemp);
    a.color = col::T_FREEZE;
    a.iconCode = 71;
    return a;
  }

  if (maxTemp >= 30.f) {
    a.kind = AlertKind::HEAT;
    strncpy(a.title, "Upał", sizeof(a.title) - 1);
    snprintf(a.text, sizeof(a.text), "Do %.0f°C - pij wodę", maxTemp);
    a.color = col::T_HOT;
    a.iconCode = 0;
    return a;
  }

  return a;
}

// ------------------------------------------------------------------ setup ----

// KLUCZOWE dla rollbacku. Arduino core (initArduino(), esp32-hal-misc.c) domyslnie
// sam potwierdza swiezy obraz OTA — jeszcze zanim ruszy setup(). Wtedy OtaGuard nigdy
// nie zobaczylby stanu PENDING_VERIFY i caly mechanizm bylby atrapa: /api/diag pisalby
// "stabilna", a ochrony by nie bylo (tak zachowywala sie v29).
// Ten slaby symbol przejmuje odpowiedzialnosc: obraz potwierdzamy sami, dopiero gdy
// udowodni, ze dziala (WiFi + udane pobranie po TLS + heap ponad progiem).
extern "C" bool verifyRollbackLater() {
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(200);
  LOG("=== Pogoda + PV — firmware v%d ===\n", FW_VERSION);

  // PSRAM: do v49 kompilowalismy z PSRAM=disabled, wiec psramInit() nigdy sie nie
  // wykonywal i diagnostyka pokazywala "psram: 0". To nie byl dowod braku pamieci,
  // tylko dowod, ze o nia nie pytalismy. Teraz pytamy.
  if (psramFound()) {
    LOG("PSRAM: JEST — %lu kB (wolne %lu kB)",
        static_cast<unsigned long>(ESP.getPsramSize() / 1024),
        static_cast<unsigned long>(ESP.getFreePsram() / 1024));
  } else {
    LOG("PSRAM: brak (uklad bez PSRAM albo init sie nie powiodl)");
  }

  settings().load();

  // Zanim cokolwiek zaalokujemy: sprawdź, czy to świeża wersja po OTA (okres
  // próbny) i zapamiętaj powód ostatniego resetu.
  otaGuardBegin();

  gLock = xSemaphoreCreateMutex();

  if (!ui.begin()) {
    LOG("BLAD: nie udalo sie utworzyc bufora TFT (wolny heap %lu B)\n",
        static_cast<unsigned long>(ESP.getFreeHeap()));
    // Regresja typu v14: nowa wersja zjadła tyle RAM, że nie ma na bufor ekranu.
    // Nie ma po co czekać 3 minuty — jeśli to wersja próbna, cofamy się natychmiast.
    otaGuardFatal("brak RAM na bufor ekranu");
    while (true) {
      delay(1000);
    }
  }

  if (cfg::COLOR_TEST_MODE) {
    ui.drawColorTest();
    return;
  }

  ledBegin();

  // Czujniki: PIR cyfrowy + LDR analog.
  // GOLY INPUT, nie INPUT_PULLDOWN. Poczatkowo dalem pulldown "na wszelki wypadek",
  // ale pomiar na urzadzeniu (v100) pokazal, ze przekombinowalem: spoczynek jest
  // stabilnie LOW (modul sam trzyma low, wiec floating nie grozi), za to pulldown 45k
  // sciagal stan WYSOKI ponizej progu VIH i czesc machniec nie wyzwalala PIR
  // (pir_last_s rosl do 100+ mimo ciaglego ruchu). Bez pulldownu modul steruje sam.
  // (Ten pomiar jest prawdziwy; nazywal tylko zly modul — bylo tu "SR505", a wlutowany
  //  jest AM312. Sama obserwacja o pulldownie tego nie dotyczy.)
  // analogRead na GPIO1 = ADC1 dziala przy WiFi; ADC_11db daje pelny zakres ~0-3,1V
  // pod dzielnik 3,3V. Nastawy sa domyslne, ustawiamy je jawnie dla pewnosci.
  pinMode(cfg::PIN_PIR, INPUT);
  analogSetPinAttenuation(cfg::PIN_LDR, ADC_11db);

  // PIR takze na przerwaniu (oba zbocza) — po co i dlaczego tak, patrz pirIsr().
  // Kolejnosc jest istotna: najpierw liczniki i wskaznik, dopiero na koncu attach.
  // Odwrotnie pierwsze zbocze trafiloby w gDiagIsr == nullptr i przepadloby, a gorzej —
  // pirRtcBegin() wyzerowaloby po nim liczniki albo doliczyloby je do smieci z RTC.
  pirRtcBegin();
  // PO pirRtcBegin(), bo seeduje gLdrBookedRises z gPir.rises — przed nim czytaloby albo
  // smiec z RTC (zimny start), albo wartosc, ktora pirRtcBegin() zaraz wyzeruje.
  ldrRtcBegin();
  // Bez zaleznosci od powyzszych dwoch (gPvRtc nie jest z nimi wspoldzielona) — kolejnosc
  // wzgledem pirRtcBegin()/ldrRtcBegin() jest tu bez znaczenia.
  pvRtcBegin();
  gDiagIsr = &diag();
  attachInterrupt(digitalPinToInterrupt(cfg::PIN_PIR), pirIsr, CHANGE);

  pvHistoryLoad(gHist);
  roomHistoryLoad(gRooms);
  uiRooms = gRooms;
  ui.setRoomHistory(&uiRooms);
  gasHistoryLoad(gGas);   // 120 dni logu gazu — bez tego weryfikacja licznika nie ma z czym porownywac
  burnerHistoryLoad(gBurner);   // profil doby palnika — dotad ginal przy KAZDYM restarcie
  uiBurner = gBurner;           // zeby wykres mial dane JUZ w pierwszej klatce, przed pierwszym odpytem pieca
  ui.setBoiler(&uiVi);
  ui.setBurnerHistory(&uiBurner);
  // Bez wczytywania z NVS: jakosc powietrza NIE ma tu historii do odtworzenia
  // (w odroznieniu od gHist/gRooms/gGas/gBurner wyzej) — to biezacy odczyt, jak
  // pogoda/PV, wiec pierwsza probka po prostu poczeka na pierwszy udany fetch.
  ui.setAir(&uiAir);
  // Modele posrednie (v126): wskaznik podpinamy raz, zawartosc odswieza loop().
  // Bez tego rysowanie zobaczy pusta strukture — czyli "brak czujnikow" i
  // "pobieram mape opadow" — a nie czarny ekran ani wskaznik w nicosc.
  ui.setRoomModel(&uiRoomModel);
  ui.setRadarModel(&uiRadarModel);

  if (!settings().hasWifi()) {
    portal::beginAp();
    gBooting = false;
  }

  ui.drawBoot(gBootMsg, 0);

  // Zrzut ekranu leci z zadania web (rdzen 0) i NIE zatrzymuje rysowania (rdzen 1) —
  // inaczej ekran zamiera na czas wysylania BMP. Rysuje wlasna kopie klatki do
  // malego sprite'a, wiec bufora wyswietlacza w ogole nie dotyka. Czekamy tylko na
  // spokojna klatke, zeby nie zlapac ekranu w polowie przejscia miedzy widokami.
  //
  // SWIADOMA DECYZJA: czytamy uiWeather/uiPv/uiHist/uiFlights BEZ gLock, chociaz
  // loop() (rdzen 1) przepisuje je pod gLock. Zrzut trwa setki ms, wiec sklada dane
  // z kilku roznych chwil — podglad w panelu moze pokazac klatke, ktorej na TFT nie
  // bylo. Crashu tu nie ma: wszystkie tablice w tych strukturach maja stala dlugosc,
  // a liczniki (np. FlightModel::count) nie wyjda poza zakres nawet przy rozdarciu.
  // Wziecie gLock na czas calego zrzutu zablokowaloby netTask i rysowanie na setki ms
  // (panel ciagnie /api/screen co 700 ms) — zamienilibysmy niescisly podglad na
  // zacinajace sie urzadzenie. Wniosek: zrzut to PODGLAD, nie dowod w diagnostyce.
  // Od diagnostyki jest /api/diag i /api/log.
  portal::setScreenshotHandler([](WiFiClient& c) {
    // Nie w trakcie nasluchu BLE: zrzut alokuje sprite'y, a stos Bluetooth trzyma
    // wtedy 72 kB. Zbieg tych dwoch rzeczy zjechal sterta do 2 kB.
    uint32_t tw = millis();
    while (ble::scanning() && millis() - tw < 9000) delay(50);

    const uint32_t t0 = millis();
    while (!ui.stableFrame() && millis() - t0 < 800) delay(10);
    ui.streamScreenshot(c, uiWeather, uiPv, uiHist, uiFlights, gWifiOk);
  });

  portal::setViewHandler([](int i) { ui.pinView(i); },
                         [](int& cur, int& pin) { ui.viewState(cur, pin); });
  // Panel: przycisk dotyku dziala jak fizyczny pin GPIO7. V3 "Pasmowy" ma wlasna
  // nawigacje (touchTapV3/touchDoubleV3, spec 7a) — kierujemy tak samo jak dotyk
  // realny, dokladamy noteRawTouch() zeby kropka feedbacku pokazala sie tez w
  // podgladzie na zywo. V1/V2 zostaja przy restartHold/prevView bez zmian.
  portal::setTapHandler([](int n) {
    if (settings().theme == 3) {
      ui.noteRawTouch();
      if (n >= 2) ui.touchDoubleV3(); else ui.touchTapV3();
    } else {
      if (n >= 2) ui.prevView(); else ui.restartHold();
    }
  });
  // Podswietlenie: test sprzetu + podglad wysterowania (patrz Portal.h).
  portal::setBacklightHandler(
      [](uint8_t v, uint32_t ms) { ui.testBacklight(v, ms); },
      [](uint8_t& cur, uint8_t& tgt) {
        cur = ui.backlightCurrent();
        tgt = ui.backlightTarget();
      });
  portal::setBacklightSweepHandler([](uint32_t ms) { ui.startBacklightSweep(ms); });

  // BLE dopiero TERAZ — bufor ekranu (66 kB) jest już zaalokowany, więc stos
  // Bluetooth bierze z tego, co zostało, a nie odwrotnie. Gdyby zabrakło sterty,
  // OtaGuard i tak cofnie tę wersję: to jest właśnie ten scenariusz, przed którym
  // ma bronić.
  ble::begin();
  touch::begin();
  radarmap::begin();

  esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_BSS_RSSI_LOW, &onRssiLow, nullptr);

  // Rozmiary flasha liczone TU, raz — patrz komentarz w Diag (Log.h). getSketchSize()
  // skanuje partycje app; w setup() SPI jest wolne, wiec skan sie udaje. Ekran PAMIEC
  // i /api/diag czytaja te dwa pola zamiast wolac skan z goracej sciezki rysowania.
  diag().sketchBytes = ESP.getSketchSize();
  diag().flashBytes = ESP.getFlashChipSize();

  // Zrzut ekranu rysuje teraz calą klatkę (a nie tylko czyta bufor), więc stos zadania
  // web musi pomieścić cały łańcuch rysujący. 4 kB zapasu ze sterty, której i tak
  // przybyło 66 kB.
  xTaskCreatePinnedToCore(webTask, "web", 16384, nullptr, 2, &gWebTask, 0);
  xTaskCreatePinnedToCore(netTask, "net", 16384, nullptr, 3, &gNetTask, 0);
}

// ------------------------------------------------------------------- loop ----

void loop() {
  if (cfg::COLOR_TEST_MODE) {
    delay(1000);
    return;
  }

  const uint32_t now = millis();

  // --- czujniki v100 (LDR analog + PIR cyfrowy), surowo do /api/diag ---
  // Co 250 ms wystarczy: LDR jest wolnozmienny, a wlutowany PIR to AM312 — impuls ~2 s,
  // okno martwe ~2 s, wiec polling nie zgubi zbocza. Odczyt to mikrosekundy, nie rusza
  // mutexa (Diag to migawka bez blokad — pojedyncze pole rozjechane o klatke nikogo nie
  // boli).
  //
  // Stalo tu wczesniej "SR505 trzyma OUT ~8 s po ruchu" i to byla NIEPRAWDA: modul
  // potwierdzil wlasciciel 16.07.2026 — jest AM312, nie SR505. Decyzja (250 ms) byla
  // dobra, ale uzasadniona liczba z czujnika, ktorego tu nie ma; przy 8 s zapas wygladal
  // na 32-krotny, realnie jest 8-krotny. To istotne, bo od szerokosci impulsu zalezy caly
  // przyszly timeout podswietlenia. Te ~2 s to nadal KATALOG, nie pomiar w tej lazience —
  // mierzy je dopiero pir_width_ms w /api/diag i to ono, a nie ten komentarz, bedzie
  // dowodem.
  //
  // pirLastAt pisze teraz pirIsr() (dokladny moment zbocza, nie krata 250 ms). Tutaj
  // zostaje samo odswiezanie pirState do podgladu.
  {
    static uint32_t nextSensorAt = 0;
    if (static_cast<int32_t>(now - nextSensorAt) >= 0) {
      nextSensorAt = now + 250;
      // Oba pola usredniamy z 8 odczytow. Wczesniej ldrRaw bylo srednia z osmiu, ale
      // ldrMv brala POJEDYNCZY analogReadMilliVolts() — czyli dwa pola /api/diag,
      // ktore maja opisywac ten sam pomiar, pochodzily z roznych probek i moglyby sie
      // rozjechac. Od v103 ldrMv steruje podswietleniem, wiec niech bedzie tak stabilne,
      // jak zawsze twierdzil opis. Kosztuje 8 dodatkowych konwersji na 250 ms (mikrosekundy).
      uint32_t acc = 0, accMv = 0;
      for (int i = 0; i < 8; ++i) {
        acc += analogRead(cfg::PIN_LDR);              // usredniamy szum ADC
        accMv += analogReadMilliVolts(cfg::PIN_LDR);
      }
      diag().ldrRaw = static_cast<uint16_t>(acc / 8);
      diag().ldrMv = static_cast<uint16_t>(accMv / 8);
      diag().pirState = digitalRead(cfg::PIN_PIR) != 0;

      // Histogram jasnosci (v108, patrz LdrRtc w Log.h). Zliczamy TU, w istniejacym bloku
      // 250 ms, a nie w osobnym timerze — bo to jedyne miejsce, w ktorym ldrMv jest SWIEZE.
      // Drugi timer albo trafialby w te sama probke dwa razy (i zawyzal kosze bez powodu),
      // albo probkowalby wlasnym rytmem i wtedy histogram opisywalby INNY sygnal niz ten,
      // ktory steruje ekranem — czyli mierzylby nie to, o co pytamy.
      // Staly rytm 250 ms jest tu warunkiem sensu: kosz = liczba probek, wiec zliczenia
      // przeliczaja sie na SEKUNDY tylko dlatego, ze kazda probka wazy tyle samo (1/4 s).
      // Blok stoi PONAD wczesnymi return-ami loop(), wiec czas w portalu AP i na ekranie
      // startowym tez jest probkowany — tak jak collectedS, ktorym sie to normuje.
      ++gLdr.hist[ldrHistBucket(diag().ldrMv)];
    }
  }

  // --- DIAGNOSTYKA: blysk diody przy wyzwoleniu PIR ---
  // Na czas pomiaru — zeby dalo sie chodzic po lazience i widziec czujnik na zywo,
  // zamiast odpytywac /api/diag. Znika razem z pomiarem.
  //
  // pirLastAt pisze pirIsr(); tu tylko czytamy. To pojedynczy wyrownany uint32
  // (volatile), wiec odczyt jest atomowy i nie potrzebuje blokady — Diag i tak jest
  // migawka bez mutexa. Start: pirLastAt == 0 i lastPirSeen == 0, wiec zaden falszywy
  // blysk nie leci przy starcie.
  //
  // Miejsce nie jest przypadkowe: PONAD wczesnymi return-ami loop() (test diody,
  // portal AP, OTA, ekran IP, boot). Tam ledShowGrid() sie nie wykonuje, wiec blysk
  // sie nie wyrenderuje — ale lastPirSeen leci dalej i po powrocie do normalnego
  // rysowania nie odpala sie blysk za zdarzenie sprzed kilkunastu sekund.
  {
    static uint32_t lastPirSeen = 0;
    const uint32_t pirAt = diag().pirLastAt;
    if (pirAt != lastPirSeen) {
      lastPirSeen = pirAt;
      ledPirFlash();
    }
  }

  // --- PIR: rytm doby + ciaglosc pomiaru (patrz PirRtc w Log.h) ---
  // Miejsce nie jest przypadkowe: tak jak blysk diody wyzej, siedzi PONAD wczesnymi
  // return-ami loop() (portal AP, OTA, ekran IP, boot). Gdyby bylo nizej, kazda minuta
  // w trybie AP albo na ekranie startowym wypadalaby z collectedS i z byHour, czyli
  // pomiar klamalby dokladnie o tym, ile go naprawde bylo.
  //
  // Wszystko to jest TANIE przy 30 fps: dwa odczyty uint32 z RTC i porownanie. time()
  // wolamy dopiero, gdy naprawde cos przyszlo albo gdy startedEpoch jest jeszcze puste.
  {
    // collectedS: doliczamy PRZYROSTY millis(), nigdy wartosc bezwzgledna — millis()
    // zeruje sie po restarcie, wiec "collectedS = millis()/1000" cofaloby licznik do
    // zera przy kazdym OTA i tracilo dokladnie to, po co ten licznik istnieje.
    // Reszte ponizej sekundy zostawiamy w gPirTickMs, zeby nie gubic po ~0,5 s na obrot.
    const uint32_t dt = now - gPirTickMs;
    if (dt >= 1000) {
      gPir.collectedS += dt / 1000;
      gLdr.collectedS += dt / 1000;   // ten sam przyrost, osobny licznik — patrz LdrRtc
      gPvRtc.collectedS += dt / 1000; // trzeci licznik, ten sam przyrost — patrz PvRtc
      gPirTickMs += (dt / 1000) * 1000;

      // --- LDR: detektor "zostawione swiatlo" (v108, patrz LdrRtc w Log.h) ---
      // Wolane RAZ NA SEKUNDE, w tym samym `if` co collectedS — nie przez wygode, tylko
      // dlatego, ze dlugosc ciaglosci liczymy wlasnie w collectedS. Gdyby to bylo w
      // osobnym warunku, oba liczniki moglyby sie rozjechac o obrot petli i zdarzenie
      // twierdziloby, ze trwa dluzej, niz pomiar w ogole zbieral.
      ldrEventTick();
    }

    // startedEpoch ustawiamy raz, przy pierwszym czasie z NTP. Odejmowanie collectedS
    // jest istotne: NTP wchodzi kilka-kilkanascie sekund PO starcie zbierania, a po
    // zimnym starcie bez sieci moze wejsc dopiero po godzinach. Bez tego odejmowania
    // "zbieram od" pokazywaloby moment SYNCHRONIZACJI, a nie poczatek pomiaru.
    if (gPir.startedEpoch == 0) {
      const time_t tt = time(nullptr);
      if (tt > 1700000000) {
        gPir.startedEpoch = static_cast<uint32_t>(tt) - gPir.collectedS;
      }
    }

    // To samo dla LDR. Pole istnialo i bylo czytane w /api/diag od v108, ale NIKT go
    // nie ustawial — ldr_meas.started_epoch wychodzil zawsze null (zlapane na urzadzeniu
    // po wydaniu v108: pir_meas mial epoch, ldr_meas nie). Same zdarzenia "zostawione
    // swiatlo" maja wlasny znacznik (LdrEvent.startEpoch z candStartEpoch) i dzialaly —
    // to bylo tylko martwe pole "zbieram od", nie utrata godzin zdarzen. Odejmowanie
    // collectedS jak przy PIR: startedEpoch ma znaczyc POCZATEK pomiaru, nie moment NTP.
    if (gLdr.startedEpoch == 0) {
      const time_t tt = time(nullptr);
      if (tt > 1700000000) {
        gLdr.startedEpoch = static_cast<uint32_t>(tt) - gLdr.collectedS;
      }
    }

    // To samo dla PV (patrz PvRtc w Log.h) — wszystkie trzy liczniki (gPir/gLdr/gPvRtc)
    // tykaja z JEDNEGO wspolnego `dt` wyzej, wiec "zbieram od" kazdego z nich opisuje
    // dokladnie to samo okno czasu i mozna je porownywac 1:1.
    if (gPvRtc.startedEpoch == 0) {
      const time_t tt = time(nullptr);
      if (tt > 1700000000) {
        gPvRtc.startedEpoch = static_cast<uint32_t>(tt) - gPvRtc.collectedS;
      }
    }

    // Rytm doby. Ksiegujemy PRZYROST zbocz w gore, a nie pojedyncze zdarzenie — przy
    // 30 fps loop widzi kazde zbocze AM312 (impuls ~2 s), ale przyrost jest odporny
    // takze wtedy, gdy petla utknie na dluzej (OTA, portal, wolny render).
    const uint32_t rises = gPir.rises;
    if (rises != gPirBookedRises) {
      const time_t tt = time(nullptr);
      if (tt > 1700000000) {
        struct tm tmv;
        localtime_r(&tt, &tmv);   // czas LOKALNY (TZ ustawia connectWifi) — pytanie
                                  // brzmi "o ktorej ktos chodzi", a nie "o ktorej UTC"
        gPir.byHour[tmv.tm_hour] += rises - gPirBookedRises;
      }
      // Przesuwamy ZAWSZE, takze bez czasu z NTP. Inaczej zbocza sprzed synchronizacji
      // czekalyby w kolejce i w chwili zlapania czasu wpadlyby CALE w jedna godzine —
      // zmyslony pik o godzinie, o ktorej akurat wstal WiFi. Lepiej je zgubic: jest ich
      // kilka, a pomiar ma trwac tydzien.
      gPirBookedRises = rises;
    }
  }

  // --- dotyk GPIO7 ---
  // V3 "Pasmowy" (spec 7a) ma WLASNA nawigacje: 1x = nastepny ekran w petli 8
  // widokow, 2x = wejscie/wyjscie z diagnostyki, powrot na GLOWNY po 60 s ciszy
  // (to ostatnie zalatwia render()). V1/V2 zostaja przy STARYM zachowaniu:
  // restartHold (odliczanie od nowa) / prevView (poprzedni) + auto-rotacja.
  const bool v3nav = settings().theme == 3;
  switch (touch::poll()) {
    case touch::Tap::SINGLE:
      if (v3nav) { ui.touchTapV3();    LOG("Dotyk V3: nastepny ekran"); }
      else       { ui.restartHold();   LOG("Dotyk: odliczanie ekranu od nowa"); }
      break;
    case touch::Tap::DOUBLE:
      if (v3nav) { ui.touchDoubleV3(); LOG("Dotyk V3 x2: diagnostyka"); }
      else       { ui.prevView();      LOG("Dotyk x2: poprzedni ekran"); }
      break;
    default:
      break;
  }
  // Kropka feedbacku V3: zapal ja NATYCHMIAST po surowym dotyku elektrody, zanim
  // minie okno 550 ms rozroznienia 1x/2x. touch::pressedRaw() zwraca stan policzony
  // w poll() wyzej (bez dodatkowego odczytu ADC). Tylko V3 rysuje kropke, wiec tylko
  // tu ustawiamy znacznik — rawTouchMs_ w V1/V2 zostaje martwy.
  if (v3nav && touch::pressedRaw()) ui.noteRawTouch();

  // --- autotest diody RGB przy starcie (3 x 500 ms) ---
  // NIE zabiera juz ekranu. Dioda i TFT to osobny sprzet — test jednego nie ma prawa
  // wygaszac drugiego, a dokladnie to robil: przez pierwsze 1,5 s po starcie zamiast
  // "Laczenie z WiFi..." staly slowa "CZERWONY/ZIELONY/NIEBIESKI". To jest ta sama
  // 1,5 s, w ktorej trwa laczenie z siecia — czyli ekran klamal o tym, co robi
  // urzadzenie, dokladnie wtedy, gdy ktos patrzy (bo wlasnie je powiesil).
  // Bez "return": test leci w tle, a ekran pokazuje prawde. ledShowGrid() i tak czeka
  // na gTestDone, wiec bilans energii nie wejdzie diodzie w droge.
  ledTestStep();

  // --- tryb konfiguracji: instrukcja na ekranie ---
  if (portal::apActive()) {
    ui.drawSetup(portal::apSsid(), portal::apPass(), portal::apIp());
    if (portal::wifiJustSaved()) {
      portal::clearWifiSavedFlag();
      delay(1500);
      ESP.restart();
    }
    delay(60);
    return;
  }

  // --- aktualizacja w toku: oddaj bufor ekranu i rysuj wprost na TFT ---
  const OtaStatus& os = otaStatus();
  if (os.state == OtaState::DOWNLOADING || os.state == OtaState::DONE ||
      os.state == OtaState::FAILED) {
    ui.releaseBuffer();
    otaUiBufferFreed();
    ui.drawOtaDirect(os.progress, os.message);
    delay(50);
    return;
  }

  // --- ekran "połączono" z adresem IP (10 s po starcie) ---
  if (gNetInfoUntil != 0) {
    if (static_cast<int32_t>(now - gNetInfoUntil) < 0) {
      const int left = static_cast<int>((gNetInfoUntil - now + 999) / 1000);
      ui.drawNetInfo(settings().ssid, gIpStr, gRssi, left, NET_INFO_MS / 1000);
      delay(60);
      return;
    }
    gNetInfoUntil = 0;
  }

  if (gBooting) {
    ui.drawBoot(gBootMsg, gWifiAttempt);
    delay(25);
    return;
  }

  xSemaphoreTake(gLock, portMAX_DELAY);
  uiWeather = gWeather;
  uiPv = gPv;
  uiHist = gHist;
  uiRooms = gRooms;
  uiVi = gVi;
  uiBurner = gBurner;
  uiFlights = gFlights;
  uiAir = gAir;
  xSemaphoreGive(gLock);

  // --- modele posrednie dla dwoch ekranow, ktore ich nie mialy (v126) ---
  // POZA gLock swiadomie: zrodlem nie jest zadne g*, tylko ble:: i radarmap::,
  // a kazdy z nich ma wlasny mutex. Trzymanie tu globalnej blokady wydluzaloby
  // tylko okno, w ktorym netTask nie moze zapisac swoich danych.
  //
  // Czytane bez blokady takze przez zrzut ekranu z webTask — dokladnie ta sama
  // swiadoma decyzja, co przy uiWeather/uiPv (patrz komentarz przy
  // ui.streamScreenshot w webTask): najgorszy przypadek to jedna klatka zlozona
  // z dwoch sasiednich odczytow, a nie uszkodzona pamiec.
  //
  // Co klatke, a nie co sekunde: obie funkcje to petla po najwyzej osmiu
  // czujnikach i garsc dzialan zmiennoprzecinkowych. Rzadsze odswiezanie
  // oszczedzaloby mikrosekundy, a kosztowaloby to, ze `nowMs` w modelu przestaje
  // byc tym samym "teraz", co reszta klatki — czyli dokladnie wade, ktora ta
  // zmiana usuwa.
  buildRoomModel(uiRoomModel, now);
  buildRadarModel(uiRadarModel, uiWeather, now);

  gFlightsNeeded = ui.needsFlights(now);

  // --- jasnosc otoczenia -> podswietlenie + dioda RGB (bilans z siecia) ---
  // Do v102 decydowal ZEGAR (22:00-6:00). Mylil sie w obie strony, bo nie wie ani czy
  // ktos jest, ani czy pali sie swiatlo: o 23:00 przy zapalonym swietle trzymal ekran
  // przygaszony, a o 14:00 przy zgaszonym swiecil na maksa. Teraz decyduje LDR, czyli
  // to samo, co widzi czlowiek. Efekt uboczny, ktory jest czysta wygrana: dziala od
  // PIERWSZEJ SEKUNDY, bez NTP i bez WiFi — a stary blok caly siedzial w
  // `if (czas ustawiony)`, wiec do pierwszej synchronizacji podswietlenie nie bylo
  // ustawiane w ogole (ratowal to blTarget_ = BL_DAY z WeatherUi::begin()).
  // Nic innego z tamtego `if` nie zginelo: `night` szlo wylacznie tutaj i do diody.
  // Nocne milczenie falownika liczy sobie osobno pvMayBeAsleep() ze wschodu/zachodu.
  //
  // SPRZEZENIA ZWROTNEGO NIE MA — i to jest POMIAR, nie zalozenie. Naturalny strach
  // brzmi: podswietlenie oswietli wlasny czujnik, LDR zobaczy "jasno", ekran rozjasni
  // sie jeszcze bardziej i uklad sie rozhusta. Dowod, ze nie: odczyt 251 mV ("ciemno")
  // padl ok. 19:30, czyli PRZED 22:00 starej granicy — ekran swiecil wtedy pelnia 255,
  // a LDR i tak widzial ciemnosc. Ilosciowo: 251 mV to ~96 kOhm, czyli ulamek luksa;
  // podswietlenie 320x240 na 255 w polu widzenia czujnika dawaloby o rzedy wielkosci
  // wiecej. Czujnik po prostu nie patrzy na ekran. Do progu LDR_DIM_UP_MV (650) brakuje
  // 400 mV, wiec petla nie ma jak sie domknac.
  // NIE DOKLADAJ tu obrony przed oscylacja (opoznien, blokad, gaszenia na czas pomiaru):
  // bronilaby przed zjawiskiem, ktore pomiar wyklucza, i kosztowala kod, ktorego nikt
  // nie umialby przetestowac.
  //
  // 0 = ciemno, 1 = polmrok, 2 = swiatlo zapalone. Start na 1: przy pierwszym przebiegu
  // odczyt LDR jest juz swiezy (blok czujnikow stoi na gorze loop(), przed wszystkimi
  // wczesnymi return-ami), wiec ta wartosc i tak zyje jedna klatke.
  static uint8_t blLevel = 1;
  const uint16_t ldrMv = diag().ldrMv;

  // WYKRYWANIA AWARII CZUJNIKA TU NIE MA — i to jest decyzja, nie zapomnienie.
  //
  // v103 mialo warunek "ldr_mv < 50 mV przez 60 s => czujnik zepsuty => wymus polmrok".
  // Prog 50 mV wzial sie z zalozenia, ze ciemnosc to ~251 mV (bo tyle zmierzylem).
  // ZALOZENIE BYLO BLEDNE: te 251 mV padlo o 19:30, czyli w ZMIERZCHU, nie w ciemnosci.
  // Pomiar o 23:30, przy zgaszonym swietle i pustej lazience, dal **17-26 mV** (LDR ~1,3 MOhm
  // — siedem razy wiecej, niz mowi nota katalogowa dla "dark").
  // Skutek: prawdziwa ciemnosc byla rozpoznawana jako AWARIA i ekran szedl na 130 zamiast 45.
  // Wlasciciel znalazl to w 30 minut, siedzac po ciemku i czekajac az ekran przygasnie.
  //
  // Dlaczego nie da sie tego naprawic obnizeniem progu: odlaczony LDR sciaga wejscie do
  // masy przez 7,93 kOhm i daje ~0 mV, a prawdziwa ciemnosc daje ~20 mV. Miedzy "zepsuty"
  // a "ciemno" jest ~20 mV, czyli tyle, ile wynosi nieliniowosc ADC przy samym dnie skali.
  // Progu, ktory rozroznia te dwa stany, po prostu NIE MA.
  //
  // Co sie stanie, gdy czujnik naprawde padnie: odczyt ~0 mV => poziom "ciemno" => ekran
  // przygaszony do 45. Czytelny, tylko ciemny, i nie reaguje na wlacznik swiatla —
  // a `ldr_mv` w /api/diag pokazuje ~0. To jest lepszy stan niz falszywy alarm, ktory
  // rozjasnia ekran w nocy i twierdzi, ze wie lepiej.
  {
    // Histereza na obu granicach. Kolejnosc kaskady jest CELOWA i robi dwie rzeczy:
    // (1) zapalenie swiatla (251 -> 3164 mV) przechodzi 0->1->2 w JEDNYM przebiegu,
    //     wiec nie ma schodkowania po jednym poziomie na klatke;
    // (2) skok przez cale pasmo do wnetrza przeciwnej histerezy nie zostawia poziomu
    //     nieprzylegajacego. Gdyby to byl lancuch else-if na przedzialach, poziom 2
    //     przy odczycie 500 mV (w pasmie 400-650) utknalby na 2, bo "srodek pasma
    //     histerezy = zostaw jak bylo" — ekran swiecilby pelnia po zgaszeniu swiatla.
    //     Tu 2 spada na 1, a potem 1 na 0.
    if (blLevel == 0 && ldrMv >= cfg::LDR_DIM_UP_MV) blLevel = 1;    // ciemno  -> polmrok
    if (blLevel == 1 && ldrMv >= cfg::LDR_DAY_UP_MV) blLevel = 2;    // polmrok -> swiatlo
    if (blLevel == 2 && ldrMv < cfg::LDR_DAY_DOWN_MV) blLevel = 1;   // swiatlo -> polmrok
    if (blLevel == 1 && ldrMv < cfg::LDR_DIM_DOWN_MV) blLevel = 0;   // polmrok -> ciemno
  }

  // --- ile czasu ekran realnie spedza na kazdym poziomie (v108, patrz LdrRtc w Log.h) ---
  // Miejsce jest CELOWE i odwrotne niz przy histogramie: ten licznik stoi TUTAJ, PONIZEJ
  // wczesnych return-ow loop(), a nie na gorze razem z probkowaniem. Powod: blLevel jest
  // wyliczany dopiero tu i tylko tu wolamy setBacklightTarget(). W portalu AP, podczas OTA
  // i na ekranie startowym ten blok sie NIE WYKONUJE, wiec zaden poziom wtedy nie
  // obowiazuje — doliczanie tamtych sekund do ostatnio znanego poziomu byloby zmyslaniem.
  // Konsekwencja, ktora trzeba znac czytajac /api/diag: suma ldr_level_s jest MNIEJSZA niz
  // collected_s, dokladnie o czas w portalu/OTA/na bootowaniu. To nie blad — to ta sama
  // roznica, co collected_s kontra wall_s, tylko o pietro nizej. Dlatego /api/diag podaje
  // ldr_level_s_sum wprost, zeby nikt nie musial jej sumowac w glowie i sie zastanawiac.
  {
    const uint32_t dt = now - gLdrLevelTickMs;
    gLdrLevelTickMs = now;
    // dt >= 2000 znaczy "tego bloku nie bylo tu przez chwile" (OTA, portal, ekran IP) —
    // przy 30 fps normalne dt to ~33 ms. Takiej dziury NIE ksiegujemy pod zaden poziom,
    // tylko resynchronizujemy zegar. To samo zalatwia pierwszy przebieg po starcie, gdzie
    // gLdrLevelTickMs pochodzi jeszcze z setup() i dt liczy sie w sekundach laczenia z WiFi.
    if (dt < 2000) {
      gLdrLevelAccMs += dt;
      if (gLdrLevelAccMs >= 1000) {
        gLdr.levelS[blLevel] += gLdrLevelAccMs / 1000;
        gLdrLevelAccMs %= 1000;   // reszta zostaje — patrz komentarz przy deklaracji
      }
    }
  }

  // Log tylko przy ZMIANIE — w petli 30 fps kazdy inny wariant zalalby bufor logu
  // (3072 B = ~47 linii = ~6 minut) i wymiotl z niego wszystko inne.
  static uint8_t lastBlLevel = 0xFF;
  if (blLevel != lastBlLevel) {
    lastBlLevel = blLevel;
    LOG("LDR: %u mV -> poziom %u (%s)\n", (unsigned)ldrMv, (unsigned)blLevel,
        blLevel == 0 ? "ciemno" : (blLevel == 1 ? "polmrok" : "swiatlo"));
  }

  // Poziomy jasnosci edytowalne z panelu (settings().bl*, clampowane w Settings z
  // TWARDYM minimum 15/30/60 — panel nie zgasi ekranu na stale). Czytane na biezaco,
  // wiec zmiana z panelu dziala od nastepnej klatki, bez restartu. cfg::BL_* zostaja
  // domyslnymi tych pol (patrz Settings::load) i nadal sluza ekranom startowym/OTA.
  ui.setBacklightTarget(blLevel == 0   ? settings().blNight
                        : blLevel == 1 ? settings().blDim
                                       : settings().blDay);

  // Dioda zostaje DWUSTANOWA, mimo trzech poziomow ekranu. `night` = poziom najciemniejszy.
  // Powod: LED_DAY/LED_NIGHT to liczby dostrojone okiem, a trzecia byla by zmyslona —
  // nikt nie zmierzyl, czy 90 oslepia w polmroku. Do tego dioda to wskaznik czytany
  // katem oka, a nie powierzchnia do czytania: trzeci odcien przygaszenia jest
  // rozroznieniem, ktorego nie da sie zauwazyc. Semantyka trzyma sie kupy — skoro
  // w pomieszczeniu jest na tyle jasno, ze LDR mowi "polmrok" (4x jasniej niz ciemnosc),
  // to dioda na 90 nie ma kogo oslepic. Gdyby jednak oslepiala, poprawka to jedna stala
  // LED_DIM i mapowanie na 3 poziomy — ale dopiero Z DANYMI, jak wszystko w tym repo.
  ledShowGrid(uiPv.data.gridPowerW, uiPv.online, blLevel == 0);

  // --- alerty ---
  const Alert a = buildAlert(uiWeather, uiPv);
  if (a.kind != AlertKind::NONE) {
    const int idx = static_cast<int>(a.kind);
    const bool changed = (a.kind != lastAlertKind);
    const bool cooled =
        (lastAlertAt[idx] == 0) || (now - lastAlertAt[idx] >= cfg::ALERT_COOLDOWN_MS);
    if (changed || cooled) {
      lastAlertAt[idx] = now;
      lastAlertKind = a.kind;
      ui.raiseAlert(a, now);
      Serial.printf("ALERT: %s — %s\n", a.title, a.text);
    }
  } else {
    lastAlertKind = AlertKind::NONE;
  }

  const bool animating = ui.render(uiWeather, uiPv, uiHist, uiFlights, gWifiOk, now);

  // TAKTOWANIE KLATEK, nie stala pauza. Poprzednio bylo delay(33) PO renderowaniu,
  // wiec okres klatki wynosil "rysowanie + 33 ms": przy 66 ms rysowania dawalo to
  // 99 ms (10 fps). Podniesienie SPI z 27 na 80 MHz scielo rysowanie do 32 ms,
  // ale pauza i tak doklejala swoje 33 — z 31 klatek, ktore sprzet potrafi,
  // polowa szla do kosza. Teraz czekamy tylko RESZTE okresu.
  // Cel liczymy od POPRZEDNIEGO CELU, nie od "teraz". Inaczej — a tak wlasnie
  // zrobilem za pierwszym razem — po kazdym renderowaniu jestesmy "spoznieni"
  // wzgledem samych siebie, resetujemy baze i czekamy pelny okres. Czyli znowu
  // "rysowanie + 70 ms" (zmierzone: 110 ms / 9 fps) zamiast rownych 70 ms.
  static uint32_t nextFrameAt = 0;
  const uint32_t period = animating ? cfg::FRAME_ACTIVE_MS : cfg::FRAME_IDLE_MS;

  if (nextFrameAt == 0) nextFrameAt = millis();
  nextFrameAt += period;

  int32_t left = static_cast<int32_t>(nextFrameAt - millis());
  if (left < 1) {
    // Rysowanie nie wyrobilo sie w okresie. Jesli zostalismy w tyle o wiecej niz
    // caly okres (zrzut ekranu, OTA), kasujemy dlug — inaczej gonilibysmy strate
    // serią klatek bez pauzy i zaglodzili pozostale zadania.
    if (left < -static_cast<int32_t>(period)) nextFrameAt = millis();
    left = 1;
  }
  delay(left);

  // rzeczywisty okres klatki (srednia kroczaca) — zeby bylo widac, czy cel 30 fps
  // jest naprawdę osiągany, a nie tylko deklarowany
  static uint32_t lastFrame = 0;
  if (lastFrame != 0) {
    diag().framePeriodUs = (diag().framePeriodUs * 7 + (micros() - lastFrame)) / 8;
  }
  lastFrame = micros();
}
