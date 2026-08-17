#pragma once

#include <cstdint>

#include "RoomHistory.h"  // RoomHistory::ROOMS - limit slotow BLE, patrz BLE_USABLE

// Cała konfiguracja urządzenia siedzi w pamięci nieulotnej (NVS).
// W kodzie źródłowym NIE MA żadnych haseł ani adresów — repo może być publiczne,
// a binarka OTA nie zawiera niczego prywatnego.
struct Settings {
  char ssid[33] = {};
  char pass[65] = {};

  char city[40] = "Gdynia";
  float lat = 54.4870f;
  float lon = 18.5216f;

  char modbusHost[24] = {};  // ustawiane w panelu WWW
  uint16_t modbusPort = 502;
  uint16_t pvPeakW = 6000;

  bool otaEnabled = true;

  // --- SKORKA WYSWIETLACZA (hak na przyszlosc) -------------------------------
  // (v162) Pole PRZYWROCONE na wyrazne polecenie wlasciciela: "Zostaw endpointy,
  // gdybym kiedys wpadl na pomysl skorek do wyswietlacza, to ma zostac". W v160,
  // przy usuwaniu motywow V1/V2, zniknelo RAZEM z nimi — i razem z /api/theme, polem
  // w /api/state oraz sekcja "Wyglad interfejsu" w panelu. Wraca wiec jako SWIADOMY,
  // PUSTY HAK, a nie jako udawanie, ze nic sie nie stalo: dzis rysowanie ma dokladnie
  // jedna sciezke (WeatherUi::paintFrame -> drawV3) i pole na nia NIE WPLYWA.
  //
  // DOKLADNIE JEDNA WARTOSC JEST LEGALNA — THEME_PASMOWY. Kazda inna (1 i 2 po
  // motywach V1/V2, ktore u wlasciciela NADAL LEZA W NVS pod kluczem "theme", a takze
  // 0 i smieci) jest sprowadzana do niej przy odczycie w load(). TO JEST JEDYNA RZECZ
  // STOJACA MIEDZY STARA WARTOSCIA W NVS A CZARNYM EKRANEM: gdyby ktokolwiek kiedys
  // rozgalezil rysowanie po tym polu, a load() oddawal surowe 2, urzadzenie wpadloby
  // w galaz nieistniejacego motywu. Urzadzenie jest tylko-OTA — z czerni nie ma
  // powrotu. Nie usuwaj tego sprowadzania przy dodawaniu drugiej skorki; rozszerz je.
  //
  // ===== JAK DODAC DRUGA SKORKE (krotka instrukcja, zeby ten hak nie zgnil) =====
  //   1. Dopisz stala obok THEME_PASMOWY ponizej (kolejny numer, np. 5 — NIE 1 ani 2,
  //      te dwie sa spalone przez stare wartosci w NVS u wlasciciela).
  //   2. Dopisz ja do themeValid() ponizej — to ono decyduje, co przejdzie przez
  //      load() i przez POST /api/theme.
  //   3. Dopisz nazwe do wyswietlenia w kThemes[] w Portal.cpp (tablica obok
  //      apiTheme) — GET /api/theme oddaje z niej liste dostepnych skorek.
  //   4. Rozgalez RYSOWANIE: WeatherUi::paintFrame() w WeatherUi.cpp wola dzis
  //      bezwarunkowo drawV3()/drawV3Bottom(); tam wstaw wybor wg settings().theme.
  //   5. Panel WWW: odkomentuj/dopisz sekcje wyboru wygladu — miejsce jest wskazane
  //      komentarzem w literale HTML w Portal.cpp (szukaj "SKORKI").
  static constexpr uint8_t THEME_PASMOWY = 3;   // uklad V3 "Pasmowy" — JEDYNY istniejacy
  // Numery 1 i 2 to WYCOFANE motywy V1 ("klasyczny") i V2 ("retro"). Zarezerwowane na
  // zawsze: u kazdego urzadzenia, ktore chodzilo na v159 lub starszym, jedna z nich
  // nadal siedzi w NVS pod kluczem "theme".
  static constexpr bool themeValid(uint8_t t) { return t == THEME_PASMOWY; }
  uint8_t theme = THEME_PASMOWY;
  // Zapisuje OD RAZU do NVS pod WLASNYM kluczem "theme" (jak viSave()/saveTuning()) i
  // tylko wtedy, gdy t przechodzi themeValid(). Wlasny klucz, a nie blob — dopisanie
  // tego pola z powrotem NIE przesuwa zadnego innego ustawienia, bo load()/save()
  // adresuja kazde pole osobno (patrz Settings.cpp).
  // false = wartosc odrzucona; RAM i NVS zostaja nietkniete.
  bool setTheme(uint8_t t);

