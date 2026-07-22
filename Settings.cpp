#include "Settings.h"

#include <Arduino.h>
#include <Preferences.h>
#include <cstring>

#include "PvData.h"
#include "GasMeter.h"
#include "RoomHistory.h"

namespace {
Settings gSettings;
constexpr const char* NS_CFG = "pogoda";
constexpr const char* NS_PV = "pvday";

// Klucze listy bramek. Slot 0 zostaje pod historycznym "blegw" (patrz Settings.h),
// reszta dostaje wlasne klucze. Jedno miejsce, bo zapisuja to DWIE funkcje:
// save() (calosc konfiguracji z panelu) i bleGwSave() (sama lista).
void bleGwWrite(Preferences& prefs, const Settings& s) {
  prefs.putString("blegw", s.bleGwHost);
  for (int i = 1; i < Settings::BLE_GW; ++i) {
    char k[8];
    snprintf(k, sizeof(k), "bgw%d", i);
    prefs.putString(k, s.bleGwAt(i));
  }
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
  // Domyslnie 2 (V2) — patrz uzasadnienie przy polu w Settings.h. Kazda wartosc
  // spoza {1,2} (np. 0 z NVS, ktorego ta wersja nigdy by nie zapisala) wraca do
  // domyslnej, zamiast wciskac sie w rysowanie jako nieznany wyglad.
  const uint8_t th = prefs.getUChar("theme", 3);
  theme = (th >= 1 && th <= 3) ? th : 3;   // 1=V1 ciemny, 2=V2 retro, 3=V3 "Pasmowy" (domyslny)

  // Ustawienia wyswietlacza (tryb nocny + rotacja + jasnosc). Domyslne = dawne stale
  // z Config.h; clampTuning() nizej pilnuje zakresow (m.in. TWARDE minimum jasnosci),
  // wiec nawet blob z uszkodzonego/przyszlego NVS nie zejdzie ponizej progu.
  nightStartH = prefs.getUChar("nstart", 22);
  nightEndH   = prefs.getUChar("nend", 6);
  dwellS      = prefs.getUShort("dwell", 9);
  blDay       = prefs.getUChar("blday", 255);
  blDim       = prefs.getUChar("bldim", 130);
  blNight     = prefs.getUChar("blnight", 45);
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
    return;
  }
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.putString("city", city);
  prefs.putString("mb", modbusHost);
  prefs.putFloat("lat", lat);
  prefs.putFloat("lon", lon);
  prefs.putUShort("mbport", modbusPort);
  prefs.putUShort("peak", pvPeakW);
  prefs.putBool("ota", otaEnabled);
  prefs.putString("mqhost", mqttHost);
  prefs.putString("mquser", mqttUser);
  prefs.putString("mqpass", mqttPass);
  prefs.putString("mqpre", mqttPrefix);
  prefs.putUShort("mqport", mqttPort);
  prefs.putBool("mqen", mqttEnabled);
  bleGwWrite(prefs, *this);
  prefs.end();
}

