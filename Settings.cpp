#include "Settings.h"

#include <Arduino.h>
#include <Preferences.h>
#include <cstring>

#include "Log.h"        // PirRtc/LdrRtc + ich magici — trwala kopia statystyk ("sen1")
#include "PvData.h"
#include "GasMeter.h"
#include "RoomHistory.h"
#include "AirHistory.h"
#include "GraphBlob.h"  // (v194) wykres mocy ladowania na OLED — klucz "graf1"

namespace {
Settings gSettings;
constexpr const char* NS_CFG = "pogoda";
constexpr const char* NS_PV = "pvday";

// ======== (v168) NADZOR NAD ZAPISAMI DO NVS — patrz komentarz w Settings.h ====
NvsWriteStat gNvsStat[NVS_SLOT_COUNT];
const NvsWriteStat kNvsEmpty{};

// Kolejnosc MUSI odpowiadac enumowi NvsSlot. static_assert nizej pilnuje tylko
// dlugosci — przestawienia dwoch pozycji nie zlapie nic procz czytania, wiec przy
// dopisywaniu slotu dopisz go NA KONCU obu tablic, jak w enumie.
constexpr const char* kNvsKey[NVS_SLOT_COUNT] = {
    "prof2", "rh3", "burn2", "gas2", "airh", "mtr2", "sen1", "graf1",
    "pogoda/*", "otaguard/*"};
// Rozmiary blobow. Te same liczby pilnuja static_asserty przy kazdej strukturze
// nizej w tym pliku — gdy ktorys blob zmieni rozmiar, kompilacja padnie TAM,
// a nie tutaj, wiec ta tablica nie moze sie po cichu rozjechac z rzeczywistoscia.
constexpr uint16_t kNvsBytes[NVS_SLOT_COUNT] = {292, 872, 148, 128, 52, 32, 424, 136,
                                                0, 0};

static_assert(sizeof(kNvsKey) / sizeof(kNvsKey[0]) == NVS_SLOT_COUNT,
              "tablica nazw kluczy NVS rozjechala sie z enumem NvsSlot");
static_assert(sizeof(kNvsBytes) / sizeof(kNvsBytes[0]) == NVS_SLOT_COUNT,
              "tablica rozmiarow blobow NVS rozjechala sie z enumem NvsSlot");

// Jedyne miejsce, w ktorym rozstrzyga sie "zapis sie udal czy nie".
//
// DO DZIENNIKA TYLKO NA PRZEJSCIU STANU — to nie jest oszczednosc na estetyke.
// Bufor /api/log ma 3072 B (Log.cpp:7), czyli okno rzedu SZESCIU MINUT przy
// normalnym ruchu, a te zapisy leca co 5 minut. Wpis przy KAZDEJ nieudanej probie
// wypchnalby z bufora wszystko inne i awaria zapisu zamiotlaby wlasny kontekst —
// czyli dokladnie te linie, ktore pozwalaja zrozumiec, co sie dzialo naokolo.
// Trwaly slad zostaje w licznikach, ktore widac w /api/diag bez limitu czasu.
// Ten sam wzorzec, co przy "sen1" w v166 (saveFailed + sekcja sensors.persist).
//
// WYSCIG: gNvsStat pisza DWA zadania — netTask (historie, viSave) i webTask
// (zapis z panelu). Kolizja moze zgubic jeden przyrost licznika i nic wiecej:
// pola sa 16- i 32-bitowe, wiec na tym rdzeniu zapis jest niepodzielny, a `failed`
// jest ustawiane na te sama wartosc z obu stron. Blokada kosztowalaby wiecej
// (trzeba by ja brac WEWNATRZ sciezki zapisu do flash) niz warta jest dokladnosc
// licznika diagnostycznego. Ta sama zasada, co przy gSensStats w v166.
void nvsMark(uint8_t slot, bool ok) {
  if (slot >= NVS_SLOT_COUNT) return;
  NvsWriteStat& s = gNvsStat[slot];
  if (ok) {
    if (s.oks < 0xFFFF) ++s.oks;
    s.okAt = millis();
    if (s.failed) {
      s.failed = false;
      LOG("NVS: zapis \"%s\" znowu dziala (nieudanych od startu: %u)\n",
          kNvsKey[slot], static_cast<unsigned>(s.fails));
    }
    return;
  }
  if (s.fails < 0xFFFF) ++s.fails;
  if (!s.failed) {
    s.failed = true;
    LOG("NVS: ZAPIS \"%s\" NIEUDANY — te dane NIE przezyja restartu. "
        "Zajetosc partycji: /api/diag sekcja nvs\n",
        kNvsKey[slot]);
  }
}

// Zapis blobu Z KONTROLA WYNIKU. `putBytes` oddaje liczbe zapisanych bajtow, a 0
// znaczy, ze nvs_set_blob albo nvs_commit odmowilo (Preferences.cpp) — do v167
// ta liczba byla wszedzie ignorowana i awaria zapisu byla calkowicie niema.
bool nvsPutBytes(Preferences& p, uint8_t slot, const char* key, const void* data, size_t n) {
  const bool ok = p.putBytes(key, data, n) == n;
  nvsMark(slot, ok);
  return ok;
}

// Zapis napisu Z KONTROLA WYNIKU — do slotow zbiorczych, gdzie wynik zbiera sie
// przez `&=` przez cala funkcje i dopiero na koncu idzie do nvsMark().
//
// PULAPKA, KTORA TU MIESZKA: Preferences::putString zwraca strlen(value), wiec dla
// PUSTEGO napisu oddaje 0 TAKZE PO UDANYM ZAPISIE — a 0 to jednoczesnie kod bledu.
// Porownanie "> 0" uznaloby wiec kazde puste pole (mquser, mqpass, bgw1, bgw2,
// viinst na urzadzeniu bez pieca) za awarie i panel swiecilby na czerwono bez
// powodu. Jedyna poprawna kontrola to rownosc z dlugoscia tego, co zapisujemy.
bool putStrOk(Preferences& p, const char* key, const char* v) {
  return p.putString(key, v) == strlen(v);
}

// Klucze listy bramek. Slot 0 zostaje pod historycznym "blegw" (patrz Settings.h),
// reszta dostaje wlasne klucze. Jedno miejsce, bo zapisuja to DWIE funkcje:
// save() (calosc konfiguracji z panelu) i bleGwSave() (sama lista).
// (v168) Zwraca false, gdy ktorykolwiek z kluczy nie wszedl. Wolajacy dokleja ten
// wynik do swojego `&=` i dopiero on melduje o awarii — inaczej jeden zapis listy
// bramek zglosilby sie w dzienniku osobno od zapisu reszty konfiguracji, chociaz
// to ta sama operacja i ta sama przyczyna.
bool bleGwWrite(Preferences& prefs, const Settings& s) {
  bool ok = putStrOk(prefs, "blegw", s.bleGwHost);
  for (int i = 1; i < Settings::BLE_GW; ++i) {
    char k[8];
    snprintf(k, sizeof(k), "bgw%d", i);
    ok &= putStrOk(prefs, k, s.bleGwAt(i));
  }
  return ok;
}

// Host wchodzi bez zmian do "http://%s/script/1/ble", wiec przepuszczamy wylacznie
// to, z czego adres moze sie skladac. Bez tego spacja albo ukosnik w polu panelu
// robi z URL-a cos zupelnie innego niz uzytkownik widzi na ekranie.
bool bleGwHostOk(const char* h) {
  for (const char* p = h; *p != '\0'; ++p) {
    const char c = *p;
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '-' || c == ':';
    if (!ok) return false;
  }
  return true;
}
}  // namespace

// UWAGA - NIE dodawaj tu globalnego `Preferences`. Kazda funkcja MUSI miec wlasny,
// lokalny obiekt (wzorzec z OtaGuard.cpp:62,75,233) i MUSI sprawdzac begin().
//
// Dlaczego: te funkcje wolane sa z DWOCH watkow naraz. netTask robi
// roomHistorySave() co 10 min i viSave() co ~55 min, webTask robi save() przy
// kazdym "Zapisz" w panelu. `Preferences` nie jest re-entrantne: begin() na
// zajetym obiekcie zwraca false i ZOSTAWIA _handle na cudzej przestrzeni nazw,
// a putString() tego nie zauwaza (_started dalej == true) i pisze pod cudzy
// uchwyt. Skutkiem byly: haslo WiFi ladujace w przestrzeni "pvday", historia
// pokoi ginaca bez sladu i ustawienia "zapisane", ktore znikaly po restarcie.
// Cicho, bez crashu i bez wpisu w logu.
//
// Samo NVS (nvs_open/nvs_set_*/nvs_close) jest w ESP-IDF bezpieczne watkowo.
// Niebezpieczny byl wylacznie wspoldzielony obiekt C++ ze stanem _handle/_started.

Settings& settings() {
  return gSettings;
}

// --- (v168) odczyt nadzoru nad zapisami do NVS (do /api/diag) ----------------
const NvsWriteStat& nvsStat(uint8_t slot) {
  return slot < NVS_SLOT_COUNT ? gNvsStat[slot] : kNvsEmpty;
}

void nvsMarkWrite(uint8_t slot, bool ok) {
  nvsMark(slot, ok);
}

const char* nvsSlotKey(uint8_t slot) {
  return slot < NVS_SLOT_COUNT ? kNvsKey[slot] : "?";
}

uint16_t nvsSlotBytes(uint8_t slot) {
  return slot < NVS_SLOT_COUNT ? kNvsBytes[slot] : 0;
}

uint16_t nvsSlotEntries(uint8_t slot) {
  const uint16_t b = nvsSlotBytes(slot);
  if (b == 0) return 0;   // slot zbiorczy — nie ma JEDNEGO rozmiaru, patrz Settings.h
  // 2 wpisy narzutu (indeks blobu + naglowek fragmentu) + 1 wpis na kazde
  // rozpoczete 32 B danych. Zrodlo: opis nvs_set_blob w nvs.h ESP-IDF ("uses 2
  // overhead and 1 entry per each 32 bytes of new data").
  return static_cast<uint16_t>(2 + (b + 31) / 32);
}