  // --- USTAWIENIA WYSWIETLACZA edytowalne z panelu (dawniej stale w Config.h) ---
  // Trzymane jako WARTOSCI GOTOWE (nie sentinel 0): load() nakłada clamp, a rysowanie
  // i backlight czytaja je WPROST, bez logiki fallbacku — to samo pole zawsze niesie
  // wartosc uzywalna. Domyslne rowne dotychczasowym stalym (noc 22..6, ekran 9 s,
  // jasnosc 255/130/45), wiec urzadzenie sprzed tej wersji zachowuje sie identycznie.
  uint8_t  nightStartH = 22;  // godzina poczatku okna nocnego (0..23) — ekran glowny zwija sie do zegara
  uint8_t  nightEndH   = 6;   // godzina konca okna nocnego (0..23)
  uint16_t dwellS      = 9;   // czas jednego ekranu rotacji [s] (DWELL_MIN..DWELL_MAX)
  uint8_t  blDay       = 255; // jasnosc podswietlenia: swiatlo (>= BL_DAY_MIN)
  uint8_t  blDim       = 130; // jasnosc podswietlenia: polmrok (>= BL_DIM_MIN)
  uint8_t  blNight     = 45;  // jasnosc podswietlenia: ciemno  (>= BL_NIGHT_MIN)

  // --- AUTO-ROTACJA EKRANOW (TYLKO motyw V3 "Pasmowy") -----------------------
  // Domyslnie WYLACZONA: brief V3 (spec 7a) mowi, ze ekrany NIE przelaczaja sie same
  // — przelacza je dotyk (touchTapV3). Gdy WLACZONA z panelu: widoki petli V3 zmieniaja
  // sie same co dwellS sekund (ten sam interwal "czas jednego ekranu"), a dotyk pauzuje
  // rotacje (po 60 s bez dotyku wraca GLOWNY i cykl rusza dalej). V1/V2 tego pola NIE
  // czytaja — ich rotacja leci zawsze (holdFor). Zapis: saveTuning(), klucz NVS "arot",
  // natychmiast (jak reszta tuningu). Bez clampu — bool.
  bool autoRotate = false;

  // TWARDE MINIMUM jasnosci. Urzadzenie wisi w lazience bez klawiatury — z czarnego
  // ekranu nie ma jak wrocic, wiec panel NIE MOZE zejsc ponizej tych progow. Jedno
  // zrodlo prawdy: clampTuning() (load i saveTuning) i endpoint clampuja tak samo.
  static constexpr uint8_t  BL_DAY_MIN   = 60;
  static constexpr uint8_t  BL_DIM_MIN   = 30;
  static constexpr uint8_t  BL_NIGHT_MIN = 15;
  static constexpr uint16_t DWELL_MIN    = 3;
  static constexpr uint16_t DWELL_MAX    = 60;

