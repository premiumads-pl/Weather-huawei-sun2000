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
  String bg = prefs.getString("blegw", "");
  strncpy(bleGwHost, bg.c_str(), sizeof(bleGwHost) - 1);
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
  prefs.putString("blegw", bleGwHost);
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
