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

  // --- wyglad interfejsu (V1 "klasyczny" / V2 "SCENA", przelaczane z panelu) ---
  // Tylko 1 albo 2 — kazda inna wartosc w NVS (urzadzenie czyste, blob z przyszlej
  // wersji) wraca do domyslnej w load(). Domyslnie 2: wlasciciel zamowil V2 jako
  // docelowy wyglad; V1 zostaje dostepny z panelu jako odwrot awaryjny, gdyby w
  // nowym stylu czegos zabraklo. Ekran VIEW_RETRO (indeks 0) NIE jest tym objety —
  // zostaje identyczny w obu wygladach (patrz WeatherUi::drawViewRetro).
  uint8_t theme = 2;
  // Zapisuje OD RAZU do NVS (jak viSave()/meterSave()/bleGwSave()) i tylko wtedy,
  // gdy t to 1 albo 2 — przelacznik w panelu ma dzialac natychmiast, samodzielnie,
  // bez klikania gdziekolwiek indziej "Zapisz". false = t byla spoza {1,2}, RAM i
  // NVS zostaja nietkniete.
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

  // Zapis OD RAZU do NVS (jak setTheme()/viSave()): osobne klucze, natychmiastowo,
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
  uint32_t viAuthAt = 0;         // epoch autoryzacji — refresh token zyje 180 dni
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

// --- historia czujnikow BLE (24 h, ruchome okno; przezywa zanik zasilania) ---
void roomHistoryLoad(struct RoomHistory& h);
void roomHistorySave(const struct RoomHistory& h);

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