// Osobno od save(): przelacznik wygladu klika sie z panelu niezaleznie od reszty
// formularzy (WiFi/MQTT/lokalizacja...), wiec nie ma powodu przy KAZDYM kliknieciu
// V1/V2 przepisywac do NVS cala reszte ustawien. Ten sam wzorzec co viSave()/
// meterSave()/bleGwSave() nizej.
bool Settings::setTheme(uint8_t t) {
  if (t < 1 || t > 3) return false;   // 1=V1, 2=V2, 3=V3 "Pasmowy"
  theme = t;
  Preferences prefs;
  if (!prefs.begin(NS_CFG, false)) {
    return false;
  }
  prefs.putUChar("theme", theme);
  prefs.end();
  return true;
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
// Ten sam wzorzec co setTheme()/viSave()/meterSave(). Clamp NAJPIERW — do NVS i do
// RAM trafiaja juz wartosci w zakresie, wiec panel czytajacy je z powrotem widzi
// PRAWDE (np. jasnosc podbita do minimum).
bool Settings::saveTuning(uint8_t nStart, uint8_t nEnd, uint16_t dwell,
                          uint8_t bDay, uint8_t bDim, uint8_t bNight) {
  nightStartH = nStart;
  nightEndH   = nEnd;
  dwellS      = dwell;
  blDay       = bDay;
  blDim       = bDim;
  blNight     = bNight;
  clampTuning();

  Preferences prefs;
  if (!prefs.begin(NS_CFG, false)) {
    return false;
  }
  prefs.putUChar("nstart", nightStartH);
  prefs.putUChar("nend", nightEndH);
  prefs.putUShort("dwell", dwellS);
  prefs.putUChar("blday", blDay);
  prefs.putUChar("bldim", blDim);
  prefs.putUChar("blnight", blNight);
  prefs.end();
  return true;
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
    return;
  }
  bleGwWrite(prefs, *this);
  prefs.end();
}

// Osobno od save(): refresh token zmienia sie co 180 dni, a IDy raz. Nie ma po co
// przepisywac calej konfiguracji przy kazdym odswiezeniu.
void Settings::viSave() {
  Preferences prefs;
  if (!prefs.begin(NS_CFG, false)) {
    return;
  }
  prefs.putString("vicid", viClientId);
  prefs.putString("viref", viRefresh);
  prefs.putString("viinst", viInstallation);
  prefs.putString("vigw", viGateway);
  prefs.putUInt("viat", viAuthAt);
  prefs.putBool("vien", viEnabled);
  prefs.end();
}

void Settings::meterSave() {
  Preferences prefs;
  if (!prefs.begin(NS_CFG, false)) {
    return;
  }
  prefs.putBytes("mets", meters, sizeof(meters));
  prefs.end();
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
    return false;
  }
  snprintf(k, sizeof(k), "b%dmac", slot);
  prefs.putString(k, c.mac);
  snprintf(k, sizeof(k), "b%dnam", slot);
  prefs.putString(k, c.name);
  snprintf(k, sizeof(k), "b%dkey", slot);
  if (c.hasKey) {
    prefs.putBytes(k, c.key, 16);
  } else {
    prefs.remove(k);
  }
  prefs.end();
  return true;
}