  // Zapis OD RAZU do NVS (jak viSave()): osobne klucze, natychmiastowo,
  // niezaleznie od save(). Argumenty sa clampowane w srodku (przez clampTuning),
  // wiec panel moze podac cokolwiek — twardych progow pilnujemy TU, nie w UI. Po
  // zapisie pola w RAM sa juz clampniete i nastepna klatka czyta nowe wartosci
  // (bez restartu). false = NVS niedostepny.
  bool saveTuning(uint8_t nStart, uint8_t nEnd, uint16_t dwell,
                  uint8_t bDay, uint8_t bDim, uint8_t bNight, bool autoRot);
  // Wspolny clamp dla load() i saveTuning() — zeby oba dawaly IDENTYCZNIE poprawne
  // wartosci (inaczej blob z przyszlej/uszkodzonej wersji ominalby progi minimum).
  void clampTuning();

  // --- MQTT / Home Assistant (domyslnie WYLACZONE) ---
  // Prefix jest krotki celowo: wchodzi do kazdego retained pakietu discovery,
  // a bufor klienta MQTT ma tylko 512 B (patrz MqttClient.cpp).
  char mqttHost[40] = {};
  uint16_t mqttPort = 1883;
  char mqttUser[32] = {};
  char mqttPass[64] = {};
  char mqttPrefix[24] = "pogoda-gdynia";
  bool mqttEnabled = false;

  // --- czujniki BLE (Xiaomi LYWSD03MMC) ---
  // Fabryczny firmware szyfruje rozgłaszanie. Klucz (bindkey) wyciąga się z chmury
  // Xiaomi i jest PRYWATNY — dlatego siedzi wyłącznie tutaj, w NVS, nigdy w repo.
  // Czujnik z firmware pvvx/ATC nadaje otwartym tekstem i klucza nie potrzebuje.

  // BLE_SLOTS JEST zrodlem prawdy o rozmiarze tablicy: stoi PRZED nia i tablica
  // deklaruje sie jako ble[BLE_SLOTS]. Wczesniej stal tu literal [8], a stala
  // lezala kilkadziesiat linii nizej i nie definiowala NICZEGO: jej zmiana nie
  // ruszylaby tablicy, za to petle po BLE_SLOTS w Settings.cpp wyjechalyby poza
  // nia. Teraz jedna liczba rzadzi tablica i petlami naraz.
  static constexpr int BLE_SLOTS = 8;

  // Ile slotow uzytkownik moze realnie obsadzic - i to jest liczba, ktora ma
  // pokazywac panel. Historia i ekran maja miejsce na RoomHistory::ROOMS pokoi;
  // czujnik wpisany ponad ten limit dalby sie zapisac i NIGDY by sie nie pokazal,
  // bez zadnego komunikatu. Nadwyzka slotow zostaje w NVS jako zapas: stare wpisy
  // dalej sie czytaja i edytuja, ale nowych tam nie przydzielamy.
  static constexpr int BLE_USABLE = RoomHistory::ROOMS;
  static_assert(BLE_SLOTS >= BLE_USABLE,
                "tablica ble[] musi pomiescic wszystkie pokoje historii");

  struct BleCfg {
    char mac[18] = {};   // "a4:c1:38:54:f9:a9"
    char name[24] = {};  // "Łazienka Góra" — UTF-8, wiec 2 B na znak z ogonkiem
    uint8_t key[16] = {};
    bool hasKey = false;
  } ble[BLE_SLOTS];

  // --- Viessmann (piec) ---
  // Client ID jest PUBLICZNY (siedzi w kazdej instalacji PyViCare) — ale refresh
  // token juz nie: przez 180 dni daje pelny dostep do ogrzewania. Dlatego oba leza
  // wylacznie w NVS, nigdy w repo, i /api/state nie zwraca tokena — tylko flage.
  char viClientId[40] = {};
  char viRefresh[600] = {};      // JWT bywa dlugi
  char viInstallation[12] = {};  // cache — zeby nie pytac o to co odczyt
  char viGateway[20] = {};
  uint32_t viAuthAt = 0;         // epoch PIERWSZEJ autoryzacji (viLink) — informacyjny, NIE licznik
  // Epoch OSTATNIEGO udanego odswiezenia/rotacji refresh tokena (ustawia storeTokens()).
  // Refresh token Viessmanna ROTUJE przy kazdym odswiezeniu (~55 min) i dostaje swieze
  // 180 dni, wiec dopoki urzadzenie jest online token NIE wygasa. Realny "zapas do
  // przymusowej autoryzacji" = 180 dni MINUS czas od ostatniego odswiezenia (bufor na
  // wypadek dluzszego offline): daysLeft() liczy od TEGO pola, nie od viAuthAt. Online
  // stoi ~180 (viRefreshAt goni now co ~55 min); po dluzszym offline uczciwie odlicza.
  // Klucz NVS "virt". 0 = jeszcze nie autoryzowano -> daysLeft() zwraca -1.
  uint32_t viRefreshAt = 0;
  bool viEnabled = false;