void Settings::load() {
  Preferences prefs;
  // Na czystym urzadzeniu przestrzen jeszcze nie istnieje i begin(readOnly)
  // zwraca false. Wtedy zostaja wartosci domyslne z definicji struktury.
  if (!prefs.begin(NS_CFG, true)) {
    return;
  }
  String s = prefs.getString("ssid", "");
  String p = prefs.getString("pass", "");
  String c = prefs.getString("city", "Gdynia");
  String m = prefs.getString("mb", "");
  lat = prefs.getFloat("lat", 54.4870f);
  lon = prefs.getFloat("lon", 18.5216f);
  modbusPort = prefs.getUShort("mbport", 502);
  pvPeakW = prefs.getUShort("peak", 6000);
  otaEnabled = prefs.getBool("ota", true);
  // SKORKA WYSWIETLACZA — wlasny klucz "theme", jak kazde inne pole (zaden blob).
  // (v162) Klucz WRACA do odczytu razem z hakiem na skorki (patrz Settings.h).
  // TA JEDNA LINIA JEST CALYM ZABEZPIECZENIEM PRZED CZARNYM EKRANEM po aktualizacji
  // z v159: u wlasciciela pod tym kluczem NADAL LEZY 1 albo 2 (wycofane motywy V1/V2),
  // a getUChar odda te wartosc DOSLOWNIE. Sprowadzamy wiec KAZDA nielegalna wartosc
  // (1, 2, 0, smiec z uszkodzonego NVS) do jedynej istniejacej skorki. Gdyby tego tu
  // nie bylo, a rysowanie kiedykolwiek rozgalezilo sie po tym polu, urzadzenie
  // wpadloby w galaz nieistniejacego motywu — a jest tylko-OTA, wiec z czerni nie ma
  // jak wrocic. NIE UPRASZCZAC tego do samego getUChar(); przy dodawaniu drugiej
  // skorki rozszerz themeValid(), nie kasuj sprowadzania.
  const uint8_t th = prefs.getUChar("theme", THEME_PASMOWY);
  theme = themeValid(th) ? th : THEME_PASMOWY;

  // Ustawienia wyswietlacza (tryb nocny + rotacja + jasnosc). Domyslne = dawne stale
  // z Config.h; clampTuning() nizej pilnuje zakresow (m.in. TWARDE minimum jasnosci),
  // wiec nawet blob z uszkodzonego/przyszlego NVS nie zejdzie ponizej progu.
  nightStartH = prefs.getUChar("nstart", 22);
  nightEndH   = prefs.getUChar("nend", 6);
  dwellS      = prefs.getUShort("dwell", 9);
  blDay       = prefs.getUChar("blday", 255);
  blDim       = prefs.getUChar("bldim", 130);
  blNight     = prefs.getUChar("blnight", 45);
  // Auto-rotacja V3 — domyslnie false (spec 7a: dotyk przelacza, ekrany nie same).
  autoRotate  = prefs.getBool("arot", false);
  clampTuning();

  String mh = prefs.getString("mqhost", "");
  String mu = prefs.getString("mquser", "");
  String mp = prefs.getString("mqpass", "");
  String mx = prefs.getString("mqpre", "pogoda-gdynia");
  mqttPort = prefs.getUShort("mqport", 1883);
  mqttEnabled = prefs.getBool("mqen", false);

  String vc = prefs.getString("vicid", "");
  String vr = prefs.getString("viref", "");
  String vi = prefs.getString("viinst", "");
  String vg = prefs.getString("vigw", "");
  viAuthAt = prefs.getUInt("viat", 0);
  viRefreshAt = prefs.getUInt("virt", 0);   // ostatnie udane odswiezenie — patrz Settings.h
  // Bramka 1 czyta sie z tego samego klucza co zawsze - to CALA "migracja" z
  // pojedynczego hosta na liste. Sloty 2..N to nowe klucze; na urzadzeniu sprzed
  // OTA ich nie ma, wiec zostaja puste i lista ma dokladnie jedna, dzialajaca
  // pozycje: te, ktora uzytkownik wpisal.
  String bg = prefs.getString("blegw", "");
  strncpy(bleGwHost, bg.c_str(), sizeof(bleGwHost) - 1);
  for (int i = 1; i < BLE_GW; ++i) {
    char k[8];
    snprintf(k, sizeof(k), "bgw%d", i);
    String g = prefs.getString(k, "");
    strncpy(bleGwHostN[i - 1], g.c_str(), sizeof(bleGwHostN[i - 1]) - 1);
  }
  if (prefs.getBytesLength("mets") == sizeof(meters)) {
    prefs.getBytes("mets", meters, sizeof(meters));
  }
  viEnabled = prefs.getBool("vien", false);
  strncpy(viClientId, vc.c_str(), sizeof(viClientId) - 1);
  strncpy(viRefresh, vr.c_str(), sizeof(viRefresh) - 1);
  strncpy(viInstallation, vi.c_str(), sizeof(viInstallation) - 1);
  strncpy(viGateway, vg.c_str(), sizeof(viGateway) - 1);

  // czujniki BLE — bindkey jako blob (16 B), nigdy jako tekst w logach/API.
  // Czytamy WSZYSTKIE sloty, takze te ponad BLE_USABLE: jesli w NVS zostal wpis
  // z czasow luzniejszego limitu, ma sie dac odczytac i skasowac przez panel.
  for (int i = 0; i < BLE_SLOTS; ++i) {
    char k[8];
    snprintf(k, sizeof(k), "b%dmac", i);
    String bm = prefs.getString(k, "");
    snprintf(k, sizeof(k), "b%dnam", i);
    String bn = prefs.getString(k, "");
    strncpy(ble[i].mac, bm.c_str(), sizeof(ble[i].mac) - 1);
    strncpy(ble[i].name, bn.c_str(), sizeof(ble[i].name) - 1);

    snprintf(k, sizeof(k), "b%dkey", i);
    ble[i].hasKey = prefs.getBytesLength(k) == 16 &&
                    prefs.getBytes(k, ble[i].key, 16) == 16;
  }
  prefs.end();

  strncpy(ssid, s.c_str(), sizeof(ssid) - 1);
  strncpy(pass, p.c_str(), sizeof(pass) - 1);
  strncpy(city, c.c_str(), sizeof(city) - 1);
  strncpy(modbusHost, m.c_str(), sizeof(modbusHost) - 1);

  strncpy(mqttHost, mh.c_str(), sizeof(mqttHost) - 1);
  strncpy(mqttUser, mu.c_str(), sizeof(mqttUser) - 1);
  strncpy(mqttPass, mp.c_str(), sizeof(mqttPass) - 1);
  strncpy(mqttPrefix, mx.c_str(), sizeof(mqttPrefix) - 1);
  if (mqttPrefix[0] == '\0') {
    strncpy(mqttPrefix, "pogoda-gdynia", sizeof(mqttPrefix) - 1);
  }
}

void Settings::save() {
  Preferences prefs;
  if (!prefs.begin(NS_CFG, false)) {
    // (v168) Nieudane otwarcie przestrzeni to TEZ nieudany zapis — do v167 ta galaz
    // wracala w milczeniu i panel pokazywal "zapisano" po operacji, ktora nie
    // dotknela flasha. Liczy sie jako jedna nieudana proba slotu zbiorczego.
    nvsMark(NVS_SLOT_CFG, false);
    return;
  }
  bool ok = putStrOk(prefs, "ssid", ssid);
  ok &= putStrOk(prefs, "pass", pass);
  ok &= putStrOk(prefs, "city", city);
  ok &= putStrOk(prefs, "mb", modbusHost);
  ok &= prefs.putFloat("lat", lat) == sizeof(float);
  ok &= prefs.putFloat("lon", lon) == sizeof(float);
  ok &= prefs.putUShort("mbport", modbusPort) == sizeof(uint16_t);
  ok &= prefs.putUShort("peak", pvPeakW) == sizeof(uint16_t);
  ok &= prefs.putBool("ota", otaEnabled) == sizeof(uint8_t);
  ok &= putStrOk(prefs, "mqhost", mqttHost);
  ok &= putStrOk(prefs, "mquser", mqttUser);
  ok &= putStrOk(prefs, "mqpass", mqttPass);
  ok &= putStrOk(prefs, "mqpre", mqttPrefix);
  ok &= prefs.putUShort("mqport", mqttPort) == sizeof(uint16_t);
  ok &= prefs.putBool("mqen", mqttEnabled) == sizeof(uint8_t);
  ok &= bleGwWrite(prefs, *this);
  prefs.end();
  nvsMark(NVS_SLOT_CFG, ok);
}

// Osobno od save(): skorka klika sie z panelu niezaleznie od reszty formularzy
// (WiFi/MQTT/lokalizacja...), wiec nie ma powodu przy kazdym kliknieciu przepisywac
// do NVS calej reszty ustawien. Ten sam wzorzec co saveTuning()/viSave()/meterSave().
// (v162) WLASNY KLUCZ "theme", nie blob — dlatego przywrocenie tego pola po v160 nie
// przesunelo ani jednego innego ustawienia w NVS.
// Wartosci spoza themeValid() ODRZUCAMY (false), zamiast po cichu podmieniac na
// domyslna: panel ma dostac jasny blad, a nie wrazenie, ze zapisal cos, czego nie ma.
bool Settings::setTheme(uint8_t t) {
  if (!themeValid(t)) return false;
  theme = t;
  Preferences prefs;
  if (!prefs.begin(NS_CFG, false)) {
    nvsMark(NVS_SLOT_CFG, false);
    return false;
  }
  const bool ok = prefs.putUChar("theme", theme) == sizeof(uint8_t);
  prefs.end();
  nvsMark(NVS_SLOT_CFG, ok);
  // (v168) Zwracamy PRAWDE o zapisie, a nie stale true. Panel pokazuje po tym
  // "zapisano" — a przy pelnym NVS skorka wracalaby po restarcie do poprzedniej
  // i wygladalo by to na blad rysowania, nie na blad zapisu.
  return ok;
}

// Jedno zrodlo prawdy o zakresach ustawien wyswietlacza. Godziny do 0..23; czas
// ekranu do DWELL_MIN..DWELL_MAX; jasnosc podbijana do TWARDEGO minimum (gorna
// granica 255 jest darmowa — uint8_t). Bez zejscia ponizej progu ekranu nie da sie
// zgasic na stale, a urzadzenie w lazience nie ma jak wrocic z czerni.
void Settings::clampTuning() {
  if (nightStartH > 23) nightStartH = 23;
  if (nightEndH > 23) nightEndH = 23;
  if (dwellS < DWELL_MIN) dwellS = DWELL_MIN;
  if (dwellS > DWELL_MAX) dwellS = DWELL_MAX;
  if (blDay < BL_DAY_MIN) blDay = BL_DAY_MIN;
  if (blDim < BL_DIM_MIN) blDim = BL_DIM_MIN;
  if (blNight < BL_NIGHT_MIN) blNight = BL_NIGHT_MIN;
}

// Osobno od save(): ustawienia wyswietlacza zmienia sie z panelu niezaleznie od
// reszty formularzy, wiec nie ma po co przy kazdej zmianie przepisywac WiFi/MQTT.
// Ten sam wzorzec co viSave()/meterSave(). Clamp NAJPIERW — do NVS i do
// RAM trafiaja juz wartosci w zakresie, wiec panel czytajacy je z powrotem widzi
// PRAWDE (np. jasnosc podbita do minimum).
bool Settings::saveTuning(uint8_t nStart, uint8_t nEnd, uint16_t dwell,
                          uint8_t bDay, uint8_t bDim, uint8_t bNight, bool autoRot) {
  // (CORE-2) Clamp NA PARAMETRACH (kopia lokalna — argumenty i tak sa przekazane
  // przez wartosc) PRZED przypisaniem do pol. Stara kolejnosc (przypisz surowe,
  // dopiero potem clampTuning()) zostawiala okno, w ktorym pola Settings maja
  // wartosc SPOZA zakresu (np. nightStartH > 23) — czytelne z innego zadania (patrz
  // komentarz przy clampTuning() w OledPanel.cpp, ktory zaklada juz przyciete
  // wartosci). Te same stale, co w clampTuning() — to wciaz JEDNO zrodlo prawdy o
  // PROGACH (patrz komentarz w Settings.h), nie duplikat: clampTuning() zostaje
  // nizej jako koncowy, teraz juz idempotentny bezpiecznik.
  if (nStart > 23) nStart = 23;
  if (nEnd > 23) nEnd = 23;
  if (dwell < DWELL_MIN) dwell = DWELL_MIN;
  if (dwell > DWELL_MAX) dwell = DWELL_MAX;
  if (bDay < BL_DAY_MIN) bDay = BL_DAY_MIN;
  if (bDim < BL_DIM_MIN) bDim = BL_DIM_MIN;
  if (bNight < BL_NIGHT_MIN) bNight = BL_NIGHT_MIN;

  nightStartH = nStart;
  nightEndH   = nEnd;
  dwellS      = dwell;
  blDay       = bDay;
  blDim       = bDim;
  blNight     = bNight;
  autoRotate  = autoRot;   // bool — bez clampu; klucz NVS "arot"
  clampTuning();

  Preferences prefs;
  if (!prefs.begin(NS_CFG, false)) {
    nvsMark(NVS_SLOT_CFG, false);
    return false;
  }
  bool ok = prefs.putUChar("nstart", nightStartH) == sizeof(uint8_t);
  ok &= prefs.putUChar("nend", nightEndH) == sizeof(uint8_t);
  ok &= prefs.putUShort("dwell", dwellS) == sizeof(uint16_t);
  ok &= prefs.putUChar("blday", blDay) == sizeof(uint8_t);
  ok &= prefs.putUChar("bldim", blDim) == sizeof(uint8_t);
  ok &= prefs.putUChar("blnight", blNight) == sizeof(uint8_t);
  ok &= prefs.putBool("arot", autoRotate) == sizeof(uint8_t);
  prefs.end();
  nvsMark(NVS_SLOT_CFG, ok);
  return ok;
}

const char* Settings::bleGwAt(int i) const {
  if (i < 0 || i >= BLE_GW) return "";
  return (i == 0) ? bleGwHost : bleGwHostN[i - 1];
}

int Settings::bleGwCount() const {
  int n = 0;
  for (int i = 0; i < BLE_GW; ++i) {
    if (bleGwAt(i)[0] != '\0') ++n;
  }
  return n;
}

bool Settings::bleGwHostValid(const char* host) {
  const char* h = (host != nullptr) ? host : "";
  return strlen(h) < sizeof(Settings::bleGwHost) && bleGwHostOk(h);
}

// Sam RAM, bez NVS. Puste = slot skasowany.
bool Settings::bleGwSet(int i, const char* host) {
  if (i < 0 || i >= BLE_GW) return false;
  if (!bleGwHostValid(host)) return false;
  const char* h = (host != nullptr) ? host : "";

  char* dst = (i == 0) ? bleGwHost : bleGwHostN[i - 1];
  // snprintf, nie strcpy: OSTATNI bajt kazdego slotu ma zostac zerem NA ZAWSZE.
  // netTask czyta te tablice bez blokady, w trakcie zapisu z webTask - dopoki
  // bajt [23] jest zerem, czytajacy ZAWSZE znajdzie koniec stringa i najgorsze,
  // co go spotka, to jedno odpytanie polowy starego adresu.
  snprintf(dst, sizeof(bleGwHost), "%s", h);
  return true;
}