void Settings::clearWifi() {
  ssid[0] = '\0';
  pass[0] = '\0';
  Preferences prefs;
  if (!prefs.begin(NS_CFG, false)) {
    return;
  }
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
// Teraz caly profil to JEDNA struktura pod JEDNYM kluczem "prof1". Jej rozmiar
// (584 B) nie jest podobny do zadnej ze skladowych, wiec pomylka o klucz jest
// niemozliwa, a pole `ver` lapie zmiane semantyki przy tym samym rozmiarze
// (np. przejscie z watow na dziesiatki watow).
//
// KAZDA zmiana ukladu tej struktury MUSI isc z NOWYM kluczem ("prof2"). Pilnuje
// tego static_assert nizej: gdy rozmiar sie zmieni, kompilacja padnie z ta
// instrukcja zamiast po cichu wczytac stary blob jako nowy.
namespace {

struct PvProfileBlob {
  uint16_t ver;
  int32_t day;
  uint16_t watts[PvHistory::SLOTS];
  uint16_t load[PvHistory::SLOTS];
};

constexpr uint16_t PV_PROF_VER = 1;
constexpr const char* K_PV_PROF = "prof1";

static_assert(sizeof(PvProfileBlob) == 584,
              "zmienil sie uklad profilu PV - podbij klucz NVS na \"prof2\", "
              "inaczej stary blob wczyta sie jako nowy (cicha korupcja)");

// Klucze ukladu v1. Nigdy juz nie beda czytane, a zajmuja ~580 B w malej
// partycji NVS (min_spiffs). Kasujemy je raz, przy pierwszym starcie po zmianie.
void pvRemoveLegacy(Preferences& p) {
  p.remove("w");
  p.remove("l");
  p.remove("day");
}

}  // namespace

void pvHistoryLoad(PvHistory& h) {
  h.reset(-1);

  Preferences prefs;
  if (!prefs.begin(NS_PV, true)) {
    return;
  }
  PvProfileBlob b{};
  const bool ok = prefs.getBytesLength(K_PV_PROF) == sizeof(b) &&
                  prefs.getBytes(K_PV_PROF, &b, sizeof(b)) == sizeof(b) &&
                  b.ver == PV_PROF_VER && b.day >= 0;
  const bool legacy = prefs.isKey("w") || prefs.isKey("l") || prefs.isKey("day");
  prefs.end();

  if (ok) {
    memcpy(h.watts, b.watts, sizeof(h.watts));
    memcpy(h.load, b.load, sizeof(h.load));
    h.day = b.day;
    for (int i = 0; i < PvHistory::SLOTS; ++i) {
      h.filled[i] = h.watts[i] > 0 || h.load[i] > 0;
    }
    Serial.printf("PV: wczytano profil dnia %d z NVS\n", static_cast<int>(b.day));
  }

  if (legacy) {
    Preferences w;
    if (w.begin(NS_PV, false)) {
      pvRemoveLegacy(w);
      w.end();
      Serial.println("PV: skasowano profil w starym ukladzie (klucze w/l/day)");
    }
  }
}

void pvHistorySave(const PvHistory& h) {
  Preferences prefs;
  if (!prefs.begin(NS_PV, false)) {
    return;
  }
  PvProfileBlob b{};
  b.ver = PV_PROF_VER;
  b.day = h.day;
  memcpy(b.watts, h.watts, sizeof(b.watts));
  memcpy(b.load, h.load, sizeof(b.load));
  prefs.putBytes(K_PV_PROF, &b, sizeof(b));
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
              "zmienil sie uklad RoomHistory - podbij klucz NVS na \"rh3\". "
              "Sam rozmiar NIE wystarczy: v1 (4 pokoje T+RH) i v2 (6 pokoi T) "
              "mialy identyczne 1736 B i kontrola rozmiarem ich nie rozroznila");

void roomHistoryLoad(RoomHistory& h) {
  Preferences prefs;
  if (!prefs.begin(NS_PV, true)) {
    h.reset();
    return;
  }
  const size_t need = sizeof(RoomHistory);
  if (prefs.getBytesLength("rh2") == need) {
    prefs.getBytes("rh2", &h, need);
    Serial.printf("BLE: wczytano historie 24 h (slot %lu)\n",
                  static_cast<unsigned long>(h.lastSlot));
  } else {
    h.reset();
  }
  prefs.end();
}

void roomHistorySave(const RoomHistory& h) {
  Preferences prefs;
  if (!prefs.begin(NS_PV, false)) {
    return;
  }
  prefs.putBytes("rh2", &h, sizeof(RoomHistory));
  prefs.end();
}

// ------------------------------------------ dzienny log gazu (120 dni) --------
// Bez tego cala weryfikacja licznika nie ma sensu: gGas zbieral dane od zawsze,
// ale ginely przy KAZDYM restarcie — a porownanie "licznik vs piec" wymaga
// tygodni historii. Zbieranie bez zapisu to byl najgorszy mozliwy stan: kod
// wygladal na dzialajacy, koszt RAM byl placony, pozytku zero.
//
// Ten sam wzorzec co przy profilu PV: wersja w blobie, wlasny klucz, asercja
// rozmiaru. GasHistory ma 248 B, wiec zapis jest tani — leci raz na dobe, przy
// przewinieciu dnia, a nie co 3 minuty.
namespace {
struct GasBlob {
  uint16_t ver;
  uint32_t lastDay;
  int16_t head;
  uint16_t m3x100[GasHistory::DAYS];
};
constexpr uint16_t GAS_VER = 1;
constexpr const char* K_GAS = "gas1";

static_assert(sizeof(GasBlob) == 252,
              "zmienil sie uklad logu gazu - podbij klucz NVS na \"gas2\"");
}  // namespace

void gasHistoryLoad(GasHistory& g) {
  g.reset();
  Preferences prefs;
  if (!prefs.begin(NS_PV, true)) {
    return;
  }
  GasBlob b{};
  if (prefs.getBytesLength(K_GAS) == sizeof(b) &&
      prefs.getBytes(K_GAS, &b, sizeof(b)) == sizeof(b) && b.ver == GAS_VER) {
    g.lastDay = b.lastDay;
    g.head = b.head;
    memcpy(g.m3x100, b.m3x100, sizeof(b.m3x100));
    Serial.printf("Gaz: wczytano log (dzien %lu)\n", static_cast<unsigned long>(g.lastDay));
  }
  prefs.end();
}

void gasHistorySave(const GasHistory& g) {
  Preferences prefs;
  if (!prefs.begin(NS_PV, false)) {
    return;
  }
  GasBlob b{};
  b.ver = GAS_VER;
  b.lastDay = g.lastDay;
  b.head = g.head;
  memcpy(b.m3x100, g.m3x100, sizeof(b.m3x100));
  prefs.putBytes(K_GAS, &b, sizeof(b));
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
// jak noc, w ktora piec stal). 144 B w blobie jest tansze teraz niz migracja klucza
// NVS pozniej.
//
// Ten sam wzorzec, co przy "gas1" i "prof1": wlasny klucz, pole `ver` w blobie,
// asercja rozmiaru. Rozmiary blobow w przestrzeni "pvday" sa rozne (prof1 = 584,
// rh2 = 1736, gas1 = 252, burn1 = 296), wiec pomylka o klucz nie ma jak przejsc
// przez kontrole getBytesLength().
namespace {
struct BurnerBlob {
  uint16_t ver;
  int32_t day;                            // tm_yday
  uint8_t mod[BurnerHistory::SLOTS];      // 0..100 %
  uint8_t filled[BurnerHistory::SLOTS];   // 0/1 — patrz wyzej: pole na zapas
};
constexpr uint16_t BURN_VER = 1;
constexpr const char* K_BURN = "burn1";

static_assert(sizeof(BurnerBlob) == 296,
              "zmienil sie uklad profilu palnika - podbij klucz NVS na \"burn2\", "
              "inaczej stary blob wczyta sie jako nowy (cicha korupcja)");
}  // namespace

void burnerHistoryLoad(BurnerHistory& h) {
  h.reset(-1);
  Preferences prefs;
  if (!prefs.begin(NS_PV, true)) {
    return;
  }
  BurnerBlob b{};
  if (prefs.getBytesLength(K_BURN) == sizeof(b) &&
      prefs.getBytes(K_BURN, &b, sizeof(b)) == sizeof(b) && b.ver == BURN_VER &&
      b.day >= 0) {
    memcpy(h.mod, b.mod, sizeof(h.mod));
    // Przez petle, nie memcpy: `filled` w BurnerHistory to bool[], a bool o wartosci
    // innej niz 0/1 (choćby ze smiecia w NVS) to zachowanie niezdefiniowane.
    for (int i = 0; i < BurnerHistory::SLOTS; ++i) h.filled[i] = b.filled[i] != 0;
    h.day = b.day;
    Serial.printf("Piec: wczytano profil palnika dnia %d z NVS\n", static_cast<int>(b.day));
  }
  prefs.end();
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
    return;
  }
  BurnerBlob b{};
  b.ver = BURN_VER;
  b.day = h.day;
  memcpy(b.mod, h.mod, sizeof(b.mod));
  for (int i = 0; i < BurnerHistory::SLOTS; ++i) b.filled[i] = h.filled[i] ? 1 : 0;
  prefs.putBytes(K_BURN, &b, sizeof(b));
  prefs.end();
}