  bool hasViessmann() const { return viEnabled && viClientId[0] != '\0' && viRefresh[0] != '\0'; }
  void viSave();

  // Odczyty licznika gazu wpisywane recznie — do weryfikacji, czy piec nie klamie.
  static constexpr int METERS = 8;
  struct MeterCfg { uint32_t day = 0; float m3 = 0.f; };
  MeterCfg meters[METERS];
  bool meterAdd(uint32_t day, float m3);
  bool meterDel(uint32_t day);
  void meterSave();

  // --- bramki BLE (Shelly) ---
  // LISTA, nie jeden host. Bluetooth nie ma sieci kratowej: kazdy czujnik musi
  // dosiegnac konkretnego odbiornika, a odbiorniki sie NIE dubluja - uzupelniaja.
  // Zmierzone u uzytkownika (dBm, wlasne radio wyswietlacza / bramka na pietrze):
  //   Lazienka  -72 / -70     Schody   -90 / -56     Salon -98 / -79
  //   Sypialnia -84 / -94     Biuro (parter) -98 / -98  <- nie slyszy go NIKT.
  // Kazdy czujnik ma dokladnie jednego opiekuna i zaden odbiornik nie zastepuje
  // drugiego. Biuro wymaga wiec bramki NA PARTERZE, a nie lepszej anteny.
  //
  // Trzy sloty: dwa sa potrzebne od zaraz (Shelly na pietrze + ESP32-C3 na
  // parterze z issue #12), trzeci to zapas na strych/garaz. Wiecej nie ma po co -
  // kazdy slot to osobny GET w netTask przy kazdym odpytaniu, a ekran statystyk
  // ma na bramki jeden wiersz.
  static constexpr int BLE_GW = 3;

  // SLOT 0 TO NADAL bleGwHost POD KLUCZEM NVS "blegw": ta sama nazwa pola, ten sam
  // klucz, to samo znaczenie co w v91. Dzieki temu jedyna skonfigurowana bramka
  // (192.168.0.102) przezywa OTA bez migracji i bez linijki kodu migrujacego - a
  // gdyby OtaGuard cofnal wersje, stara binarka czyta swoj klucz i dziala dalej.
  // Sloty 1..BLE_GW-1 leza pod NOWYMI kluczami "bgw1".."bgwN", ktorych stara
  // binarka nie zna i po prostu je ignoruje.
  //
  // Nie ma tu blobu, wiec nie ma pulapki "dwa uklady, ten sam rozmiar" (patrz
  // RoomHistory w Settings.cpp): kazdy slot to osobny klucz z osobnym stringiem.
  // Rozszerzenie listy = dopisanie kluczy, nigdy przemeblowanie istniejacych.
  //
  // Lista jest ZAGESZCZANA przy zapisie (bleGwSave), wiec slot 0 jest obsadzony
  // zawsze, gdy obsadzony jest ktorykolwiek. To nie kosmetyka: netTask
  // (pogoda-gdynia.ino) i ekran statystyk pytaja o bleGwHost[0] != '\0' i dzieki
  // zageszczaniu ten warunek dalej znaczy dokladnie "jest jakas bramka".
  char bleGwHost[24] = {};               // bramka 1 - klucz NVS "blegw"
  char bleGwHostN[BLE_GW - 1][24] = {};  // bramki 2..N - klucze "bgw1".."bgwN"