// Zageszcza liste i zapisuje ja w NVS. Osobno od save(), tak jak viSave()/meterSave():
// zapis bramki nie ma po co przepisywac hasla WiFi i calego MQTT.
void Settings::bleGwSave() {
  // Dziury wypadaja, kolejnosc zostaje, duplikaty gina. Duplikat to nie blad
  // uzytkownika bez konsekwencji: ten sam Shelly odpytywany dwa razy startuje
  // sam ze soba w wyborze opiekuna czujnika i marnuje sekundy w netTask.
  //
  // Zageszczanie ma jeszcze jedno zadanie: pilnuje, ze slot 0 (klucz "blegw",
  // czytany takze przez starsza binarke po cofnieciu OTA) trzyma PRAWDZIWA
  // bramke, a nie pustke, gdy user obsadzi tylko sloty 2-3.
  char tmp[BLE_GW][sizeof(bleGwHost)] = {};
  int n = 0;
  for (int i = 0; i < BLE_GW; ++i) {
    const char* h = bleGwAt(i);
    if (h[0] == '\0') continue;
    bool dup = false;
    for (int j = 0; j < n; ++j) {
      if (strcasecmp(tmp[j], h) == 0) { dup = true; break; }
    }
    if (!dup) snprintf(tmp[n++], sizeof(tmp[0]), "%s", h);
  }
  for (int i = 0; i < BLE_GW; ++i) {
    char* dst = (i == 0) ? bleGwHost : bleGwHostN[i - 1];
    snprintf(dst, sizeof(bleGwHost), "%s", tmp[i]);
  }

  Preferences prefs;
  if (!prefs.begin(NS_CFG, false)) {
    nvsMark(NVS_SLOT_CFG, false);
    return;
  }
  const bool ok = bleGwWrite(prefs, *this);
  prefs.end();
  nvsMark(NVS_SLOT_CFG, ok);
}

// Osobno od save(): refresh token zmienia sie co 180 dni, a IDy raz. Nie ma po co
// przepisywac calej konfiguracji przy kazdym odswiezeniu.
void Settings::viSave() {
  Preferences prefs;
  if (!prefs.begin(NS_CFG, false)) {
    nvsMark(NVS_SLOT_CFG, false);
    return;
  }
  bool ok = putStrOk(prefs, "vicid", viClientId);
  // (v168) NAJDROZSZY POJEDYNCZY KLUCZ W CALYM NVS. Bufor ma 600 B, a token
  // rzeczywiscie potrafi go zapelnic — to do 20 wpisow po 32 B, czyli tyle, ile
  // caly profil doby PV. Jesli kiedykolwiek zabraknie miejsca w partycji, ta linia
  // jest pierwszym podejrzanym i pierwszym kandydatem do skrocenia; dlatego wynik
  // jej zapisu MUSI byc widoczny, a nie tylko domniemany.
  ok &= putStrOk(prefs, "viref", viRefresh);
  ok &= putStrOk(prefs, "viinst", viInstallation);
  ok &= putStrOk(prefs, "vigw", viGateway);
  ok &= prefs.putUInt("viat", viAuthAt) == sizeof(uint32_t);
  // ostatnie udane odswiezenie — patrz Settings.h
  ok &= prefs.putUInt("virt", viRefreshAt) == sizeof(uint32_t);
  ok &= prefs.putBool("vien", viEnabled) == sizeof(uint8_t);
  prefs.end();
  nvsMark(NVS_SLOT_CFG, ok);
}

void Settings::meterSave() {
  Preferences prefs;
  if (!prefs.begin(NS_CFG, false)) {
    nvsMark(NVS_SLOT_CFG, false);
    return;
  }
  const bool ok = prefs.putBytes("mets", meters, sizeof(meters)) == sizeof(meters);
  prefs.end();
  nvsMark(NVS_SLOT_CFG, ok);
}

// Trzymamy posortowane po dacie. Ten sam dzien = nadpisanie (poprawka literowki).
bool Settings::meterAdd(uint32_t day, float m3) {
  if (day == 0 || m3 <= 0.f) return false;

  int slot = -1;
  for (int i = 0; i < METERS; ++i) {
    if (meters[i].day == day) { slot = i; break; }
  }
  if (slot < 0) {
    for (int i = 0; i < METERS; ++i) {
      if (meters[i].day == 0) { slot = i; break; }
    }
  }
  if (slot < 0) {   // pelno — wyrzucamy najstarszy
    slot = 0;
    for (int i = 1; i < METERS; ++i) {
      if (meters[i].day < meters[slot].day) slot = i;
    }
  }
  meters[slot].day = day;
  meters[slot].m3 = m3;

  for (int i = 0; i < METERS - 1; ++i) {
    for (int j = i + 1; j < METERS; ++j) {
      const bool swap = (meters[i].day == 0 && meters[j].day != 0) ||
                        (meters[j].day != 0 && meters[i].day != 0 && meters[j].day < meters[i].day);
      if (swap) { MeterCfg t = meters[i]; meters[i] = meters[j]; meters[j] = t; }
    }
  }
  meterSave();
  return true;
}

bool Settings::meterDel(uint32_t day) {
  for (int i = 0; i < METERS; ++i) {
    if (meters[i].day == day) {
      meters[i].day = 0;
      meters[i].m3 = 0.f;
      meterSave();
      return true;
    }
  }
  return false;
}

const Settings::BleCfg* Settings::bleFind(const char* mac) const {
  for (int i = 0; i < BLE_SLOTS; ++i) {
    if (ble[i].mac[0] != '\0' && strcasecmp(ble[i].mac, mac) == 0) return &ble[i];
  }
  return nullptr;
}

// keyHex: 32 znaki hex (bindkey z chmury Xiaomi) albo "" — czujnik z otwartym
// firmware klucza nie potrzebuje.
bool Settings::bleSet(const char* mac, const char* name, const char* keyHex) {
  if (mac == nullptr || mac[0] == '\0') return false;

  // Znany MAC edytujemy w jego slocie, gdziekolwiek stoi - takze ponad
  // BLE_USABLE, zeby stary wpis dalo sie poprawic albo skasowac.
  int slot = -1;
  for (int i = 0; i < BLE_SLOTS; ++i) {
    if (strcasecmp(ble[i].mac, mac) == 0) {
      slot = i;
      break;
    }
  }
  // NOWY czujnik tylko do slotu, ktory ma miejsce w historii i na ekranie.
  // Slot ponad limitem dalby sie wpisac i nigdy by sie nie pokazal: cicho,
  // bez komunikatu. Lepiej powiedziec "brak miejsca" niz udawac, ze zapisano.
  if (slot < 0) {
    for (int i = 0; i < BLE_USABLE; ++i) {
      if (ble[i].mac[0] == '\0') {
        slot = i;
        break;
      }
    }
  }
  if (slot < 0) return false;

  BleCfg& c = ble[slot];
  strncpy(c.mac, mac, sizeof(c.mac) - 1);
  c.mac[sizeof(c.mac) - 1] = '\0';
  strncpy(c.name, name ? name : "", sizeof(c.name) - 1);
  c.name[sizeof(c.name) - 1] = '\0';

  // Puste pole = BEZ ZMIAN (tak samo jak hasło MQTT). Wcześniej pusty klucz kasował
  // zapisany — czyli sama zmiana nazwy wywalała bindkey. Skasować można wpisując "-".
  if (keyHex != nullptr && strlen(keyHex) == 32) {
    for (int i = 0; i < 16; ++i) {
      char b[3] = {keyHex[i * 2], keyHex[i * 2 + 1], '\0'};
      c.key[i] = static_cast<uint8_t>(strtoul(b, nullptr, 16));
    }
    c.hasKey = true;
  } else if (keyHex != nullptr && strcmp(keyHex, "-") == 0) {
    memset(c.key, 0, sizeof(c.key));
    c.hasKey = false;
  }

  char k[8];
  Preferences prefs;
  if (!prefs.begin(NS_CFG, false)) {
    nvsMark(NVS_SLOT_CFG, false);
    return false;
  }
  snprintf(k, sizeof(k), "b%dmac", slot);
  bool ok = putStrOk(prefs, k, c.mac);
  snprintf(k, sizeof(k), "b%dnam", slot);
  ok &= putStrOk(prefs, k, c.name);
  snprintf(k, sizeof(k), "b%dkey", slot);
  if (c.hasKey) {
    ok &= prefs.putBytes(k, c.key, 16) == 16;
  } else {
    // remove() na NIEISTNIEJACYM kluczu zwraca false i to nie jest awaria —
    // czujnik bez bindkey nigdy go tu nie mial. Do wyniku NIE wchodzi.
    prefs.remove(k);
  }
  prefs.end();
  nvsMark(NVS_SLOT_CFG, ok);
  // (v168) Zwracamy PRAWDE o zapisie: panel po `true` wypisuje "czujnik zapisany",
  // a przy nieudanym zapisie nazwa i klucz wrocilyby po restarcie do poprzednich.
  return ok;
}

void Settings::clearWifi() {
  ssid[0] = '\0';
  pass[0] = '\0';
  Preferences prefs;
  if (!prefs.begin(NS_CFG, false)) {
    nvsMark(NVS_SLOT_CFG, false);
    return;
  }
  // KASOWANIE, nie zapis: remove() zwalnia wpisy, wiec nie ma jak zabraknac miejsca,
  // a false znaczy tu najczesciej "klucza i tak nie bylo" (urzadzenie bez WiFi).
  // Do licznikow nie wchodzi — inaczej pierwsze "zapomnij siec" na czystym
  // urzadzeniu zapalaloby w panelu awarie NVS.
  prefs.remove("ssid");
  prefs.remove("pass");
  prefs.end();
}

// ------------------------------------------------- profil produkcji PV -------
// Profil doby: 144 sloty po 10 minut, dwie serie (produkcja i pobor) + numer dnia.
//
// PULAPKA (to samo rodzenstwo co "rh2"): do v1 profil lezal w DWOCH blobach:
// "w" (produkcja) i "l" (pobor). Oba maja IDENTYCZNE 288 B, wiec kontrola
// `getBytesLength(klucz) == sizeof(...)` przepuszczala kazdy z nich w KAZDA
// strone. Literowka w kluczu albo zamiana kolejnosci przy zapisie zamienilaby
// produkcje z poborem i NIC by tego nie zlapalo: wykres pokazalby zolte slupki
// jako czerwone i odwrotnie. Cicha korupcja, nie crash.
//
// Teraz caly profil to JEDNA struktura pod JEDNYM kluczem "prof2". Jej rozmiar
// (292 B) nie jest podobny do zadnej ze skladowych, wiec pomylka o klucz jest
// niemozliwa, a pole `ver` lapie zmiane semantyki przy tym samym rozmiarze
// (np. przejscie z watow na dziesiatki watow).
//
// KAZDA zmiana ukladu tej struktury MUSI isc z NOWYM kluczem ("prof3"). Pilnuje
// tego static_assert nizej: gdy rozmiar sie zmieni, kompilacja padnie z ta
// instrukcja zamiast po cichu wczytac stary blob jako nowy.
namespace {

// ======== (v169) SKALA NIELINIOWA WATOW: uint16 -> jeden bajt ================
//
// POWOD: profil doby kosztowal 584 B = 21 wpisow NVS przy KAZDYM zapisie (co 5 min),
// a wolnych wpisow bylo 111 na siedem blobow o lacznym koszcie 123. Dwie serie po
// 144 probek to 576 z tych 584 bajtow — czyli caly problem siedzi w rozdzielczosci
// probki, nie w naglowku.
//
// DLACZEGO NIE SKALA LINIOWA: byla juz raz odrzucona i slusznie. Jeden bajt liniowo
// to krok 257 W przy zakresie do 65535 W, a przy dzielniku dobranym pod 6 kW (krok
// 24 W) kazdy szczyt poboru powyzej 6 kW (czajnik + piekarnik) zostalby SCIETY do
// 6 kW — wykres pokazalby plaski sufit tam, gdzie byl pik.
//
// SKALA NIELINIOWA usuwa jedno i drugie, bo blad odwzorowania ma byc mierzony
// W PIKSELACH, a nie w watach: wykres ma 62 px wysokosci (v3Pv: ch = 64, slupek
// (ch-2)) i skaluje sie do szczytu doby, wiec 1 px to peak/62 watow.
//
//   kody   0..150 -> krok  10 W (0..1500 W)      blad <=   5 W
//   kody 150..240 -> krok  50 W (1500..6000 W)   blad <=  25 W
//   kody 240..255 -> krok 1000 W (6000..21000 W) blad <= 500 W
//
// BLAD W PIKSELACH przy realnych szczytach doby:
//   * pochmurny zimowy dzien, peak 400 W  -> 62 * 5/400   = 0,78 px
//   * przecietny dzien,       peak 2000 W -> 62 * 25/2000 = 0,78 px
//   * peak 6000 W (moc instalacji)        -> 62 * 25/6000 = 0,26 px
//   * dzien ze szczytem poboru 9000 W     -> 62 * 500/9000 = 3,4 px, ale WYLACZNIE
//     na slupkach powyzej 6 kW; wszystko ponizej dalej ma blad ponizej 1/3 piksela.
// Czyli w kazdym normalnym dniu roznicy NIE DA SIE zobaczyc, a scinania nie ma az do
// 21 kW — przy instalacji 6 kW i przylaczu jednofazowym taka probka nie wystapila.
//
// GDZIE TO DZIALA: WYLACZNIE na drodze do NVS i z powrotem. W RAM PvHistory dalej
// trzyma pelne uint16, wiec wykres ogladany na zywo jest co do wata dokladny;
// kwantyzacja dotyczy tego, co przezylo restart.
//
// ZAOKRAGLANIE JEST STABILNE: pvWattCode(pvWattValue(c)) == c dla kazdego kodu, wiec
// profil wczytany z NVS i zapisany z powrotem nie dryfuje z kazdym cyklem.
uint16_t pvWattValue(uint8_t c) {
  if (c <= 150) return static_cast<uint16_t>(c) * 10;
  if (c <= 240) return static_cast<uint16_t>(1500 + (c - 150) * 50);
  return static_cast<uint16_t>(6000 + (c - 240) * 1000);
}

uint8_t pvWattCode(uint16_t w) {
  if (w >= 21000) return 255;
  if (w <= 1500) return static_cast<uint8_t>((w + 5) / 10);
  if (w <= 6000) return static_cast<uint8_t>(150 + (w - 1500 + 25) / 50);
  return static_cast<uint8_t>(240 + (w - 6000 + 500) / 1000);
}

struct PvProfileBlob {
  uint16_t ver;
  int16_t day;                        // tm_yday (0..365) — int32 bylo marnotrawstwem
  uint8_t watts[PvHistory::SLOTS];    // kody skali nieliniowej, patrz pvWattValue()
  uint8_t load[PvHistory::SLOTS];
};

constexpr uint16_t PV_PROF_VER = 2;
constexpr const char* K_PV_PROF = "prof2";

static_assert(sizeof(PvProfileBlob) == 292,
              "zmienil sie uklad profilu PV - podbij klucz NVS na \"prof3\", "
              "inaczej stary blob wczyta sie jako nowy (cicha korupcja)");

// --- uklad v1 ("prof1", 584 B, surowe uint16) — TYLKO DO MIGRACJI ------------
// Czytany RAZ, przy pierwszym starcie po aktualizacji, zeby wlasciciel nie stracil
// wykresu z biezacej doby. Potem klucz leci z partycji: 21 wpisow, ktorych nikt juz
// nie przeczyta, a wlasnie o kazdy wpis toczy sie ta gra.
struct PvProfileBlobV1 {
  uint16_t ver;
  int32_t day;
  uint16_t watts[PvHistory::SLOTS];
  uint16_t load[PvHistory::SLOTS];
};
static_assert(sizeof(PvProfileBlobV1) == 584, "uklad v1 profilu PV mial 584 B");
constexpr const char* K_PV_PROF_V1 = "prof1";

// Klucze ukladu v1. Nigdy juz nie beda czytane, a zajmuja ~580 B w malej
// partycji NVS (min_spiffs). Kasujemy je raz, przy pierwszym starcie po zmianie.
void pvRemoveLegacy(Preferences& p) {
  p.remove("w");
  p.remove("l");
  p.remove("day");
  // (v169) "prof1" dolacza do tej listy: jest juz przepisany na "prof2" (albo byl
  // nieczytelny), a zajmuje 21 wpisow NVS, ktorych zaden kod juz nie przeczyta.
  p.remove(K_PV_PROF_V1);
}

}  // namespace

void pvHistoryLoad(PvHistory& h) {
  h.reset(-1);

  Preferences prefs;
  if (!prefs.begin(NS_PV, true)) {
    // Na czystym urzadzeniu przestrzen jeszcze nie istnieje — to normalny pierwszy
    // start, nie awaria. Ale po restarcie urzadzenia, ktore chodzi od miesiecy, ta
    // sama galaz znaczy juz cos zupelnie innego, wiec nie wolno jej przemilczec.
    LOG("PV: przestrzen NVS \"%s\" niedostepna do odczytu — profil doby pusty\n", NS_PV);
    return;
  }
  PvProfileBlob b{};
  // Dlugosc W OSOBNEJ ZMIENNEJ, zeby dalo sie ja wypisac w galezi bledu. Bez tego
  // "nie wczytalem" nie odroznia braku klucza (0 B) od blobu w innym ukladzie.
  const size_t len = prefs.getBytesLength(K_PV_PROF);
  bool ok = len == sizeof(b) &&
            prefs.getBytes(K_PV_PROF, &b, sizeof(b)) == sizeof(b) &&
            b.ver == PV_PROF_VER && b.day >= 0;
  // (v169) MIGRACJA Z "prof1": nie kasujemy wykresu wlasciciela po cichu. Stary blob
  // (584 B, surowe uint16) czytamy RAZ i przepisujemy na skale nieliniowa. Pierwszy
  // zapis (za najwyzej 5 minut) utrwali go juz pod "prof2"; stary klucz kasujemy
  // nizej, zeby jego 21 wpisow wrocilo do puli.
  bool migrated = false;
  if (!ok && prefs.getBytesLength(K_PV_PROF_V1) == sizeof(PvProfileBlobV1)) {
    PvProfileBlobV1 v1{};
    if (prefs.getBytes(K_PV_PROF_V1, &v1, sizeof(v1)) == sizeof(v1) && v1.ver == 1 &&
        v1.day >= 0) {
      b.ver = PV_PROF_VER;
      b.day = static_cast<int16_t>(v1.day);
      for (int i = 0; i < PvHistory::SLOTS; ++i) {
        b.watts[i] = pvWattCode(v1.watts[i]);
        b.load[i] = pvWattCode(v1.load[i]);
      }
      ok = true;
      migrated = true;
    }
  }
  const bool legacy = prefs.isKey("w") || prefs.isKey("l") || prefs.isKey("day") ||
                      prefs.isKey(K_PV_PROF_V1);
  prefs.end();

  if (ok) {
    for (int i = 0; i < PvHistory::SLOTS; ++i) {
      h.watts[i] = pvWattValue(b.watts[i]);
      h.load[i] = pvWattValue(b.load[i]);
    }
    h.day = b.day;
    int slots = 0;
    for (int i = 0; i < PvHistory::SLOTS; ++i) {
      h.filled[i] = h.watts[i] > 0 || h.load[i] > 0;
      if (h.filled[i]) ++slots;
    }
    // (v168) LOG(), NIE Serial.printf. TO JEST TA JEDNA LINIA, KTOREJ ZABRAKLO
    // 17.08.2026: po restarcie zniknal caly dzienny wykres PV i nie dalo sie
    // rozstrzygnac, czy profil nie wszedl do NVS, czy nie wyszedl — bo urzadzenie
    // wisi na scianie BEZ USB, a Serial nie trafia do /api/log (Log.cpp pisze do
    // bufora kolowego dopiero z logPrintf). Dzien i LICZBA SLOTOW rozstrzygaja
    // spor w jednym zdaniu: `dzien` inny niz dzisiejszy znaczy "w NVS lezal profil
    // z wczoraj, wiec ZAPIS zawiodl", a `slotow 0` przy dzisiejszej dacie znaczy
    // "zapis szedl, ale byl pusty".
    LOG("PV: wczytano z NVS profil doby — dzien %d, slotow z danymi %d/%d%s\n",
        static_cast<int>(b.day), slots, PvHistory::SLOTS,
        migrated ? " (przepisany ze starego klucza \"prof1\")" : "");
  } else {
    // GALAZ, KTORA DO v167 BYLA CALKOWICIE NIEMA — a to ONA opisuje awarie.
    // Bez niej brak wpisu o wczytaniu znaczyl jednoczesnie "nie wczytalem" i
    // "wczytalem, ale piszę do Serial, ktorego nikt nie widzi". Teraz milczenie
    // w tym miejscu jest niemozliwe.
    LOG("PV: BRAK profilu doby w NVS — wykres startuje pusty (dlugosc klucza "
        "\"%s\": %u B, oczekiwano %u B)\n",
        K_PV_PROF, static_cast<unsigned>(len), static_cast<unsigned>(sizeof(b)));
  }

  // (v169) NOWY BLOB ZAPISUJEMY OD RAZU, JESZCZE PRZED SKASOWANIEM STAREGO.
  // Kolejnosc jest tu cala trescia: gdyby zapis czekal na zwykla kadencje (5 minut),
  // a stary klucz zniknal teraz, restart w tym oknie zostawilby wlasciciela BEZ
  // profilu — z danymi skasowanymi przez migracje, ktora mial ich nie stracic.
  if (migrated) {
    pvHistorySave(h);
  }
  if (legacy) {
    Preferences w;
    if (w.begin(NS_PV, false)) {
      pvRemoveLegacy(w);
      w.end();
      LOG("PV: skasowano profil w starym ukladzie (klucze w/l/day/prof1) — "
          "odzyskane do 21 wpisow NVS\n");
    }
  }
}

void pvHistorySave(const PvHistory& h) {
  Preferences prefs;
  if (!prefs.begin(NS_PV, false)) {
    nvsMark(NVS_SLOT_PROF, false);
    return;
  }
  PvProfileBlob b{};
  b.ver = PV_PROF_VER;
  b.day = static_cast<int16_t>(h.day);
  for (int i = 0; i < PvHistory::SLOTS; ++i) {
    b.watts[i] = pvWattCode(h.watts[i]);
    b.load[i] = pvWattCode(h.load[i]);
  }
  nvsPutBytes(prefs, NVS_SLOT_PROF, K_PV_PROF, &b, sizeof(b));
  prefs.end();
}

// -------------------------------- (v165) baza licznikow miernika -------------
// "Dzis" z licznikow NARASTAJACYCH = odczyt biezacy minus odczyt z ostatniej
// polnocy, wiec ta baza jest jedyna rzecza, ktora trzeba przechowac miedzy dobami.
//
// DLACZEGO NVS, A NIE RTC: 16.08.2026 wlasciciel wylaczyl zasilanie i wszystkie
// zbiory w pamieci RTC wystartowaly od zera. RTC przezywa restart, OTA i panic,
// ale NIE przezywa zaniku napiecia — a baza, ktora znika razem z pradem, jest
// bezuzyteczna dokladnie w dniu, w ktorym prad byl wylaczony.
//
// WLASNY KLUCZ, NIE DOKLEJANIE DO "prof2": profil doby (292 B) jest zapisywany
// co 5 minut, a baza zmienia sie RAZ NA DOBE. Wspolny blob oznaczalby albo
// przepisywanie bazy 288 razy dziennie bez potrzeby, albo — gorzej — zmiane
// rozmiaru PvProfileBlob, ktora wywalilaby static_assert i wymusila migracje
// klucza profilu (utrata wykresu doby). Osobny klucz kosztuje 32 B.
//
// (v169) Rozmiary blobow w przestrzeni "pvday" sa parami rozne (prof2 = 292,
// rh3 = 872, gas2 = 128, burn2 = 148, mtr2 = 32, airh = 52, sen1 = 424), wiec
// pomylka o klucz nie przejdzie przez kontrole getBytesLength() — ten sam wzorzec,
// co przy pozostalych. Ta rozlacznosc jest WARUNKIEM, a nie przypadkiem: przy
// dodawaniu blobu sprawdz te liste.
namespace {

// (v169) UKLAD v2: doszedl OSTATNI UDANY ODCZYT licznikow (stempel + oba stany).
// To on pozwala po restarcie zbudowac baze z odczytu SPRZED polnocy zamiast czekac
// na pierwszy udany odczyt po polnocy — patrz dlugi komentarz przy PvMeterBase.
//
// KOLEJNOSC POL DOBRANA POD ROZMIAR, nie pod czytelnosc: najpierw czworki, potem
// dwojki, na koncu bajty. Struktura ma przez to 32 B zamiast 36 — a 32 B to granica
// miedzy TRZEMA a CZTEREMA wpisami NVS (2 narzutu + 1 na kazde rozpoczete 32 B).
// `year` schodzi na uint16 (kontrola 2020..2200 i tak byla), `yday` na int16.
struct PvMeterBlob {
  uint32_t lastEpoch;      // epoch ostatniego udanego odczytu; 0 = brak
  float importKwh;         // stan rej. 37121 przyjety za baze doby
  float exportKwh;         // stan rej. 37119 przyjety za baze doby
  float lastImportKwh;     // rej. 37121 z chwili lastEpoch
  float lastExportKwh;     // rej. 37119 z chwili lastEpoch
  uint16_t year;           // 2020..2200 — patrz kontrola w pvMeterBaseLoad
  int16_t yday;            // tm_yday dnia, KTOREGO dotyczy baza
  int16_t minute;          // minuta doby lokalnej odczytu-bazy (0..1439)
  int16_t offsetMin;       // odleglosc od polnocy ZE ZNAKIEM (ujemna = sprzed polnocy)
  uint8_t ver;
  uint8_t flags;           // bit0 = full, bity 1-3 = PvBaseEvent, ktory ustawil baze
};

constexpr uint8_t PV_METER_VER = 2;
constexpr const char* K_PV_METER = "mtr2";
constexpr uint8_t PV_METER_FLAG_FULL = 0x01;

static_assert(sizeof(PvMeterBlob) == 32,
              "zmienil sie uklad bazy licznikow - podbij klucz NVS na \"mtr3\". "
              "UWAGA: 32 B to granica trzech wpisow NVS, 33 B kosztuje juz cztery");

// --- uklad v1 ("mtr1", 24 B) — TYLKO DO MIGRACJI ----------------------------
struct PvMeterBlobV1 {
  uint16_t ver;
  int32_t year;
  int32_t yday;
  float importKwh;
  float exportKwh;
  int16_t minute;
  uint8_t full;
  uint8_t pad;
};
static_assert(sizeof(PvMeterBlobV1) == 24, "uklad v1 bazy licznikow mial 24 B");
constexpr const char* K_PV_METER_V1 = "mtr1";

}  // namespace