  // Jednolity dostep do calej listy. Poza lista zwraca "" - nigdy nullptr, zeby
  // wolajacy nie musial sprawdzac przed kazdym snprintf("%s").
  const char* bleGwAt(int i) const;
  int bleGwCount() const;
  bool hasBleGw() const { return bleGwCount() > 0; }

  // Sam sprawdzian, bez zapisu: panel ma przepuscic CALA liste, zanim ruszy
  // pierwszy slot. Inaczej literowka w trzecim polu zostawia dwa pierwsze zmienione
  // w RAM i niezapisane w NVS - netTask odpytuje juz nowy adres, a restart wraca
  // do starego i nikt nie wie, ktora wersja jest prawdziwa.
  static bool bleGwHostValid(const char* host);

  // bleGwSet pisze TYLKO do RAM i waliduje (host wchodzi prosto do URL-a).
  // NVS rusza dopiero bleGwSave() - inaczej zapis calej listy z panelu to trzy
  // osobne transakcje NVS, a zageszczanie w polowie petli mieszaloby sloty.
  bool bleGwSet(int i, const char* host);
  void bleGwSave();

  // BLE_SLOTS / BLE_USABLE stoja wyzej, przy samej tablicy ble[].
  const BleCfg* bleFind(const char* mac) const;
  // keyHex: 32 znaki hex albo "" (bez zmian) albo "-" (skasuj klucz).
  // false = brak wolnego slotu, czyli obsadzone juz BLE_USABLE czujnikow.
  bool bleSet(const char* mac, const char* name, const char* keyHex);

  bool hasWifi() const { return ssid[0] != '\0'; }
  bool hasInverter() const { return modbusHost[0] != '\0'; }
  bool hasMqtt() const { return mqttEnabled && mqttHost[0] != '\0'; }

  void load();
  void save();
  void clearWifi();
};

Settings& settings();

// --- profil produkcji PV z bieżącego dnia (trwały po zaniku zasilania) ---
void pvHistoryLoad(struct PvHistory& h);
void pvHistorySave(const struct PvHistory& h);
void pvHistoryClear();

// --- (v165) baza licznikow miernika z ostatniej polnocy (trwala po zaniku
// zasilania — DLATEGO NVS, a nie RTC; patrz komentarz przy definicji) ---
void pvMeterBaseLoad(struct PvMeterBase& b);
void pvMeterBaseSave(const struct PvMeterBase& b);