void pvMeterBaseLoad(PvMeterBase& b) {
  b = PvMeterBase{};

  Preferences prefs;
  if (!prefs.begin(NS_PV, true)) {
    return;
  }
  PvMeterBlob blob{};
  bool migrated = false;
  bool ok = prefs.getBytesLength(K_PV_METER) == sizeof(blob) &&
            prefs.getBytes(K_PV_METER, &blob, sizeof(blob)) == sizeof(blob) &&
            blob.ver == PV_METER_VER;
  // (v169) MIGRACJA Z "mtr1": baza z ostatniej polnocy to jedyna rzecz, ktora
  // pozwala ekranowi PRAD pokazac "dzis" z licznikow — porzucenie jej kosztowaloby
  // wlasciciela cala dobe na calce. Pola, ktorych uklad v1 nie mial (ostatni odczyt),
  // zostaja puste; pierwszy udany odczyt je uzupelni.
  if (!ok && prefs.getBytesLength(K_PV_METER_V1) == sizeof(PvMeterBlobV1)) {
    PvMeterBlobV1 v1{};
    if (prefs.getBytes(K_PV_METER_V1, &v1, sizeof(v1)) == sizeof(v1) && v1.ver == 1) {
      blob = PvMeterBlob{};
      blob.ver = PV_METER_VER;
      blob.year = static_cast<uint16_t>(v1.year < 0 ? 0 : (v1.year > 65535 ? 65535 : v1.year));
      blob.yday = static_cast<int16_t>(v1.yday);
      blob.minute = v1.minute;
      // Uklad v1 nie znal offsetu — baza mogla powstac WYLACZNIE z odczytu po
      // polnocy, wiec offset jest rowny minucie i to nie jest domysl, tylko
      // wlasnosc tamtego kodu.
      blob.offsetMin = v1.minute;
      blob.importKwh = v1.importKwh;
      blob.exportKwh = v1.exportKwh;
      blob.flags = v1.full ? PV_METER_FLAG_FULL : 0;
      ok = true;
      migrated = true;
    }
  }
  const bool legacy = prefs.isKey(K_PV_METER_V1);
  prefs.end();
  if (!ok) {
    // Stary klucz kasujemy TAKZE tutaj: skoro nic z niego nie wyszlo, to sa trzy
    // wpisy NVS trzymane bez powodu. Ale dopiero PO tym, jak proba odczytu padla —
    // nigdy przed.
    if (legacy) {
      Preferences w;
      if (w.begin(NS_PV, false)) {
        w.remove(K_PV_METER_V1);
        w.end();
      }
    }
    return;
  }
  // Kontrola sensownosci PRZED przyjeciem bazy. Baza ze smieciem (ujemny licznik,
  // yday spoza zakresu) jest gorsza niz jej brak: brak konczy sie jednodniowym
  // zjazdem do calki, a smiec — roznica rzedu tysiecy kWh podpisana "dzis".
  if (blob.yday < 0 || blob.yday > 365 || blob.year < 2020 || blob.year > 2200 ||
      blob.importKwh < 0.f || blob.exportKwh < 0.f || blob.minute < 0 ||
      blob.minute > 1439) {
    // (v168) LOG(), nie Serial: to jest komunikat o UTRACIE danych wlasciciela
    // (ekran wraca na dobe do calki z v164), a Serial nie trafia do /api/log.
    LOG("PV: baza licznikow w NVS niespojna — ignoruje (dzis liczone z calki)\n");
    return;
  }
  b.year = blob.year;
  b.yday = blob.yday;
  b.importKwh = blob.importKwh;
  b.exportKwh = blob.exportKwh;
  b.minute = blob.minute;
  b.offsetMin = blob.offsetMin;
  b.full = (blob.flags & PV_METER_FLAG_FULL) != 0;
  b.event = static_cast<uint8_t>((blob.flags >> 1) & 0x07);
  b.valid = true;
  // (v169) Ostatni udany odczyt. Kontrola jest tu OSOBNA i celowo lagodniejsza niz
  // przy samej bazie: gdy ostatni odczyt jest smieciem, tracimy tylko mozliwosc
  // siegniecia po odczyt sprzed polnocy (baza dalej dziala), wiec zerujemy go
  // zamiast odrzucac cala baze.
  if (blob.lastEpoch >= 1700000000UL && blob.lastImportKwh >= 0.f &&
      blob.lastExportKwh >= 0.f) {
    b.lastEpoch = blob.lastEpoch;
    b.lastImportKwh = blob.lastImportKwh;
    b.lastExportKwh = blob.lastExportKwh;
  }
  // (v168) TA linia ZOSTAJE na Serial i to jest swiadomy wybor, a nie przeoczenie:
  // udana baza jest w calosci widoczna w /api/diag jako pv.meter.base (rok, dzien,
  // godzina zlapania, oba liczniki, flaga `full`), i to bez ograniczenia czasowego
  // bufora dziennika. Do /api/log przenosimy tylko te komunikaty startowe, ktorych
  // /api/diag NIE potrafi odtworzyc — czyli galezie AWARII wyzej.
  Serial.printf(
      "PV: baza licznikow z NVS: %ld dzien %ld min %d (od polnocy %+d min) pobor %.2f "
      "oddane %.2f%s\n",
      static_cast<long>(b.year), static_cast<long>(b.yday), b.minute,
      static_cast<int>(b.offsetMin), b.importKwh, b.exportKwh,
      b.full ? "" : " (NIEPELNA)");

  // (v169) Nowy blob NAJPIERW, kasowanie starego POTEM — patrz pvHistoryLoad.
  // Tu okno bylo najdluzsze ze wszystkich: baza zapisuje sie przy zdarzeniu zmiany,
  // czyli raz na dobe, wiec bez tego zapisu restart miedzy migracja a polnoca kasowal
  // baze i ekran PRAD wracal do calki na cala dobe.
  if (migrated) {
    pvMeterBaseSave(b);
    Preferences w;
    if (w.begin(NS_PV, false)) {
      w.remove(K_PV_METER_V1);
      w.end();
    }
  }
}

// Wolane po zdarzeniu zmiany bazy (SET_FIRST/ROLLED/WENT_BACK) ORAZ — od v169 —
// co cfg::PV_METER_STORE_MS, zeby utrwalic OSTATNI UDANY ODCZYT.
//
// DLACZEGO TEN DRUGI ZAPIS ISTNIEJE: bez niego pole `lastEpoch` zyje wylacznie
// w RAM i ginie przy kazdym restarcie. Urzadzenie restartuje sie czesto (17.08.2026
// licznik pokazywal osiem startow w dobie), a restart trafiajacy w okolice polnocy
// kasowal jedyny odczyt sprzed polnocy — czyli dokladnie te dana, dla ktorej cala
// ta naprawa powstala. Z utrwaleniem co 15 minut najgorszy przypadek to odczyt
// starszy o 15 minut, a nie jego brak.
//
// KOSZT: 32 B co 15 min = 96 zapisow na dobe = ~3 kB/dobe wobec ~250 kB/dobe, ktore
// ten sam netTask juz pisze profilami — czyli ponizej 1,5% ruchu do flasha. W wpisach
// NVS to 3 wpisy na zapis, najtanszy blob w calej partycji.
void pvMeterBaseSave(const PvMeterBase& b) {
  if (!b.valid) {
    return;
  }
  Preferences prefs;
  if (!prefs.begin(NS_PV, false)) {
    nvsMark(NVS_SLOT_METER, false);
    return;
  }
  PvMeterBlob blob{};
  blob.ver = PV_METER_VER;
  blob.year = static_cast<uint16_t>(b.year < 0 ? 0 : (b.year > 65535 ? 65535 : b.year));
  blob.yday = static_cast<int16_t>(b.yday);
  blob.importKwh = b.importKwh;
  blob.exportKwh = b.exportKwh;
  blob.minute = b.minute;
  blob.offsetMin = b.offsetMin;
  blob.flags = static_cast<uint8_t>((b.full ? PV_METER_FLAG_FULL : 0) |
                                    ((b.event & 0x07) << 1));
  blob.lastEpoch = b.lastEpoch;
  blob.lastImportKwh = b.lastImportKwh;
  blob.lastExportKwh = b.lastExportKwh;
  nvsPutBytes(prefs, NVS_SLOT_METER, K_PV_METER, &blob, sizeof(blob));
  prefs.end();
}

// NIE WOLNO tu wolac prefs.clear(). W przestrzeni "pvday" siedzi TAKZE "rh2",
// czyli 24 h historii z czujnikow BLE. clear() skasowalby ja przy okazji i nikt
// by sie o tym nie dowiedzial. Kasujemy wylacznie klucze profilu PV, po nazwie.
void pvHistoryClear() {
  Preferences prefs;
  if (!prefs.begin(NS_PV, false)) {
    return;
  }
  prefs.remove(K_PV_PROF);
  pvRemoveLegacy(prefs);
  prefs.end();
}

// ---------------------------------------- historia czujnikow BLE (24 h) -------
// Blob ma ~1,7 kB i leci do NVS co 10 minut — tak samo jak profil PV.
// Zapisujemy CALY bufor razem z numerem slotu; bez niego po restarcie nie dalo by
// sie stwierdzic, ktore probki sa jeszcze wazne.

// PULAPKA: klucz to "rh2", nie "rh". Uklad v1 (4 pokoje: temperatura + wilgotnosc)
// i v2 (6 pokoi: sama temperatura) maja PRZYPADKIEM identyczny rozmiar — 1736 B.
// Kontrola getBytesLength() przepuscilaby stary blob i wczytala wilgotnosc jako
// temperature pokoi 4-5. Nowy klucz = stara historia jest po prostu ignorowana.
//
// Rozmiar jest tu JEDYNYM zabezpieczeniem, a wlasnie sie okazalo, ze potrafi nie
// zauwazyc zmiany ukladu. Asercja nizej lapie to w kompilacji: jesli RoomHistory
// sie zmieni, budowanie padnie z instrukcja, zamiast po cichu wczytac stary blob.
static_assert(sizeof(RoomHistory) == 1736,
              "zmienil sie uklad RoomHistory - podbij klucz NVS na \"rh4\". "
              "Sam rozmiar NIE wystarczy: v1 (4 pokoje T+RH) i v2 (6 pokoi T) "
              "mialy identyczne 1736 B i kontrola rozmiarem ich nie rozroznila");

namespace {

// ======== (v169) TEMPERATURA POKOJU W JEDNYM BAJCIE ==========================
//
// POWOD: "rh2" to byl NAJDROZSZY blob w partycji — 1736 B = 57 wpisow NVS przy
// kazdym zapisie (co 10 min), przy 111 wolnych wpisach na wszystko. Sam ten jeden
// zapis zjadal polowe dostepnej puli; zbieg z profilem PV, palnikiem i statystykami
// wyczerpywal ja calkowicie i to jest zmierzona przyczyna cichych porazek zapisu.
//
// CO SIE ZMIENIA: probka schodzi z int16 (0,1 stopnia) na uint8 (0,5 stopnia)
//   kod 0..160 -> -20,0 .. +60,0 stopnia C, krok 0,5
//   kod 255    -> BRAK POMIARU (odpowiednik RoomHistory::NO_T)
// 6 pokoi x 144 probek to 864 B zamiast 1728 B — polowa blobu i 30 wpisow zamiast 57.
//
// DLACZEGO 0,5 STOPNIA WYSTARCZA: wykres pokoi ma 26 px wysokosci i skaluje sie do
// rozpietosci WSZYSTKICH pokoi z doby. Realna rozpietosc w mieszkaniu to 4-8 stopni,
// czyli 1 px odpowiada 0,15-0,3 stopnia — a blad kwantyzacji to najwyzej 0,2 stopnia
// (probka w RAM jest calkowita w dziesiatych czesciach stopnia, wiec najgorsze
// zaokraglenie do wielokrotnosci 0,5 to dwie dziesiate, nie dwie i pol), czyli
// najwyzej jeden piksel. Przy czujniku na balkonie rozpietosc doby siega
// 15-20 stopni, wiec 1 px to 0,6-0,8 stopnia i blad jest juz o rzad wielkosci
// ponizej piksela. Zakres -20..+60 obejmuje z zapasem i mroz, i nasloneczniona
// scianę; SAMYCH LICZB z historii nigdzie nie wypisujemy (kafelki biora wartosc
// biezaca prosto z czujnika, nie z tej tablicy), wiec 0,5 stopnia nie ma gdzie
// wyjsc jako "21,5 zamiast 21,3".
//
// GDZIE TO DZIALA: wylacznie na drodze do NVS i z powrotem. RoomHistory w RAM dalej
// trzyma int16 w dziesiatych czesciach stopnia i wykres na zywo jest dokladny;
// kwantyzacja dotyczy tego, co przezylo restart.
//
// STABILNOSC: roomTempValue(roomTempCode(v)) == v dla kazdego kodu, wiec historia
// wczytana z NVS i zapisana z powrotem nie dryfuje z kazdym cyklem.
constexpr uint8_t ROOM_T_NONE = 255;

uint8_t roomTempCode(int16_t t10) {
  if (t10 == RoomHistory::NO_T) return ROOM_T_NONE;
  int32_t v = static_cast<int32_t>(t10) + 200;   // -20,0 C -> 0
  if (v < 0) v = 0;
  v = (v + 2) / 5;                               // zaokraglenie do 0,5 stopnia
  if (v > 160) v = 160;
  return static_cast<uint8_t>(v);
}

int16_t roomTempValue(uint8_t c) {
  if (c > 160) return RoomHistory::NO_T;
  return static_cast<int16_t>(static_cast<int32_t>(c) * 5 - 200);
}

struct RoomBlob {
  uint32_t lastSlot;
  uint16_t ver;
  int16_t head;
  uint8_t t[RoomHistory::ROOMS][RoomHistory::SLOTS];
};

constexpr uint16_t ROOM_VER = 3;
constexpr const char* K_ROOM = "rh3";
constexpr const char* K_ROOM_V2 = "rh2";

static_assert(sizeof(RoomBlob) == 872,
              "zmienil sie uklad historii pokoi - podbij klucz NVS na \"rh4\", "
              "inaczej stary blob wczyta sie jako nowy (cicha korupcja)");

}  // namespace

void roomHistoryLoad(RoomHistory& h) {
  Preferences prefs;
  if (!prefs.begin(NS_PV, true)) {
    h.reset();
    LOG("BLE: przestrzen NVS \"%s\" niedostepna — historia 24 h pusta\n", NS_PV);
    return;
  }
  const size_t need = sizeof(RoomBlob);
  const size_t len = prefs.getBytesLength(K_ROOM);
  bool ok = false;
  bool migrated = false;
  if (len == need) {
    RoomBlob b{};
    if (prefs.getBytes(K_ROOM, &b, need) == need && b.ver == ROOM_VER) {
      for (int r = 0; r < RoomHistory::ROOMS; ++r) {
        for (int i = 0; i < RoomHistory::SLOTS; ++i) {
          h.t10[r][i] = roomTempValue(b.t[r][i]);
        }
      }
      h.lastSlot = b.lastSlot;
      h.head = b.head;
      ok = true;
    }
  } else if (prefs.getBytesLength(K_ROOM_V2) == sizeof(RoomHistory)) {
    // (v169) MIGRACJA Z "rh2": stary blob ma DOKLADNIE uklad RoomHistory, wiec
    // czytamy go PROSTO DO `h` — bez bufora posredniego, ktory kosztowalby 1736 B
    // stosu w setup(). Kwantyzacja nastapi przy pierwszym zapisie (za <= 10 min).
    // Nie porzucamy 24 h pomiarow tylko dlatego, ze zmienil sie format.
    prefs.getBytes(K_ROOM_V2, &h, sizeof(RoomHistory));
    ok = true;
    migrated = true;
  }
  if (ok) {
    // (v168) LOG() z tego samego powodu, co przy profilu PV: to najwiekszy blob
    // w partycji i gdy zabraknie miejsca, przestanie sie miescic PIERWSZY.
    // Bez tej linii jego zniknieciu towarzyszylaby cisza.
    LOG("BLE: wczytano z NVS historie 24 h (slot %lu)%s\n",
        static_cast<unsigned long>(h.lastSlot),
        migrated ? " — przepisana ze starego klucza \"rh2\"" : "");
  } else {
    h.reset();
    LOG("BLE: BRAK historii 24 h w NVS — wykres pokoi startuje pusty "
        "(dlugosc \"%s\": %u B, oczekiwano %u B)\n",
        K_ROOM, static_cast<unsigned>(len), static_cast<unsigned>(need));
  }
  // (v168) ODZYSK MIEJSCA: klucz "rh" z ukladu v1 (4 pokoje, temperatura +
  // wilgotnosc) zostal porzucony w v92 na rzecz "rh2" i od tamtej pory NIKT go
  // nie kasowal — w odroznieniu od kluczy profilu PV, ktore maja pvRemoveLegacy().
  // Na urzadzeniu, ktore chodzi od czasow sprzed v92, ten blob wciaz zajmuje
  // 1736 B danych = 57 wpisow NVS po 32 B = 1824 B, czyli DZIEWIEC PROCENT calej
  // partycji 20480 B, i nie jest czytany przez zaden kod tej wersji ani zadnej
  // wczesniejszej od v92.
  //
  // TO NIE JEST KASOWANIE DANYCH WLASCICIELA: obecne firmware nie ma jak tego
  // blobu zinterpretowac (uklad pol jest inny, a klucz "rh2" celowo powstal po to,
  // zeby "rh" NIGDY nie zostal wczytany — patrz PULAPKA wyzej). To sa bajty, ktore
  // od v92 sa wylacznie balastem. Porzucamy je swiadomie i z wpisem w dzienniku,
  // dokladnie tak, jak pvRemoveLegacy() robi to z kluczami "w"/"l"/"day".
  // (v169) Do listy dolacza "rh2": jest juz przepisany na "rh3" (albo byl
  // nieczytelny) i kosztuje 57 wpisow, czyli najwiecej ze wszystkiego, co w tej
  // partycji lezy. To ta sama decyzja, co przy "rh" — porzucamy swiadomie i z
  // wpisem w dzienniku, a nie po cichu.
  const bool legacy = prefs.isKey("rh") || prefs.isKey(K_ROOM_V2);
  prefs.end();
  // (v169) Nowy blob NAJPIERW, kasowanie starego POTEM — patrz pvHistoryLoad.
  if (migrated) {
    roomHistorySave(h);
  }
  if (legacy) {
    Preferences w;
    if (w.begin(NS_PV, false)) {
      w.remove("rh");
      w.remove(K_ROOM_V2);
      w.end();
      LOG("BLE: skasowano historie w starych ukladach (klucze \"rh\", \"rh2\") — "
          "odzyskane do 114 wpisow NVS\n");
    }
  }
}

void roomHistorySave(const RoomHistory& h) {
  Preferences prefs;
  if (!prefs.begin(NS_PV, false)) {
    nvsMark(NVS_SLOT_ROOMS, false);
    return;
  }
  // Blob (872 B) budowany na stosie netTask, ktory ma tu ~8,5 kB zapasu
  // (diag mem.stack_net_spare) — a wolajacy trzyma juz kopie RoomHistory (1736 B),
  // wiec razem to nadal ponizej jednej trzeciej zapasu.
  RoomBlob b{};
  b.ver = ROOM_VER;
  b.lastSlot = h.lastSlot;
  b.head = h.head;
  for (int r = 0; r < RoomHistory::ROOMS; ++r) {
    for (int i = 0; i < RoomHistory::SLOTS; ++i) {
      b.t[r][i] = roomTempCode(h.t10[r][i]);
    }
  }
  nvsPutBytes(prefs, NVS_SLOT_ROOMS, K_ROOM, &b, sizeof(b));
  prefs.end();
}

// -------------------------------------- historia jakosci powietrza (7 dni) ----
// Ten sam wzorzec, co RoomHistory: caly bufor (52 B) do NVS pod wlasnym kluczem
// "airh", w tej samej przestrzeni "pvday". 52 B raz na 10 min to koszt pomijalny.
// Rozmiar jest jedynym zabezpieczeniem przed wczytaniem cudzego/starego blobu, wiec
// asercja pilnuje ukladu: gdy AirHistory sie zmieni, kompilacja padnie z instrukcja
// podbicia klucza — zamiast po cichu wczytac blob o innym znaczeniu.
static_assert(sizeof(AirHistory) == 52,
              "zmienil sie uklad AirHistory - podbij klucz NVS na \"airh2\" "
              "(kontrola dlugoscia getBytesLength nie rozrozni blobow tej samej dlugosci)");

void airHistoryLoad(AirHistory& h) {
  Preferences prefs;
  if (!prefs.begin(NS_PV, true)) {
    h.reset();
    return;
  }
  const size_t need = sizeof(AirHistory);
  if (prefs.getBytesLength("airh") == need) {
    prefs.getBytes("airh", &h, need);
    Serial.printf("Powietrze: wczytano historie 7 dni (dzien %lu)\n",
                  static_cast<unsigned long>(h.lastDay));
  } else {
    h.reset();
  }
  prefs.end();
}

void airHistorySave(const AirHistory& h) {
  Preferences prefs;
  if (!prefs.begin(NS_PV, false)) {
    nvsMark(NVS_SLOT_AIR, false);
    return;
  }
  nvsPutBytes(prefs, NVS_SLOT_AIR, "airh", &h, sizeof(AirHistory));
  prefs.end();
}

// ------------------------------- (v194) wykres mocy ladowania (OLED) ----------
// Ten sam wzorzec, co przy "airh" wyzej: caly bufor pod jednym krotkim kluczem,
// pole wersji w blobie, asercja rozmiaru przy strukturze (GraphBlob.h).
//
// KADENCJA ZAPISU: raz na dopisana probke, czyli co 3 minuty — ale TYLKO W TRAKCIE
// LADOWANIA, bo poza sesja probki w ogole nie powstaja. Realnie to kilkadziesiat
// zapisow na dobe, a nie 480, i to jest cala roznica miedzy tym blobem a np. "prof2",
// ktory pisze sie caly dzien.
//
// O ZBIEGANIU SIE ZAPISOW (lekcja z 17.08, notatka nvs-i-pamiec.md): wtedy caly cykl
// potrzebowal 123 wpisow przy 111 DOSTEPNYCH i zapisy wywracaly sie, gdy wypadaly
// w tej samej chwili — lekarstwem bylo rozsuniecie ich w fazie. Dzis dostepnych jest
// 4187, a ten blob kosztuje 7, wiec zbieg okresow nie ma jak zabraknac miejsca.
// Zostawiam to zapisane, zeby nikt nie "naprawial" tego przez rzadszy zapis: notatka
// wymienia rzadszy zapis wprost jako ruch w zla strone — poszerza okno utraty danych,
// zamiast je zwezac.
bool graphBlobLoad(GraphBlob& b) {
  Preferences prefs;
  if (!prefs.begin(NS_PV, true)) return false;
  bool ok = false;
  if (prefs.getBytesLength("graf1") == sizeof(GraphBlob)) {
    prefs.getBytes("graf1", &b, sizeof(GraphBlob));
    // Wersja I zakres licznika sprawdzane OSOBNO, bo znacza co innego: obca wersja
    // to blob z innego ukladu pol, a cnt > 128 to blob wlasciwej wersji, ale
    // uszkodzony. Oba konczy sie tak samo (odrzuceniem), lecz mylenie ich w jednym
    // warunku ukrywaloby, ktory przypadek naprawde zaszedl.
    ok = (b.ver == 1) && (b.cnt <= 128);
    if (!ok) {
      Serial.printf("Wykres OLED: blob odrzucony (ver=%u cnt=%u)\n",
                    static_cast<unsigned>(b.ver), static_cast<unsigned>(b.cnt));
    }
  }
  prefs.end();
  return ok;
}

void graphBlobSave(const GraphBlob& b) {
  Preferences prefs;
  if (!prefs.begin(NS_PV, false)) {
    nvsMark(NVS_SLOT_GRAPH, false);
    return;
  }
  nvsPutBytes(prefs, NVS_SLOT_GRAPH, "graf1", &b, sizeof(GraphBlob));
  prefs.end();
}