// --- (v166) TRWALA KOPIA STATYSTYK PIR + LDR (klucz "sen1") ------------------
// gPir i gLdr siedza w pamieci RTC, a ta przezywa OTA, panic i watchdog, ale NIE
// przezywa zaniku napiecia (patrz PirRtc w Log.h). Dokladnie to sie stalo 16.08.2026:
// wlasciciel wylaczyl zasilanie i wielotygodniowe zbiory wystartowaly od zera.
// Kopia w NVS jest LEKIEM NA ZANIK ZASILANIA i niczym wiecej — RTC zostaje pamiecia
// podreczna "na goraco", jego uklad pol i adresy sa NIETKNIETE.
//
// Ten stan zyje w DRAM przez cala sesje: czesc pol wraca z NVS przy starcie
// (savedEpoch/powerGapS/coldStarts), reszta opisuje TA sesje. Jedna instancja,
// dokladnie jak settings() — bo czytaja go trzy zadania: setup()/loop() (rdzen 1),
// netTask (zapis) i webTask (/api/diag).
struct SensStats {
  uint32_t savedEpoch = 0;    // epoch ostatniego zapisu WCZYTANY z NVS; 0 = nie wiem
                              // (zapisano, zanim NTP dal czas) i wtedy dlugosci przerwy
                              // bez pradu NIE DA sie policzyc — patrz sensStatsSave()
  uint32_t powerGapS = 0;     // sumaryczne sekundy BEZ ZASILANIA. NIE wchodza do
                              // collected_s (wtedy nic nie zbieralismy), ale musza byc
                              // widoczne, bo inaczej dziura w pomiarze wyglada jak
                              // czas zjedzony przez restarty
  uint32_t coldStarts = 0;    // ile razy zbiory wrocily z NVS po zaniku napiecia
  uint32_t saveOkAt = 0;      // millis() ostatniego UDANEGO zapisu, 0 = ani razu
  bool pirOk = false;         // kopia w NVS ma uklad pol PirRtc zgodny z tym firmware'em
  bool ldrOk = false;         // to samo dla LdrRtc — rozstrzygane OSOBNO, tak jak osobne
                              // sa magici obu struktur w RTC
  // TA sesja wstala na danych z NVS, a nie z RTC — OSOBNO dla kazdej struktury.
  // Rozbite na dwa pola, bo jedno klamaloby: przy samym podbiciu LDR_RTC_MAGIC
  // (uklad pol LDR-a) PIR wstaje z RTC, a LDR z zera — i dziennik PIR-a raportowalby
  // wtedy "odtworzone z NVS" o licznikach, ktore z NVS nie przyszly. `restored` to
  // suma logiczna obu, do jednego zdania w panelu.
  bool restoredPir = false;
  bool restoredLdr = false;
  bool restored = false;      // restoredPir || restoredLdr
  bool outagePending = false; // czekamy na pierwszy czas z NTP, zeby zmierzyc przerwe
  bool saveFailed = false;    // ostatni zapis do NVS sie NIE udal (pelna partycja?) —
                              // bez tego pola awaria zapisu jest niema az do zaniku pradu
};
SensStats& sensStats();

// Wczytuje kopie. Wskaznik rowny nullptr znaczy "tej struktury NIE odtwarzaj" — tak
// zglasza sie restart programowy, po ktorym RTC jest SWIEZSZY niz NVS. Zwraca true,
// gdy w NVS byl wazny blob (niezaleznie od tego, czy cokolwiek odtworzono).
bool sensStatsLoad(struct PirRtc* pir, struct LdrRtc* ldr);
// Zapis z netTask, poza gLock. Zwraca false, gdy NVS odmowilo — patrz saveFailed.
bool sensStatsSave(const struct PirRtc& pir, const struct LdrRtc& ldr);
// Rozmiar blobu w bajtach, do /api/diag — zeby liczba w panelu nie rozjechala sie
// z kodem przy najblizszej zmianie struktur.
uint32_t sensStatsBytes();

// --- historia czujnikow BLE (24 h, ruchome okno; przezywa zanik zasilania) ---
void roomHistoryLoad(struct RoomHistory& h);
void roomHistorySave(const struct RoomHistory& h);

// --- historia jakosci powietrza (7 dni, srednie dobowe; przezywa zanik zasilania) ---
// Ten sam wzorzec co RoomHistory: caly bufor do NVS pod wlasnym, krotkim kluczem "airh".
void airHistoryLoad(struct AirHistory& h);
void airHistorySave(const struct AirHistory& h);

// Dzienny log gazu. Bez utrwalania cala weryfikacja licznika byla martwa: dane
// zbierane co 3 min ginely przy kazdym restarcie, a porownanie z rachunkiem
// wymaga tygodni.
void gasHistoryLoad(struct GasHistory& g);
void gasHistorySave(const struct GasHistory& g);

// Profil doby palnika. Do v98 gBurner byl JEDYNYM profilem bez utrwalania — PV,
// pokoje i gaz maja swoje — i dokladnie dlatego "wykres pieca nie pamieta po
// resecie, a wykres fotowoltaiki pamieta". Zadnej innej przyczyny w tym nie ma.
void burnerHistoryLoad(struct BurnerHistory& b);
void burnerHistorySave(const struct BurnerHistory& b);