// ------------------------------------------ dzienny log gazu (120 dni) --------
// Bez tego cala weryfikacja licznika nie ma sensu: gGas zbieral dane od zawsze,
// ale ginely przy KAZDYM restarcie — a porownanie "licznik vs piec" wymaga
// tygodni historii. Zbieranie bez zapisu to byl najgorszy mozliwy stan: kod
// wygladal na dzialajacy, koszt RAM byl placony, pozytku zero.
//
// Ten sam wzorzec co przy profilu PV: wersja w blobie, wlasny klucz, asercja
// rozmiaru. Blob ma (v169) 128 B, wiec zapis jest tani — leci raz na dobe, przy
// przewinieciu dnia, a nie co 3 minuty.
namespace {

// ======== (v169) DOBOWE ZUZYCIE GAZU W JEDNYM BAJCIE =========================
//
// POWOD: 120 dni po uint16 to 240 B = 10 wpisow NVS. Rozdzielczosc 0,01 m3 na DOBE
// jest tu fikcja: sam piec oddaje currentDay z dokladnoscia 0,1 m3 (patrz GasMeter.h),
// wiec dwa miejsca po przecinku pochodza z zaokraglenia, a nie z pomiaru.
//
// SKALA NIELINIOWA, dokladnie jak przy watach, bo rozstrzepanie dobowego zuzycia
// jest ogromne: 0,4 m3 w lipcu i 30 m3 w styczniu.
//   kody   0..100 -> krok 0,05 m3 (0..5 m3)     blad <= 0,02 m3
//   kody 100..160 -> krok 0,25 m3 (5..20 m3)    blad <= 0,12 m3
//   kody 160..200 -> krok 1,00 m3 (20..60 m3)   blad <= 0,50 m3
//   kod  200      -> 60 m3 i wiecej (sufit)
// (Bledy sprawdzone przebiegiem po CALYM zakresie 0..6000 setnych m3, a nie
// policzone z krokow — dlatego 0,02 i 0,12, a nie 0,025 i 0,125: wejscie jest
// calkowite w setnych m3, wiec polowa kroku nigdy nie wypada dokladnie.)
//
// CO TO ZNACZY DLA WERYFIKACJI RACHUNKU, bo tylko po to ten log istnieje:
// zaokraglamy do NAJBLIZSZEGO punktu siatki, wiec bledy pojedynczych dob znosza sie
// wzajemnie, a nie kumuluja. Skrajny, nierealny przypadek "kazda doba okresu
// rozliczeniowego wypada 0,5 m3 obok, zawsze w te sama strone" to przy 60 dobach po
// >20 m3 blad 30 m3 na rachunku ~1200 m3, czyli 2,5%; realny (dobowe zuzycie ponizej
// 5 m3, blad 0,02 m3, znaki losowe) to okolo 0,2 m3 na cale dwa miesiace.
// Rozdzielczosc odczytu licznika u dostawcy i tak wynosi 1 m3.
uint16_t gasValueX100(uint8_t c) {
  if (c <= 100) return static_cast<uint16_t>(c) * 5;
  if (c <= 160) return static_cast<uint16_t>(500 + (c - 100) * 25);
  if (c <= 200) return static_cast<uint16_t>(2000 + (c - 160) * 100);
  return 6000;
}

uint8_t gasCode(uint16_t x100) {
  if (x100 >= 6000) return 200;
  if (x100 <= 500) return static_cast<uint8_t>((x100 + 2) / 5);
  if (x100 <= 2000) return static_cast<uint8_t>(100 + (x100 - 500 + 12) / 25);
  return static_cast<uint8_t>(160 + (x100 - 2000 + 50) / 100);
}

struct GasBlob {
  uint32_t lastDay;
  uint16_t ver;
  int16_t head;
  uint8_t m3[GasHistory::DAYS];   // kody skali nieliniowej, patrz gasValueX100()
};
constexpr uint16_t GAS_VER = 2;
constexpr const char* K_GAS = "gas2";

static_assert(sizeof(GasBlob) == 128,
              "zmienil sie uklad logu gazu - podbij klucz NVS na \"gas3\"");

// --- uklad v1 ("gas1", 252 B, surowe uint16) — TYLKO DO MIGRACJI ------------
struct GasBlobV1 {
  uint16_t ver;
  uint32_t lastDay;
  int16_t head;
  uint16_t m3x100[GasHistory::DAYS];
};
static_assert(sizeof(GasBlobV1) == 252, "uklad v1 logu gazu mial 252 B");
constexpr const char* K_GAS_V1 = "gas1";

}  // namespace

void gasHistoryLoad(GasHistory& g) {
  g.reset();
  Preferences prefs;
  if (!prefs.begin(NS_PV, true)) {
    return;
  }
  GasBlob b{};
  bool migrated = false;
  if (prefs.getBytesLength(K_GAS) == sizeof(b) &&
      prefs.getBytes(K_GAS, &b, sizeof(b)) == sizeof(b) && b.ver == GAS_VER) {
    g.lastDay = b.lastDay;
    g.head = b.head;
    for (int i = 0; i < GasHistory::DAYS; ++i) g.m3x100[i] = gasValueX100(b.m3[i]);
    Serial.printf("Gaz: wczytano log (dzien %lu)\n", static_cast<unsigned long>(g.lastDay));
  } else if (prefs.getBytesLength(K_GAS_V1) == sizeof(GasBlobV1)) {
    // (v169) MIGRACJA Z "gas1": to sa MIESIACE zbieranych danych, jedyna podstawa
    // porownania pieca z rachunkiem — porzucenie ich kosztowaloby wlasciciela cztery
    // miesiace czekania na nowy komplet.
    GasBlobV1 v1{};
    if (prefs.getBytes(K_GAS_V1, &v1, sizeof(v1)) == sizeof(v1) && v1.ver == 1) {
      migrated = true;
      g.lastDay = v1.lastDay;
      g.head = v1.head;
      // Przez kody, a nie 1:1: inaczej pierwsze zapisane doby mialyby dokladnosc
      // 0,01 m3, a kazda nastepna 0,05 m3 i suma okresu mieszalaby dwie skale.
      for (int i = 0; i < GasHistory::DAYS; ++i) {
        g.m3x100[i] = gasValueX100(gasCode(v1.m3x100[i]));
      }
      LOG("Gaz: log przepisany ze starego klucza \"gas1\" (dzien %lu) — "
          "odzyskane 10 wpisow NVS\n",
          static_cast<unsigned long>(g.lastDay));
    }
  }
  const bool legacy = prefs.isKey(K_GAS_V1);
  prefs.end();
  // (v169) TU TA KOLEJNOSC JEST NAJWAZNIEJSZA W CALYM PLIKU. Log gazu zapisuje sie
  // normalnie RAZ NA DOBE (przy przewinieciu dnia), wiec bez tego zapisu od razu
  // stary klucz zniknalby teraz, a nowy powstal dopiero o polnocy — kazdy restart
  // w tym oknie kasowalby CZTERY MIESIACE danych, ktore sluza do sprawdzania rachunku.
  if (migrated) {
    gasHistorySave(g);
  }
  if (legacy) {
    Preferences w;
    if (w.begin(NS_PV, false)) {
      w.remove(K_GAS_V1);
      w.end();
    }
  }
}

void gasHistorySave(const GasHistory& g) {
  Preferences prefs;
  if (!prefs.begin(NS_PV, false)) {
    nvsMark(NVS_SLOT_GAS, false);
    return;
  }
  GasBlob b{};
  b.ver = GAS_VER;
  b.lastDay = g.lastDay;
  b.head = g.head;
  for (int i = 0; i < GasHistory::DAYS; ++i) b.m3[i] = gasCode(g.m3x100[i]);
  nvsPutBytes(prefs, NVS_SLOT_GAS, K_GAS, &b, sizeof(b));
  prefs.end();
}

// ------------------------------------------- profil doby palnika (144 sloty) --
// Ostatni profil bez utrwalania. PV zapisuje sie co 5 minut, pokoje co 10, gaz raz
// na dobe — a palnik nie zapisywal sie NIGDY. Wykres pieca gasl przy kazdym
// restarcie i to jest cala tajemnica "fotowoltaika pamieta, piec nie".
//
// FILLED IDZIE DO BLOBU NA ZAPAS — DZIS BEZ OBSERWOWALNEGO SKUTKU.
// Przy PV odtwarzamy filled z danych (`filled[i] = watts[i] > 0 || load[i] > 0`).
// Tu daloby sie tak samo i nikt by nie zauwazyl: jedyny konsument to drawGasChart
// (WeatherUi.cpp:2323), ktory pomija slot warunkiem `!h.filled[s] || h.mod[s] == 0` —
// a `mod[s] == 0` i tak pomija ten sam slot, push() zas ustawia filled wszedzie, gdzie
// mod > 0. Czyli `!filled[s]` jest dzis warunkiem MARTWYM. peak() nie jest wolane wcale.
// NIE SZUKAJ TU LOGIKI, KTORA TO WYKORZYSTUJE — nie ma jej.
//
// Pole zostaje na potrzeby PLANOWANEGO wykresu: mod == 0 znaczy "palnik zmierzony,
// stal", i to jest pelnoprawny pomiar, ktory przeprojektowany wykres bedzie chcial
// odroznic od "nie bylo odczytu" (bez tego cala noc bez odpytow wyglada identycznie
// jak noc, w ktora piec stal). (v169) Od tego wydania to pole nie kosztuje w NVS
// ANI JEDNEGO BAJTU: mieszka jako wartownik 255 w bajcie modulacji (patrz nizej).
//
// Ten sam wzorzec, co przy "gas2" i "prof2": wlasny klucz, pole `ver` w blobie,
// asercja rozmiaru. Rozmiary blobow w przestrzeni "pvday" sa rozne (prof2 = 292,
// rh3 = 872, gas2 = 128, burn2 = 148), wiec pomylka o klucz nie ma jak przejsc
// przez kontrole getBytesLength().
namespace {

// (v169) `filled` NIE MA JUZ WLASNEJ TABLICY — MIESZKA W WARTOSCI MODULACJI.
// Komentarz wyzej mowi, po co to pole istnieje (planowany wykres ma odroznic
// "palnik zmierzony, stal" od "nie bylo odczytu") i ta informacja zostaje w calosci.
// Zmienia sie sposob zapisu: modulacja ma zakres 0..100, wiec 155 wartosci bajtu
// stalo pustych. Kod 255 znaczy "NIE BYLO ODCZYTU" i zastepuje cala 144-bajtowa
// tablice filled[].
//
// DLACZEGO NIE MASKA BITOWA: sprawdzone, nie zgadniete. Maska 18 B daje blob 166 B,
// a to nadal SZESC rozpoczetych blokow po 32 B, czyli 8 wpisow NVS. Wartownik w
// bajcie modulacji daje 148 B = piec blokow = 7 wpisow. Jeden wpis roznicy przesadza
// o tym, czy suma cyklu miesci sie w polowie dostepnej puli — a informacji nie tracimy
// ani na jotę, w odroznieniu od odtwarzania filled z `mod > 0` (tak robi PvHistory,
// i tam wlasnie ginie roznica miedzy "zmierzone zero" a "brak pomiaru").
constexpr uint8_t BURN_MOD_NONE = 255;

struct BurnerBlob {
  uint16_t ver;
  int16_t day;                            // tm_yday (0..365)
  // 0..100 = zmierzona modulacja w procentach, 255 = nie bylo odczytu w tym slocie
  uint8_t mod[BurnerHistory::SLOTS];
};
constexpr uint16_t BURN_VER = 2;
constexpr const char* K_BURN = "burn2";

static_assert(sizeof(BurnerBlob) == 148,
              "zmienil sie uklad profilu palnika - podbij klucz NVS na \"burn3\", "
              "inaczej stary blob wczyta sie jako nowy (cicha korupcja)");

// --- uklad v1 ("burn1", 296 B) — TYLKO DO MIGRACJI --------------------------
struct BurnerBlobV1 {
  uint16_t ver;
  int32_t day;
  uint8_t mod[BurnerHistory::SLOTS];
  uint8_t filled[BurnerHistory::SLOTS];
};
static_assert(sizeof(BurnerBlobV1) == 296, "uklad v1 profilu palnika mial 296 B");
constexpr const char* K_BURN_V1 = "burn1";

}  // namespace

void burnerHistoryLoad(BurnerHistory& h) {
  h.reset(-1);
  Preferences prefs;
  if (!prefs.begin(NS_PV, true)) {
    return;
  }
  BurnerBlob b{};
  bool migrated = false;
  bool ok = prefs.getBytesLength(K_BURN) == sizeof(b) &&
            prefs.getBytes(K_BURN, &b, sizeof(b)) == sizeof(b) && b.ver == BURN_VER &&
            b.day >= 0;
  // (v169) MIGRACJA Z "burn1": ten profil zyje jedna dobe, wiec strata boli mniej niz
  // przy gazie — ale to nadal wykres, ktory wlasciciel wlasnie oglada, a przepisanie
  // kosztuje kilkanascie linii. `filled` znika jako tablica i wchodzi w wartownika.
  if (!ok && prefs.getBytesLength(K_BURN_V1) == sizeof(BurnerBlobV1)) {
    BurnerBlobV1 v1{};
    if (prefs.getBytes(K_BURN_V1, &v1, sizeof(v1)) == sizeof(v1) && v1.ver == 1 &&
        v1.day >= 0) {
      b = BurnerBlob{};
      b.ver = BURN_VER;
      b.day = static_cast<int16_t>(v1.day);
      for (int i = 0; i < BurnerHistory::SLOTS; ++i) {
        b.mod[i] = v1.filled[i] != 0 ? (v1.mod[i] > 100 ? 100 : v1.mod[i])
                                     : BURN_MOD_NONE;
      }
      ok = true;
      migrated = true;
    }
  }
  if (ok) {
    // Przez petle, nie memcpy: `filled` w BurnerHistory to bool[], a bool o wartosci
    // innej niz 0/1 (choćby ze smiecia w NVS) to zachowanie niezdefiniowane. Przy
    // okazji rozpakowujemy wartownika 255 z powrotem na pare (mod, filled).
    for (int i = 0; i < BurnerHistory::SLOTS; ++i) {
      const uint8_t v = b.mod[i];
      h.filled[i] = v <= 100;
      h.mod[i] = h.filled[i] ? v : 0;
    }
    h.day = b.day;
    // (v168) LOG(): profil palnika to RODZENSTWO profilu PV — ten sam blad zapisu
    // uderzy w oba, a "wykres pieca pusty po restarcie" jest dokladnie tym objawem,
    // ktory v166 mial zamknac. Bez wpisu w /api/log nie da sie stwierdzic, czy
    // wrocil, bo urzadzenie nie ma USB.
    LOG("Piec: wczytano z NVS profil palnika — dzien %d\n", static_cast<int>(b.day));
  } else {
    LOG("Piec: BRAK profilu palnika w NVS — wykres pieca startuje pusty\n");
  }
  const bool legacy = prefs.isKey(K_BURN_V1);
  prefs.end();
  // (v169) Nowy blob NAJPIERW, kasowanie starego POTEM — patrz pvHistoryLoad.
  if (migrated) {
    burnerHistorySave(h);
  }
  if (legacy) {
    Preferences w;
    if (w.begin(NS_PV, false)) {
      w.remove(K_BURN_V1);
      w.end();
    }
  }
  // Profil ze WCZORAJ zostaje tu CELOWO nietkniety i CELOWO nie sprawdzamy daty:
  // przy starcie NTP jeszcze nie odpowiedzial, wiec tm_yday bylby z 1970 i skasowalby
  // dobry profil. Kasowanie doby jest osobno, w netTask, gdzie zegar jest juz pewny —
  // patrz pogoda-gdynia.ino, "polnoc: profil doby palnika przestaje byc dzis".
  // NIE polega ono na push(): push() przychodzi tylko po udanym odpycie pieca, wiec
  // przy milczacym API wczorajszy profil wisialby na ekranie jako "dzis" godzinami.
}

void burnerHistorySave(const BurnerHistory& h) {
  Preferences prefs;
  if (!prefs.begin(NS_PV, false)) {
    nvsMark(NVS_SLOT_BURN, false);
    return;
  }
  BurnerBlob b{};
  b.ver = BURN_VER;
  b.day = static_cast<int16_t>(h.day);
  for (int i = 0; i < BurnerHistory::SLOTS; ++i) {
    // Przyciecie do 100 nie jest ozdoba: bez niego modulacja 255 z uszkodzonego
    // odczytu zapisalaby sie jako wartownik "brak pomiaru" i slot zniknalby z wykresu.
    b.mod[i] = h.filled[i] ? (h.mod[i] > 100 ? 100 : h.mod[i]) : BURN_MOD_NONE;
  }
  nvsPutBytes(prefs, NVS_SLOT_BURN, K_BURN, &b, sizeof(b));
  prefs.end();
}

// ------------- (v166) TRWALA KOPIA STATYSTYK PIR + LDR (klucz "sen1") --------
// OBJAW ZGLOSZONY PRZEZ WLASCICIELA: "po restarcie brak danych, a powinny zostac"
// w sekcji panelu "Obecnosc · Swiatlo · Ruch".
//
// STAN FAKTYCZNY, SPRAWDZONY NA ZYWO: restart PROGRAMOWY dane PRZEZYWAJA — w
// dzienniku urzadzenia stoi wprost "PIR: liczniki z RTC przezyly restart — zbieram
// od 14242 s, start #3". Gina przy ZANIKU NAPIECIA: gPir i gLdr siedza w sekcji
// .rtc_noinit, a ta przezywa OTA, panic, watchdog i brownout, ale nie odlaczenie
// zasilania (pelne uzasadnienie przy PirRtc w Log.h). 16.08.2026 wlasciciel
// wylaczyl zasilanie recznie i wielotygodniowe zbiory wystartowaly od zera.
//
// LEK: kopia w NVS. RTC ZOSTAJE pamiecia podreczna "na goraco" — nic sie stamtad
// nie przenosi, uklad pol i ADRESY (gPir @ 0x50000200, gLdr @ 0x500002c0) sa
// NIETKNIETE, bo kazda ich zmiana kasuje zbiory przy najblizszym OTA.
//
// WLASNY KLUCZ, NIE DOKLEJANIE DO ISTNIEJACEGO BLOBU — ten sam wzorzec, co przy
// "mtr2" i "prof2": osobna struktura, pole wersji, kontrola rozmiaru w czasie
// kompilacji. Rozmiary blobow w przestrzeni "pvday" sa PARAMI ROZNE (prof2 = 292,
// rh3 = 872, gas2 = 128, burn2 = 148, mtr2 = 32, airh = 52, sen1 = 424), wiec
// pomylka o klucz nie przejdzie przez kontrole getBytesLength().
//
// DLACZEGO KOPIA 1:1 CALYCH STRUKTUR, A NIE WYBRANE POLA: przepisywanie pole po
// polu wymaga pamietania o KAZDYM nowym polu PirRtc/LdrRtc, a zapomniane pole nie
// wywala kompilacji — po cichu wraca z NVS jako zero. Kopia calosci ma za to jedna
// twarda zalete: `magic` KAZDEJ struktury jedzie razem z danymi, a jego dolny bajt
// to WERSJA UKLADU POL. Podbicie PIR_RTC_MAGIC uniewaznia wiec kopie w NVS dokladnie
// tak samo, jak uniewaznia zawartosc RTC — jednym ruchem i bez drugiej numeracji
// do pilnowania. static_assert nizej lapie kazda zmiane ROZMIARU.
namespace {

struct SensStatsBlob {
  uint16_t ver;
  uint16_t pad;          // jawne wyrownanie, zeby rozmiar nie zalezal od kompilatora
  uint32_t savedEpoch;   // epoch chwili zapisu; 0 = zapisano, zanim NTP dal czas
  uint32_t powerGapS;    // sumaryczne sekundy BEZ ZASILANIA (patrz SensStats)
  uint32_t coldStarts;   // ile razy zbiory wrocily z NVS po zaniku napiecia
  PirRtc pir;            // kopia 1:1, RAZEM z magic (= wersja ukladu pol)
  LdrRtc ldr;            // kopia 1:1, RAZEM z magic
};

constexpr uint16_t SENS_VER = 1;
constexpr const char* K_SENS = "sen1";

static_assert(sizeof(PirRtc) == 192,
              "zmienil sie uklad PirRtc - podbij PIR_RTC_MAGIC (Log.h) ORAZ klucz NVS "
              "na \"sen2\"; sam podbity magic uniewazni kopie, ale rozmiar blobu i tak "
              "przestanie pasowac do wpisu w NVS");
static_assert(sizeof(LdrRtc) == 216,
              "zmienil sie uklad LdrRtc - podbij LDR_RTC_MAGIC (Log.h) ORAZ klucz NVS "
              "na \"sen2\" (jak wyzej)");
static_assert(sizeof(SensStatsBlob) == 424,
              "zmienil sie uklad kopii statystyk - podbij klucz NVS na \"sen2\", "
              "inaczej stary blob wczyta sie jako nowy (cicha korupcja)");

SensStats gSensStats;

}  // namespace

SensStats& sensStats() { return gSensStats; }

uint32_t sensStatsBytes() { return static_cast<uint32_t>(sizeof(SensStatsBlob)); }

bool sensStatsLoad(PirRtc* pir, LdrRtc* ldr) {
  Preferences prefs;
  if (!prefs.begin(NS_PV, true)) {
    return false;
  }
  // Blob na stosie: 424 B. Wolane z setup(), gdzie stos glowny ma 8 kB — a jedyna
  // alternatywa (statyczny bufor) kosztowalaby te 424 B RAM-u NA STALE, przy barierze
  // 76000 B i zapasie rzedu dwoch kilobajtow.
  SensStatsBlob b{};
  const bool ok = prefs.getBytesLength(K_SENS) == sizeof(b) &&
                  prefs.getBytes(K_SENS, &b, sizeof(b)) == sizeof(b) &&
                  b.ver == SENS_VER;
  prefs.end();
  if (!ok) {
    return false;
  }

  gSensStats.savedEpoch = b.savedEpoch;
  gSensStats.powerGapS = b.powerGapS;
  gSensStats.coldStarts = b.coldStarts;
  // Magic w kopii to WERSJA UKLADU POL tej struktury. Gdy sie nie zgadza, blob
  // pochodzi z firmware'u o innym ukladzie i wczytany 1:1 dalby liczby wygladajace
  // sensownie, a bedace smieciem — to ta sama zasada, co przy odczycie RTC.
  gSensStats.pirOk = b.pir.magic == PIR_RTC_MAGIC;
  gSensStats.ldrOk = b.ldr.magic == LDR_RTC_MAGIC;
  // memcpy, a nie przypisanie: obie struktury maja pola `volatile` (pisze do nich ISR),
  // a kopiowanie ich przypisaniem to w C++20 teren ostrzezenia -Wvolatile. Kopia
  // bajtowa jest tu poprawna, bo obie struktury to same wyrownane uint32.
  if (pir != nullptr && gSensStats.pirOk) memcpy(pir, &b.pir, sizeof(b.pir));
  if (ldr != nullptr && gSensStats.ldrOk) memcpy(ldr, &b.ldr, sizeof(b.ldr));
  return true;
}

bool sensStatsSave(const PirRtc& pir, const LdrRtc& ldr) {
  // Zapis smiecia jest GORSZY niz brak zapisu: nadpisalby dobra kopie zbiorem,
  // ktorego i tak nie wolno wczytac. Gdy ZADNA ze struktur nie ma waznego znacznika,
  // nie mamy czego utrwalac.
  if (pir.magic != PIR_RTC_MAGIC && ldr.magic != LDR_RTC_MAGIC) {
    return false;
  }
  Preferences prefs;
  if (!prefs.begin(NS_PV, false)) {
    gSensStats.saveFailed = true;
    nvsMark(NVS_SLOT_SENS, false);   // (v168) ten sam licznik, co reszta blobow
    return false;
  }
  SensStatsBlob b{};
  b.ver = SENS_VER;
  // 0 = "nie wiem, kiedy". CELOWO nie przepisujemy tu starego stempla: przerwe bez
  // pradu liczymy jako roznice DWOCH epochow, wiec stempel sprzed dwoch dni pracy bez
  // NTP kazalby doliczyc te dwa dni do power_gap_s, czyli SKLAMAC. Brak stempla konczy
  // sie utrata JEDNEJ liczby (dlugosci przerwy), a nie zafalszowaniem statystyki.
  const time_t nowT = time(nullptr);
  b.savedEpoch = (nowT > 1700000000) ? static_cast<uint32_t>(nowT) : 0;
  b.powerGapS = gSensStats.powerGapS;
  b.coldStarts = gSensStats.coldStarts;
  memcpy(&b.pir, &pir, sizeof(b.pir));
  memcpy(&b.ldr, &ldr, sizeof(b.ldr));
  // (v168) Ten zapis JUZ w v166 sprawdzal wynik — teraz robi to przez wspolny
  // nvsPutBytes(), zeby "sen1" stalo w tej samej tabeli /api/diag co pozostale
  // szesc blobow. gSensStats.saveFailed/saveOkAt ZOSTAJA: karmia istniejaca sekcje
  // panelu "Obecnosc · Swiatlo · Ruch", ktorej nie przepisujemy przy okazji.
  const bool ok = nvsPutBytes(prefs, NVS_SLOT_SENS, K_SENS, &b, sizeof(b));
  prefs.end();

  gSensStats.saveFailed = !ok;
  if (ok) gSensStats.saveOkAt = millis();
  return ok;
}
