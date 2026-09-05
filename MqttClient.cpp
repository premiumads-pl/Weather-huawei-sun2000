#include "MqttClient.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>

#include "BleSensors.h"
#include "Log.h"
#include "Settings.h"
#include "Version.h"
// Celowo NIE wlaczamy WeatherIcons.h — to naglowek z ~96 kB tablic ikon
// (static const w naglowku = osobna kopia w kazdej jednostce kompilacji)
// i calym TFT_eSPI w zaleznosciach. Opis pogody skladamy nizej sami.
//
// (v174) ArduinoJson JEST juz w tym programie (WeatherClient, AirClient,
// FlightClient, BleGateway) i to on rozstrzyga wybor parsera dla przychodzacego
// <prefix>/auto/stan: drugi, recznie pisany skaner JSON-a bylby DRUGA definicja
// tego, jak w tym projekcie czyta sie odpowiedzi — a takich rozjazdow ten plik
// juz raz kosztowal (patrz "38/22" przy sendDiscovery nizej). Naglowek jest
// szablonowy i nie dodaje ANI BAJTA statycznego RAM-u; JsonDocument bierze swoja
// pule na STERCIE, na czas jednego wywolania parsera (kilkaset bajtow przy 150 B
// ladunku), i oddaje ja przed powrotem z callbacku.

namespace mqttha {
namespace {

// --- budzet pamieci -----------------------------------------------------------
// PubSubClient trzyma JEDEN bufor na stercie (domyslnie 256 B). Najwiekszy pakiet,
// jaki wysylamy, to retained config encji. Przy maksymalnym prefiksie (23 znaki)
// i najdluzszej nazwie encji wychodzi 430 B razem z tematem i naglowkiem — 512 B
// daje na to zapas, a jest to okolo 1/80 tego, co zjada jeden handshake TLS.
// publishConfig() i tak sprawdza rozmiar kazdego pakietu i odrzuca za duze.
//
// (v174) BUFORA NIE POWIEKSZAMY, mimo ze od tego wydania takze ODBIERAMY. Ten sam
// bufor obsluguje oba kierunki, wiec trzeba bylo policzyc najwiekszy pakiet
// PRZYCHODZACY — czyli PUBLISH na <prefix>/auto/stan:
//     1 B naglowka stalego + 2 B dlugosci zmiennej (ladunek > 127 B)
//   + 2 B dlugosci tematu + 33 B tematu (23 znaki maks. prefiksu + "/auto/stan")
//   + ~150 B ladunku JSON (pelny przyklad z opisu tematu ma 151 B)
//   = ~188 B, czyli 37% bufora i o 242 B MNIEJ niz nasz wlasny pakiet wychodzacy.
// Bufor wyznacza wiec dalej retained config encji (430 B) i nic sie nie zmienia.
// Gdyby ktos po drugiej stronie kiedys dolozyl pol do tego JSON-a, granica lezy przy
// ~470 B ladunku; PubSubClient wieksza wiadomosc PO CICHU ODRZUCA (readPacket ustawia
// len = 0, PubSubClient.cpp:364), wiec objawem byloby "ekran AUTO przestal sie
// pokazywac", a nie awaria — dlatego ta granica jest tu wypisana liczba.
//
// (v179) TRZECI KANDYDAT, PRZELICZONY PO DOLOZENIU LICZNIKOW MIERNIKA. Grupa `pv`
// dostala pola `gin`/`gout` (encje pv_grid_in / pv_grid_out), wiec jej pakiet stanu
// urosl i wypada policzyc go jawnie zamiast zakladac, ze "state jest maly":
//     1 B naglowka stalego + 2 B dlugosci zmiennej + 2 B dlugosci tematu
//   + 32 B tematu (23 znaki maks. prefiksu + "/pv/state") + 194 B ladunku
//   = 231 B, czyli 45% bufora — pelna rozpiska ladunku stoi przy publishPv().
// Retained config encji (430 B) zostaje wiec dalej NAJWIEKSZYM pakietem i to on
// wyznacza kBufSize. Same nowe encje tez tej granicy nie podnosza: przy maksymalnym
// prefiksie ich configi wychodza 405 i 408 B razem z tematem i naglowkiem, czyli
// TYLE SAMO co juz obecny pv_total (408 B) — nazwy "Pobrane z sieci" / "Oddane do
// sieci" sa krotsze od "Produkcja całkowita" dokladnie o tyle, o ile dluzsze sa
// klucze pv_grid_in / pv_grid_out. Najgrubsze pakiety i tak robi discovery BLE,
// gdzie nazwa encji sklada sie z nazwy pokoju wpisanej przez uzytkownika.
//
// (v180) DRUGI TEMAT PRZYCHODZACY — <prefix>/dom/stan — i znowu bufora nie ruszamy.
// Rachunek ten sam, co przy auto/stan, przy maksymalnym prefiksie (23 znaki):
//     1 B naglowka stalego + 1 B dlugosci zmiennej (ladunek < 128 B, wiec JEDEN)
//   + 2 B dlugosci tematu + 32 B tematu (23 znaki prefiksu + "/dom/stan")
//   + 10 B ladunku ({"zl":4.8}; wariant z groszami {"zl":9999.99} ma 15 B)
//   = 46 B, czyli 9% bufora — NAJMNIEJSZY ze wszystkich pakietow tego modulu.
// (v181) Doszlo pole "pv" ({"zl":4.8,"pv":13279} = 22 B, wariant skrajny z szescioma
// cyframi 24 B), czyli pakiet ma dzis 58-60 B — nadal najmniejszy i nadal ~12% bufora.
// Pelna kolejnosc po tej zmianie: retained config encji (430 B) > stan grupy pv
// (231 B) > auto/stan (188 B) > dom/stan (46 B). Bufor wyznacza wiec dalej
// discovery. Ladunek dom/stan ma byc ROZSZERZALNY (kolejne pola stanu domu), wiec
// zapas wypada zapisac liczba: przy tym samym temacie miesci sie do ~470 B tresci.
// (v182) Doszlo pole "t" — strefa taryfy ({"zl":4.8,"pv":13279,"t":1} = 28 B, pakiet
// 64-66 B, ~13% bufora). Trzecie pole i trzeci raz bez ruszania bufora: to jest ta
// sama liczba ~470 B zapasu, co linijke wyzej, i nadal nie zblizamy sie do niej.
constexpr uint16_t kBufSize = 512;

constexpr uint16_t kKeepAliveS = 60;      // PINGREQ co minute, nie co 15 s
constexpr uint16_t kSockTimeoutS = 3;     // czekanie na CONNACK
constexpr uint32_t kConnTimeoutMs = 2000; // TCP connect (domyslne 3000 to za dlugo)
constexpr uint32_t kBackoffMinMs = 5000;
constexpr uint32_t kBackoffMaxMs = 5UL * 60UL * 1000UL;
constexpr uint32_t kDevPublishMs = 60000;

// Klient zyje na stercie tylko wtedy, gdy MQTT jest wlaczony.
WiFiClient* gSock = nullptr;
PubSubClient* gCli = nullptr;

char gDevId[16] = {};   // pg_a1b2c3 — node_id, client id, prefiks unique_id
char gAvail[36] = {};   // <prefix>/status (LWT)
char gAutoTopic[40] = {};   // (v174) <prefix>/auto/stan — pierwsza nasza subskrypcja
char gCostTopic[40] = {};   // (v180) <prefix>/dom/stan — druga (koszt energii z sieci)

// (v174) Ostatni odebrany stan auta + jego wlasny mutex.
//
// MUTEX POWSTAJE TUTAJ, W INICJALIZACJI STATYCZNEJ, a nie leniwie przy pierwszym
// uzyciu — "if (gMx == nullptr) gMx = xSemaphoreCreateMutex()" samo jest wyscigiem,
// gdy dwa zadania trafia tam naraz. Dokladnie ta sama decyzja i to samo uzasadnienie,
// co przy gMx w Viessmann.cpp; tam stoi pelny opis.
//
// KTO PISZE, KTO CZYTA: pisze onMessage() (callback PubSubClienta, czyli netTask),
// czyta autoSnapshot() wolane z petli rysujacej na drugim rdzeniu. 44 B to nie jest
// zapis atomowy, wiec bez tej blokady dalo by sie zlapac polowe starej i polowe nowej
// wiadomosci — na ekranie objawiloby sie to naladowaniem z jednej sekundy przy mocy
// z innej, czyli bledem nie do powiazania z przyczyna.
AutoModel gAutoRx{};
SemaphoreHandle_t gAutoMx = xSemaphoreCreateMutex();

// (v180) Ostatni odebrany stan domu (<prefix>/dom/stan) — POD TYM SAMYM gAutoMx.
//
// TEN SAM MUTEX, A NIE DRUGI, i to jest decyzja policzona, a nie oszczednosc na
// czuja. Ukladu watkow nie zmieniamy ani o krok: pisze onMessage() (netTask, rdzen 0),
// czyta costSnapshot() z petli rysujacej (rdzen 1) — czyli DOKLADNIE ta sama para,
// co przy gAutoRx. Obie sekcje krytyczne to przepisanie struktury (44 B i 8 B) bez
// ani jednego wywolania, ktore moze uspic zadanie, wiec jedna blokada nie tworzy
// tu zwloki: dwie wiadomosci nigdy nie sa parsowane rownolegle, bo obie przychodza
// z tego samego gCli->loop(). Osobny gCostMx kosztowalby uchwyt w statyku i ~80 B
// sterty za zerowy zysk — ten sam rachunek, ktory w v175 kazal wpuscic pod gAutoMx
// takze gModeReq. 8 B to nie jest zapis atomowy (`atMs` i `zl` to dwa slowa), wiec
// blokada jest tu obowiazkowa, a nie kosmetyczna: bez niej ekran umialby pokazac
// swieza kwote z wiekiem sprzed trzech minut albo odwrotnie.
// (v181) Struktura ma dzis 12 B (doszlo pvPln), a sekcja krytyczna zyskala ODCZYT
// starej wartosci pvPln obok zapisu nowej (wiadomosc bez pola "pv" ma zostawic
// poprzednia liczbe — patrz onMessage). Tym bardziej nierozdzielne.
// (v182) Struktura ma dzis 13 B tresci w 16 B (doszlo `tariff`), a sekcja krytyczna
// zyskala DRUGI przenoszony stan obok pvPln — z tego samego powodu i tym samym
// wzorcem (wiadomosc bez pola "t" ma zostawic poprzednia strefe).
CostModel gCostRx{};

// (v175) ZAMOWIENIE Z PANELU OLED: tryb do wyslania na <prefix>/auto/tryb/set.
// Sklada je petla rysowania (rdzen 1), wysyla netTask (rdzen 0) — pelne uzasadnienie
// przy requestAutoMode() w MqttClient.h. Napis chodzi pod TYM SAMYM mutexem, co
// gAutoRx: sekcje krytyczne to przepisanie 8 B, wiec drugi mutex bylby kosztem bez
// zysku. Stan jest `volatile uint8_t` i celowo POZA mutexem — pojedynczy bajt na
// Xtensie zapisuje sie jedna instrukcja, a czytelnik i tak reaguje dopiero na
// wartosc koncowa (2 albo 3).
char gModeReq[8] = {};
volatile uint8_t gModeReqState = 0;   // 0 nic / 1 czeka / 2 wyslane / 3 blad

// (P1-1) gLastTryAt trzyma CZAS OSTATNIEJ PROBY (nie "kolejny dozwolony termin" jak
// dawne gNextTryAt) — patrz pelne uzasadnienie przy uzyciu w loop() nizej. Start
// USTAWIONY W TYL o kBackoffMaxMs, NIE 0: przy starcie millis() tez jest male, wiec
// "0 jako pierwsza probka" dawaloby PIERWSZEMU polaczeniu sztuczna zwloke do
// kBackoffMinMs od startu programu, ktorej stary kod (harmonogram na "kolejnym
// terminie") nie mial. Odejmowanie bez znaku samo sobie radzi z tym "ujemnym"
// startem — ten sam mechanizm, ktory chroni przed przekreceniem millis() po
// 49,7 dnia, dziala tu w odwrotna strone i gwarantuje natychmiastowa pierwsza probe.
uint32_t gLastTryAt = 0u - kBackoffMaxMs;
uint32_t gBackoffMs = kBackoffMinMs;
uint32_t gNextDevAt = 0;
volatile bool gReconfig = false;

// Ostatnie znane liczniki falownika. Gdy Modbus milczy (noc, restart falownika),
// publikujemy moce = 0, ale energie trzymamy — HA liczy total_increasing i zjazd
// do zera zinterpretowalby jako reset licznika i dorzucil fikcyjna produkcje.
struct PvCache {
  float todayKwh = 0.f;
  float totalKwh = 0.f;
  float tempC = 0.f;
  bool have = false;
} gPvCache;

// ---------------------------------------------------------------- pomocnicze --

void setErr(const char* msg) {
  strncpy(diag().mqttErr, msg, sizeof(diag().mqttErr) - 1);
  diag().mqttErr[sizeof(diag().mqttErr) - 1] = '\0';
}

void clearErr() {
  diag().mqttErr[0] = '\0';
}

const char* stateText(int st) {
  switch (st) {
    case -4: return "Broker nie odpowiada";
    case -3: return "Zerwane połączenie";
    case -2: return "Brak połączenia z brokerem";
    case -1: return "Rozłączony";
    case 1:  return "Broker odrzucił wersję MQTT";
    case 2:  return "Broker odrzucił client id";
    case 3:  return "Broker niedostępny";
    case 4:  return "Zły użytkownik lub hasło";
    case 5:  return "Brak autoryzacji";
    default: return "Błąd MQTT";
  }
}

// Doklejanie do bufora z wykrywaniem przepelnienia: zwraca nowa dlugosc, ktora
// moze przekroczyc cap — wtedy zawartosc jest obcieta i pakiet trzeba odrzucic.
int addf(char* b, int cap, int len, const char* fmt, ...) {
  if (len >= cap) {
    return len;
  }
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(b + len, static_cast<size_t>(cap - len), fmt, ap);
  va_end(ap);
  if (n < 0) {
    return cap;  // blad formatowania traktujemy jak przepelnienie
  }
  return len + n;
}

// -------------------------------------------------------- definicje encji -----
// Tablica jest const → laduje w .rodata (flash), nie zjada RAM-u.

struct Ent {
  const char* key;     // object_id + koncowka unique_id (ASCII)
  const char* name;    // nazwa w HA
  const char* group;   // pv | wx | dev  → temat <prefix>/<group>/state
  const char* field;   // klucz w JSON-ie stanu
  const char* devCla;  // device_class albo nullptr
  const char* unit;    // unit_of_measurement albo nullptr
  const char* staCla;  // state_class albo nullptr
  const char* icon;    // ikona albo nullptr
  bool diagnostic;     // entity_category: diagnostic
};

const Ent kEnts[] = {
    // --- fotowoltaika ---
    {"pv_ac", "Moc AC", "pv", "ac", "power", "W", "measurement", nullptr, false},
    {"pv_dc", "Moc DC", "pv", "dc", "power", "W", "measurement", nullptr, false},
    {"pv_today", "Produkcja dzisiaj", "pv", "today", "energy", "kWh", "total_increasing",
     nullptr, false},
    {"pv_total", "Produkcja całkowita", "pv", "total", "energy", "kWh", "total_increasing",
     nullptr, false},
    // (v179) LICZNIKI MIERNIKA, NIE NASZA CALKA. Panel Energia w HA chce dwoch
    // licznikow narastajacych kWh, a do v178 powstawaly one po stronie HA przez
    // calkowanie chwilowego `pv_grid` — czyli z bledu kwantyzacji 30-sekundowej
    // kadencji. Firmware od v165 czyta PRAWDZIWE rejestry miernika (37121 pobor,
    // 37119 oddanie; PvClient::readMeterEnergy) i tylko ich nie publikowal. To ten
    // sam miernik, ktory raportuje do FusionSolar, wiec HA pokaze liczby IDENTYCZNE
    // z portalem Huawei zamiast wlasnego przyblizenia.
    //
    // Pola `gin`/`gout`, a nie `grid_in`/`grid_out`: nazwa pola wchodzi do payloadu
    // stanu grupy `pv` PRZY KAZDEJ publikacji (co 30 s), a nie raz w discovery —
    // rozliczenie bajtow jest w komentarzu przy publishPv() nizej.
    {"pv_grid_in", "Pobrane z sieci", "pv", "gin", "energy", "kWh", "total_increasing",
     nullptr, false},
    {"pv_grid_out", "Oddane do sieci", "pv", "gout", "energy", "kWh", "total_increasing",
     nullptr, false},
    // znak: + oddawanie do sieci, - pobor
    {"pv_grid", "Bilans sieci", "pv", "grid", "power", "W", "measurement", nullptr, false},
    {"pv_house", "Pobór domu", "pv", "house", "power", "W", "measurement", nullptr, false},
    {"pv_temp", "Temperatura falownika", "pv", "temp", "temperature", "°C", "measurement",
     nullptr, false},
    {"pv_status", "Status falownika", "pv", "status", nullptr, nullptr, nullptr,
     "mdi:solar-power", false},

    // --- pogoda ---
    {"wx_temp", "Temperatura", "wx", "temp", "temperature", "°C", "measurement", nullptr,
     false},
    {"wx_feels", "Temperatura odczuwalna", "wx", "feels", "temperature", "°C", "measurement",
     nullptr, false},
    {"wx_hum", "Wilgotność", "wx", "hum", "humidity", "%", "measurement", nullptr, false},
    {"wx_pres", "Ciśnienie", "wx", "pres", "atmospheric_pressure", "hPa", "measurement",
     nullptr, false},
    {"wx_wind", "Wiatr", "wx", "wind", "wind_speed", "km/h", "measurement", nullptr, false},
    {"wx_cloud", "Zachmurzenie", "wx", "cloud", nullptr, "%", "measurement", "mdi:cloud",
     false},
    {"wx_uv", "Indeks UV", "wx", "uv", nullptr, nullptr, "measurement", "mdi:weather-sunny",
     false},
    {"wx_rain", "Opad", "wx", "rain", "precipitation_intensity", "mm/h", "measurement",
     nullptr, false},
    {"wx_desc", "Pogoda", "wx", "desc", nullptr, nullptr, nullptr,
     "mdi:weather-partly-cloudy", false},

    // --- piec Viessmann (chmura ViCare, publikowane co 3 min) ---
    // Nazwy zaczynaja sie od "Piec", zeby encje sortowaly sie razem w HA.
    {"pc_supply", "Piec zasilanie CO", "pc", "sup", "temperature", "°C", "measurement",
     nullptr, false},
    {"pc_mod", "Piec modulacja palnika", "pc", "mod", nullptr, "%", "measurement",
     "mdi:fire", false},
    // Godziny i starty to liczniki ZYCIOWE palnika, nie dobowe â i jedyne dane
    // o piecu ODPORNE NA ALIASING kadencji 3 minut: roznica miedzy dwoma odpytami
    // mowi, ile palnik chodzil, nawet jesli ANI RAZU nie zlapalismy go w akcji
    // (pelny wywod przy Model w Viessmann.h).
    {"pc_burner_h", "Piec godziny palnika", "pc", "bh", "duration", "h",
     "total_increasing", nullptr, false},
    {"pc_burner_s", "Piec starty palnika", "pc", "bs", nullptr, nullptr,
     "total_increasing", "mdi:restart", false},
    // Gaz i prad to liczniki DOBOWE (ViCare `currentDay`) â zeruja sie o polnocy.
    // `total_increasing` jest tu wlasciwa klasa WLASNIE dlatego: HA traktuje zejscie
    // do zera jako reset licznika i sumuje dalej poprawnie. Klasa `gas` z jednostka
    // m3 pozwala wpiac gaz do panelu Energia obok pradu i fotowoltaiki.
    {"pc_gas_heat", "Piec gaz CO", "pc", "gh", "gas", "m³", "total_increasing",
     nullptr, false},
    {"pc_gas_dhw", "Piec gaz CWU", "pc", "gd", "gas", "m³", "total_increasing",
     nullptr, false},
    // Prad pieca â JEDYNE jego zuzycie, ktore pokrywa fotowoltaika. Gazu nie pokryje.
    {"pc_el_heat", "Piec prąd CO", "pc", "eh", "energy", "kWh", "total_increasing",
     nullptr, false},
    {"pc_el_dhw", "Piec prąd CWU", "pc", "ed", "energy", "kWh", "total_increasing",
     nullptr, false},
    {"pc_target", "Piec nastawa obiegu", "pc", "tgt", "temperature", "°C", "measurement",
     nullptr, false},
    {"pc_mode", "Piec tryb obiegu", "pc", "mode", nullptr, nullptr, nullptr,
     "mdi:radiator", false},

    // --- samo urzadzenie (kategoria diagnostyczna) ---
    {"dev_temp", "Temperatura ESP32", "dev", "cpu", "temperature", "°C", "measurement",
     nullptr, true},
    {"dev_heap", "Wolna pamięć", "dev", "heap", "data_size", "B", "measurement", nullptr,
     true},
    {"dev_up", "Czas pracy", "dev", "up", "duration", "s", "measurement", nullptr, true},
    {"dev_rssi", "Sygnał Wi-Fi", "dev", "rssi", "signal_strength", "dBm", "measurement",
     nullptr, true},
    {"dev_fw", "Wersja firmware", "dev", "fw", nullptr, nullptr, nullptr, "mdi:chip", true},
};

constexpr int kEntCount = static_cast<int>(sizeof(kEnts) / sizeof(kEnts[0]));

// ------------------------------------------------------------------ discovery --

// Jedna encja = jeden retained config na homeassistant/sensor/<devId>/<key>/config.
// Klucze skrocone (stat_t, val_tpl, dev_cla...) — HA je rozumie, a payload schodzi
// z ~700 B do ~420 B i miesci sie w naszym malym buforze.
bool publishConfig(const Ent& e) {
  const Settings& s = settings();

  char topic[72];
  int tn = snprintf(topic, sizeof(topic), "homeassistant/sensor/%s/%s/config", gDevId, e.key);
  if (tn < 0 || tn >= static_cast<int>(sizeof(topic))) {
    return false;
  }

  char p[480];
  const int cap = static_cast<int>(sizeof(p));
  int n = addf(p, cap, 0,
               "{\"~\":\"%s\",\"name\":\"%s\",\"uniq_id\":\"%s_%s\","
               "\"stat_t\":\"~/%s/state\",\"avty_t\":\"~/status\","
               "\"val_tpl\":\"{{value_json.%s}}\"",
               s.mqttPrefix, e.name, gDevId, e.key, e.group, e.field);
  if (e.devCla != nullptr) n = addf(p, cap, n, ",\"dev_cla\":\"%s\"", e.devCla);
  if (e.unit != nullptr) n = addf(p, cap, n, ",\"unit_of_meas\":\"%s\"", e.unit);
  if (e.staCla != nullptr) n = addf(p, cap, n, ",\"stat_cla\":\"%s\"", e.staCla);
  if (e.icon != nullptr) n = addf(p, cap, n, ",\"ic\":\"%s\"", e.icon);
  if (e.diagnostic) n = addf(p, cap, n, ",\"ent_cat\":\"diagnostic\"");

  // Wspolny blok device — dzieki niemu HA sklei wszystkie 24 encje w JEDNO urzadzenie.
  n = addf(p, cap, n,
           ",\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\",\"mf\":\"premiumads-pl\","
           "\"mdl\":\"ESP32-S3 pogoda+PV\",\"sw\":\"%d\"}}",
           gDevId, s.mqttPrefix, FW_VERSION);

  if (n >= cap) {
    LOG("MQTT: config %s za dlugi (%d B) — pomijam\n", e.key, n);
    return false;
  }
  // 5 B naglowka + 2 B dlugosci tematu — musi sie zmiescic w buforze PubSubClient.
  if (tn + n + 7 > static_cast<int>(kBufSize)) {
    LOG("MQTT: pakiet %s > bufora (%d B) — pomijam\n", e.key, tn + n + 7);
    return false;
  }
  return gCli->publish(topic, reinterpret_cast<const uint8_t*>(p),
                       static_cast<unsigned int>(n), true);
}

// Czujniki BLE nie moga siedziec w statycznej tablicy: ich nazwy pochodza z NVS
// (uzytkownik wpisuje "Łazienka", "Schody"), a liczba zalezy od tego, ile ich
// skonfigurowal. Budujemy wiec Ent na stosie — publishConfig i tak uzywa wskaznikow
// od razu, jeszcze przed powrotem.
// `total` (wyjscie): ile encji BLE PROBOWALISMY wystawic. Bez tego wywolujacy zna sam
// licznik udanych i nie ma go do czego odniesc — a "ile ich w ogole mialo byc" zalezy
// od NVS i nie da sie tego policzyc z zewnatrz.
int sendBleDiscovery(int& total) {
  int ok = 0;
  total = 0;
  // BLE_USABLE, nie "4". Zaszyta czworka byla TRZECIA kopia tej samej petli
  // (ekran i .ino juz poprawione) i jedyna, ktora zostala: czujnik nr 5 pojawilby
  // sie na wyswietlaczu, ale NIGDY nie dostalby encji w Home Assistancie — bez
  // zadnego komunikatu. Objaw "na ekranie jest, w HA go nie ma" jest gorszy niz
  // brak wszedzie, bo nie wskazuje przyczyny.
  for (int i = 0; i < Settings::BLE_USABLE; ++i) {
    const Settings::BleCfg& c = settings().ble[i];
    if (c.mac[0] == '\0') continue;

    const char* room = c.name[0] ? c.name : c.mac;

    struct Def {
      const char* suffix;  // klucz + pole
      const char* label;
      const char* devCla;
      const char* unit;
      bool diag;
    };
    const Def defs[4] = {
        {"t", "temperatura", "temperature", "°C", false},
        {"h", "wilgotność", "humidity", "%", false},
        {"b", "bateria", "battery", "%", true},
        {"r", "sygnał", "signal_strength", "dBm", true},
    };

    for (const Def& d : defs) {
      ++total;
      // (OLED-1) name[48], nie [40]: room (do 23 B, Settings::BleCfg::name) + " — "
      // (5 B, myslnik jest 3-bajtowy UTF-8) + najdluzsza etykieta "wilgotność" (12 B:
      // 8 znakow ASCII + s/c z ogonkiem po 2 B) + NUL = 41 B. Przy [40] snprintf
      // ucinal ostatni bajt dwubajtowego "ć" na skrajnie dlugiej nazwie pokoju —
      // powstawaly niepoprawny UTF-8, HA po cichu odrzucal discovery i encja
      // wilgotnosci dla tego czujnika nie powstawala, bez zadnego bledu w logu.
      char key[16], field[8], name[48];
      snprintf(key, sizeof(key), "ble%d_%s", i, d.suffix);
      snprintf(field, sizeof(field), "s%d%s", i, d.suffix);
      snprintf(name, sizeof(name), "%s — %s", room, d.label);

      const Ent e{key, name, "ble", field, d.devCla, d.unit, "measurement", nullptr, d.diag};
      if (publishConfig(e)) ++ok;
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
  return ok;
}

void sendDiscovery() {
  int bleTotal = 0;
  const int bleOk = sendBleDiscovery(bleTotal);

  int entOk = 0;
  for (int i = 0; i < kEntCount; ++i) {
    if (publishConfig(kEnts[i])) {
      ++entOk;
    }
    // 24 pakiety po ~490 B pod rzad potrafia zapchac okno TCP — oddajemy procesor,
    // zeby webTask (nizszy priorytet, ten sam rdzen) nie zglodnial.
    vTaskDelay(pdMS_TO_TICKS(5));
  }

  // Dwa liczniki, nie jeden. Do v107 szlo tu "discovery %d/%d" z sumy BLE+stale jako
  // licznikiem i samego kEntCount jako mianownikiem — czyli "38/22", bo mianownik
  // opisywal tylko druga polowe licznika. Czytalo sie to jak "38 z 22" i zamiast
  // powiedziec "wszystko poszlo", kazalo szukac nieistniejacej awarii.
  LOG("MQTT: discovery BLE %d/%d + stale %d/%d encji, heap %lu\n", bleOk, bleTotal, entOk,
      kEntCount, static_cast<unsigned long>(ESP.getFreeHeap()));
}

// ------------------------------------------------------- ODBIOR: stan auta ----
// (v174) Do v173 ten klient TYLKO publikowal. Doszedl JEDEN temat przychodzacy:
// <prefix>/auto/stan, ktory Home Assistant publikuje co ~15 s ze stanem Tesli.
//
// Kopiuje najwyzej n-1 znakow do bufora `dst` o rozmiarze `cap` i ZAWSZE zamyka
// go zerem. Osobna funkcja, bo obie wartosci tekstowe (tryb, stan) maja ten sam
// problem: JsonDocument oddaje wskaznik do SWOJEJ puli na stercie, ktora ginie
// razem z dokumentem na koncu onMessage(). Przepisanie do char[] w modelu jest
// wiec obowiazkowe, a nie kosmetyczne.
void copyStr(char* dst, size_t cap, const char* src) {
  if (src == nullptr) { dst[0] = '\0'; return; }
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = '\0';
}

// CALLBACK PubSubClienta — biegnie WEWNATRZ gCli->loop(), czyli w netTasku.
//
// CZEGO TU BYC NIE MOZE: rysowania, dotykania sprite'a i BRANIA gLock (globalnej
// blokady danych z pogoda-gdynia.ino). gLock trzyma rdzen rysujacy na czas
// kopiowania wszystkich modeli klatki; czekanie na niego w callbacku wstrzymaloby
// caly netTask — razem z keepalive MQTT, ktory wlasnie ten callback obsluguje.
// Piszemy wiec do gAutoRx pod WLASNYM mutexem tego modulu (gAutoMx), a przelozeniem
// danych do modelu ekranowego zajmuje sie petla rysujaca przez autoSnapshot().
// To ta sama sciezka, ktora chodza dane z BleGateway i Viessmanna.
void onMessage(char* topic, uint8_t* payload, unsigned int len) {
  // Subskrybujemy DWA tematy (v180) i oba bez znakow wieloznacznych, ale broker
  // potrafi dostarczyc zalegly pakiet z poprzedniej sesji (inny prefiks po zmianie
  // ustawien), wiec temat sprawdzamy zamiast zakladac. Rozgalezienie stoi TUTAJ,
  // a nie w dwoch callbackach: PubSubClient ma jeden wskaznik na funkcje, wiec
  // "drugi callback" i tak musialby byc tym samym `if` — tylko schowanym.
  if (topic == nullptr) return;
  const bool isAuto = strcmp(topic, gAutoTopic) == 0;
  const bool isCost = !isAuto && strcmp(topic, gCostTopic) == 0;
  if (!isAuto && !isCost) return;

  // LADUNEK NIE JEST ZAKONCZONY ZEREM — PubSubClient oddaje wskaznik w srodek
  // swojego bufora razem z dlugoscia. Dlatego deserializeJson dostaje jawna
  // dlugosc, a nie sam wskaznik; przepisywanie do wlasnego bufora tylko po to,
  // zeby dokleic NUL, byloby 200 B stosu bez powodu.
  if (len == 0) return;

  // (v180) STAN DOMU — <prefix>/dom/stan. Osobny, krotki tor: ladunek jest maly,
  // pol ma dzis jedno, a caly parser miesci sie przed rozbudowanym torem auta nizej.
  if (isCost) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, static_cast<size_t>(len))) {
      // Popsuta wiadomosc NIE KASUJE poprzedniej — ta sama zasada, co przy auto/stan
      // i przy gPv/gVi w netTasku od v161: ekran ma pokazac ostatnia znana kwote
      // i jej wiek, a nie pustke.
      LOG("MQTT: /dom/stan — zly JSON (%u B)\n", len);
      return;
    }
    // is<float>() zamiast golego przypisania — ta sama pulapka, co przy `soc` nizej:
    // ArduinoJson po cichu oddaje 0 dla pola, ktorego NIE MA, a "0,00 zł" to zdanie
    // "dzis nic nie kupilismy", czyli konkretne klamstwo, a nie brak danych. Ladunek
    // ma byc ROZSZERZALNY, wiec sprawdzamy WYLACZNIE `zl` i nie wymagamy kompletu:
    // wiadomosc z nowymi polami, ktorych ta wersja nie zna, ma dalej dzialac.
    if (!doc["zl"].is<float>()) {
      LOG("MQTT: /dom/stan — brak pola zl, pomijam\n");
      return;
    }
    CostModel c{};
    // Przyciecie do zakresu SENSOWNEGO, nie do zakresu typu — ta sama zasada, co przy
    // soc/limit/km nizej. Ujemny koszt zakupu nie istnieje (to jest energia POBRANA),
    // a gorna granica bierze sie z szerokosci napisu: linia w module PRAD jest
    // policzona do "999,99 zł" i przy wiekszej liczbie zaczelaby wchodzic w kolumne
    // kontekstu. Realny sufit doby to ~700 zl (20 kW non stop po stawce szczytowej).
    float zl = doc["zl"].as<float>();
    if (!(zl > 0.f)) zl = 0.f;            // lapie takze NaN — porownanie z NaN jest falszywe
    else if (zl > 999.99f) zl = 999.99f;
    c.zl = zl;

    // (v181) DRUGIE POLE TEGO LADUNKU: "pv" — skumulowana korzysc z fotowoltaiki
    // w PELNYCH ZLOTYCH (ekran ZWROT). Ten sam wzorzec, co przy `zl` wyzej, z JEDNA
    // roznica, ktora jest cala tresc komentarza przy `hasPv` nizej.
    //
    // is<float>(), a NIE is<int>(), mimo ze pole jest calkowite: is<int>() w
    // ArduinoJson odpowiada na pytanie "jak to zapisano", a nie "czy to liczba" —
    // gdyby Home Assistant kiedykolwiek wypuscil 13279.0 zamiast 13279 (a szablony
    // HA robia to przy byle dzieleniu), warunek by odpadl i ekran ZWROT zamarlby
    // na ostatniej znanej kwocie bez zadnego sladu w logu poza jedna linijka.
    // is<float>() przyjmuje OBA zapisy; do int32 schodzimy sami, z zaokragleniem.
    const bool hasPv = doc["pv"].is<float>();
    if (hasPv) {
      // Przyciecie do zakresu SENSOWNEGO, nie do zakresu typu — jak przy `zl`.
      // Dol: ujemna skumulowana korzysc nie istnieje (licznik tylko rosnie).
      // Gora: 999 999 zl to szerokosc napisu "wróciło 999 999 zł" na ekranie ZWROT;
      // realny sufit to ~34 000 zl (pelny zwrot) i jeszcze dlugie lata po nim.
      float pv = doc["pv"].as<float>();
      if (!(pv > 0.f)) pv = 0.f;          // lapie takze NaN
      else if (pv > 999999.f) pv = 999999.f;
      c.pvPln = static_cast<int32_t>(pv + 0.5f);
    }

    // (v182) TRZECIE POLE TEGO LADUNKU: "t" — STREFA TARYFY G12w (1 = droga, 0 = tania).
    // Ten sam wzorzec, co przy `zl` i `pv` wyzej, wlacznie z is<float>() zamiast
    // is<int>(): powod jest identyczny (szablon Home Assistanta potrafi wypuscic 1.0
    // zamiast 1 przy byle dzieleniu), a tu wazy podwojnie, bo pole jest DWUSTANOWE
    // i cichy odpad warunku zostawilby -1 na zawsze — czyli plakietke, ktora nigdy
    // sie nie pojawia, bez jednego sladu poza linijka w logu.
    //
    // STREFY NIE LICZYMY TUTAJ Z ZEGARA I TO JEST DECYZJA, NIE LENISTWO — pelne
    // uzasadnienie stoi przy polu `tariff` w CostData.h (dzien tygodnia + swieta
    // ustawowe, kalendarz Workday po stronie HA). Tu tylko przepisujemy gotowa liczbe.
    const bool hasT = doc["t"].is<float>();
    if (hasT) {
      // Przyciecie do DWOCH legalnych stanow, nie do zakresu typu — jak przy `zl`/`pv`.
      // Wszystko, co nie jest czystym 0 albo 1 (NaN, 2, -5, smiec po zlym szablonie),
      // ma znaczyc "NIE WIEM" i zgasic plakietke, a NIE wybrac jednej ze stref na
      // chybil-trafil. Zla plakietka jest gorsza niz jej brak: wlasciciel podejmuje
      // na jej podstawie decyzje o ladowaniu auta.
      const float t = doc["t"].as<float>();
      if (t > 0.5f && t < 1.5f) c.tariff = 1;
      else if (t > -0.5f && t < 0.5f) c.tariff = 0;
      else c.tariff = -1;
    }

    // (v192) CZWARTE POLE TEGO LADUNKU: "zl_s" — PRZYCHOD ZE SPRZEDAZY od polnocy,
    // w ZLOTOWKACH (na strukturze trzymamy grosze — uzasadnienie typu przy polu
    // sellGr w CostData.h). Ten sam wzorzec i to samo is<float>(), co wyzej.
    //
    // WARTOSC PRZYCHODZI JUZ PO KOREKCIE 0,956 — mnozy Home Assistant, bo wspolczynnik
    // ma byc poprawiany suwakiem, a nie wgrywaniem firmware'u. Tu go NIE STOSUJEMY
    // drugi raz; gdyby kiedys zniknal z szablonu HA, ekran zawyzy o ~4,6% i to jest
    // swiadomie wybrany kierunek bledu: liczba zawyzona rzuca sie w oczy przy
    // porownaniu z faktura, a po cichu zanizona nie rzuca sie nigdy.
    const bool hasS = doc["zl_s"].is<float>();
    if (hasS) {
      // Przyciecie do zakresu SENSOWNEGO, nie do zakresu typu — jak przy `zl`/`pv`.
      // Dol: ujemny przychod nie istnieje (to suma energii ODDANEJ, nie bilans).
      // Gora: 655,35 zl to sufit uint16_t w groszach, a nie liczba wzieta z sufitu —
      // fizyczne maksimum tej instalacji to ~41 zl na dobe (CostData.h). Przyciecie
      // jest tu po to, zeby smiec z szablonu nie przekrecil sie w male dodatnie
      // grosze i nie udawal wiarygodnej kwoty.
      float s = doc["zl_s"].as<float>();
      if (!(s > 0.f)) s = 0.f;            // lapie takze NaN
      else if (s > 655.35f) s = 655.35f;
      c.sellGr = static_cast<uint16_t>(s * 100.f + 0.5f);
    }

    c.atMs = millis();
    if (c.atMs == 0) c.atMs = 1;   // 0 znaczy "nigdy" — patrz ten sam zabieg przy aucie
    if (gAutoMx != nullptr && xSemaphoreTake(gAutoMx, pdMS_TO_TICKS(20)) == pdTRUE) {
      // BRAK POLA "pv" NIE KASUJE POPRZEDNIEJ WARTOSCI. Ladunek jest z zalozenia
      // rozszerzalny i NIEKOMPLETNY (patrz naglowek CostData.h): wiadomosc bez "pv"
      // jest legalna i znaczy "nie mam nowej liczby", a nie "korzysc spadla do zera".
      // Bez tego przeniesienia jedna taka wiadomosc zbilaby ekran ZWROT do 0% i 0 zl
      // — i to na 60 s, do nastepnej publikacji. Odczyt STAREJ wartosci siedzi POD
      // TYM SAMYM mutexem, co zapis nowej: gCostRx pisze ten watek, a czyta go
      // costSnapshot() z loop(), wiec para odczyt-zapis musi byc nierozdzielna.
      // (v182) `tariff` idzie DOKLADNIE ta sama sciezka, co pvPln linijke wyzej:
      // wiadomosc bez pola "t" ma zostawic poprzednia strefe, a nie zgasic plakietki.
      // Roznica wobec pvPln jest tylko taka, ze tu "brak" ma juz swoja reprezentacje
      // (-1) — i wlasnie dlatego przeniesienie jest KONIECZNE, a nie kosmetyczne:
      // bez niego swiezy CostModel{} wnosilby -1 przy kazdej wiadomosci bez "t"
      // i plakietka MIGALABY co 60 s miedzy kolorem a pustka.
      // (v192) `sellGr` idzie ta sama sciezka, co pvPln i tariff — z jednym
      // zastrzezeniem, ktore trzeba powiedziec wprost, bo brzmi jak sprzecznosc:
      // przeniesienie chroni przed BRAKIEM POLA, a nie przed POLEM ROWNYM ZERU.
      // Szablon HA ma tam float(0), wiec gdy licznik sprzedazy jest niedostepny,
      // przyjdzie jawne 0 i ekran pokaze 0,00 zl. Tak ma byc: o polnocy licznik
      // NAPRAWDE zeruje sie do zera i przeniesienie starej wartosci zamrozilo by
      // wczorajszy przychod na calą noc. Cena tego wyboru: przy awarii samego
      // czujnika zobaczymy zero zamiast ostatniej znanej kwoty — czyli to samo,
      // co robi `zl` obok, i ta sama zasada, ktora kazala szablonom zwracac `none`
      // zamiast zera (ha/energia.py): dziura ma wygladac na dziure.
      const int32_t keepPv = gCostRx.pvPln;
      const int8_t keepT = gCostRx.tariff;
      const uint16_t keepS = gCostRx.sellGr;
      gCostRx = c;
      if (!hasPv) gCostRx.pvPln = keepPv;
      if (!hasT) gCostRx.tariff = keepT;
      if (!hasS) gCostRx.sellGr = keepS;
      xSemaphoreGive(gAutoMx);
    }
    // Nieudane wziecie mutexu gubi TE JEDNA wiadomosc; nastepna przyjdzie za 60 s,
    // a prog cfg::COST_STALE_MS (3 min) jest wlasnie na to z zapasem.
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, payload, static_cast<size_t>(len))) {
    // Niepelna albo popsuta wiadomosc NIE MOZE skasowac tej poprzedniej: ekran ma
    // wtedy pokazywac ostatni znany stan i jego wiek, a nie pustke. Tak samo dziala
    // netTask z gPv/gVi od v161 — blad zostawia dane nietkniete.
    LOG("MQTT: /auto/stan — zly JSON (%u B)\n", len);
    return;
  }

  AutoModel a{};
  // is<...>() zamiast golego przypisania: ArduinoJson po cichu oddaje 0 dla pola,
  // ktorego nie ma (ta sama pulapka, co opisana przy Viessmann.h), a "0 kW przy
  // 0% baterii" wyglada jak prawdziwy pomiar. Wymagamy wiec, zeby wiadomosc niosla
  // komplet — inaczej odrzucamy ja w calosci i zostajemy przy poprzedniej.
  if (!doc["soc"].is<int>() || !doc["tryb"].is<const char*>() ||
      !doc["stan"].is<const char*>()) {
    LOG("MQTT: /auto/stan — brak pol soc/tryb/stan, pomijam\n");
    return;
  }

  const int soc = doc["soc"].as<int>();
  const int lim = doc["limit"] | 0;
  const int amp = doc["a"] | 0;
  // Przyciecie do zakresu pola, nie do zakresu fizyki: soc i limit ida na pasek
  // baterii jako ulamek szerokosci, wiec 120% wypchneloby wypelnienie poza ekran.
  a.soc = static_cast<uint8_t>(soc < 0 ? 0 : (soc > 100 ? 100 : soc));
  a.limitPct = static_cast<uint8_t>(lim < 0 ? 0 : (lim > 100 ? 100 : lim));
  a.amps = static_cast<uint8_t>(amp < 0 ? 0 : (amp > 255 ? 255 : amp));
  const int km = doc["km"] | 0;
  a.rangeKm = static_cast<int16_t>(km < 0 ? 0 : (km > 9999 ? 9999 : km));
  a.kw = doc["kw"] | 0.f;
  a.addedKwh = doc["kwh"] | 0.f;
  a.sunKwh = doc["sl"] | 0.f;
  a.gridKwh = doc["si"] | 0.f;
  a.cable = (doc["kabel"] | 0) != 0;
  // (v186) `ble` — czy klucz BLE ma ZYWE polaczenie z autem. Ten sam wzorzec, co
  // `kabel` wyzej: brak pola daje 0, czyli false. To POLE NIEOBOWIAZKOWE i celowo
  // NIE dolaczylo do sprawdzenia kompletu soc/tryb/stan kilka linijek wyzej —
  // starsza automatyka, ktora go jeszcze nie wysyla, ma dalej dostarczac ekran AUTO,
  // a nie zostac odrzucona w calosci przez jedna nowa nazwe.
  //
  // DLACZEGO HOME ASSISTANT LICZY TO Z SYGNALU, A NIE ZE STATUSU — bo tylko sygnal
  // nie klamie. Zrodlem jest `sensor.garaz_tesla_klucz_ble_sygnal_ble`, a NIE
  // `binary_sensor ... status` i NIE przelacznik `Polaczenie BLE`: tamte dwa stoja
  // na `on` takze wtedy, gdy auta nie ma w zasiegu (sprawdzone 26.08: oba `on`,
  // sygnal `unknown`, auto na podjezdzie). Sile sygnalu da sie odczytac wylacznie
  // przy zywej sesji BLE — i wlasnie dlatego to ona rozstrzyga, czy polecenie
  // ladowania z panelu ma przez co pojsc. Pelny opis stoi przy polu w AutoData.h.
  a.bleLink = (doc["ble"] | 0) != 0;
  // (v188) `sp` — UDZIAL SLONCA w biezacym ladowaniu, 0..100 %. Ten sam wzorzec, co
  // `ble` i `kabel` wyzej: pole NIEOBOWIAZKOWE, brak daje 0, i celowo NIE wchodzi do
  // sprawdzenia kompletu soc/tryb/stan — starsza automatyka ma dalej dostarczac ekran
  // AUTO, a nie zostac odrzucona w calosci przez jedna nowa nazwe.
  //
  // PRZYCINAMY DO 0..100 tak samo, jak soc i limit: to jest UDZIAL, wiec 120 % nie
  // znaczy nic, a panel liczy z tego pola progi 10/90 i po przekroczeniu zakresu
  // rysowalby ikone, ktora nie odpowiada zadnemu stanowi swiata.
  const int sp = doc["sp"] | 0;
  a.sunPct = static_cast<uint8_t>(sp < 0 ? 0 : (sp > 100 ? 100 : sp));
  copyStr(a.mode, sizeof(a.mode), doc["tryb"]);
  copyStr(a.state, sizeof(a.state), doc["stan"]);
  a.atMs = millis();
  // atMs == 0 znaczy "nigdy nie bylo wiadomosci", wiec przy przekreceniu millis()
  // (raz na ~49 dni) nie wolno go tak zostawic — jedna milisekunda roznicy jest
  // niewidoczna, a falszywe "nigdy" wygasiloby ekran na 45 s.
  if (a.atMs == 0) a.atMs = 1;

  // Sprawdzenie na nullptr, mimo ze mutex powstaje w inicjalizacji statycznej:
  // xSemaphoreCreateMutex() moze oddac nullptr przy braku sterty, a xSemaphoreTake()
  // na pustym uchwycie to asercja rdzenia, czyli restart urzadzenia. Ta sama obrona
  // stoi w autoSnapshot() — bez niej brak 80 B sterty przy starcie wywracalby caly
  // program zamiast tylko wygasic jeden ekran.
  if (gAutoMx != nullptr && xSemaphoreTake(gAutoMx, pdMS_TO_TICKS(20)) == pdTRUE) {
    gAutoRx = a;
    xSemaphoreGive(gAutoMx);
  }
  // Nieudane wziecie mutexu (20 ms to i tak wiecznosc jak na kopie 44 B) oznacza
  // tylko tyle, ze ta jedna wiadomosc przepadla — nastepna przyjdzie za 15 s.
  // Czekanie w nieskonczonosc wstrzymaloby keepalive MQTT.
}

// (v175) WYSLANIE ZAMOWIENIA Z PANELU. Wolane WYLACZNIE z mqttha::loop(), czyli
// z netTaska — to jedyne zadanie, ktore ma prawo dotykac gCli.
//
// retain = FALSE, i to nie jest przeoczenie: to POLECENIE, a nie stan. Retained
// polecenie broker dosylalby kazdemu nowemu subskrybentowi — czyli po restarcie
// Home Assistanta auto dostawaloby "MAX" sprzed tygodnia jako swiezy rozkaz.
// Stanem jest <prefix>/auto/stan i to on jest retained po tamtej stronie.
void flushModeRequest() {
  if (gModeReqState != 1) return;

  // Brak klienta albo brak polaczenia konczy sprawe NATYCHMIAST bledem, zamiast
  // trzymac zamowienie do skutku. Panel ma 10 s na komunikat i lepiej, zeby napisal
  // "nie wyslano" od razu, niz zeby wlasciciel patrzyl w "wysylam..." przez caly
  // backoff brokera (do 5 minut).
  if (gCli == nullptr || !gCli->connected()) {
    gModeReqState = 3;
    return;
  }

  char topic[48];
  const int tn = snprintf(topic, sizeof(topic), "%s/auto/tryb/set", settings().mqttPrefix);
  if (tn < 0 || tn >= static_cast<int>(sizeof(topic))) {
    gModeReqState = 3;
    return;
  }

  // Kopia pod mutexem: panel moze wlasnie nadpisywac gModeReq nowym wyborem.
  char m[sizeof(gModeReq)] = {};
  if (gAutoMx != nullptr && xSemaphoreTake(gAutoMx, pdMS_TO_TICKS(20)) == pdTRUE) {
    memcpy(m, gModeReq, sizeof(m));
    xSemaphoreGive(gAutoMx);
  }
  if (m[0] == '\0') {
    gModeReqState = 3;
    return;
  }

  const bool ok = gCli->publish(topic, m, false);
  gModeReqState = ok ? 2 : 3;
  if (ok) {
    diag().mqttOkAt = millis();
    ++diag().mqttPublished;
    LOG("MQTT: panel OLED -> %s = %s\n", topic, m);
  } else {
    setErr("Broker odrzucił wybór trybu");
  }
}

// Subskrypcje tematow PRZYCHODZACYCH. Wolana po KAZDYM udanym polaczeniu, bo broker
// nie pamieta subskrypcji zerwanej sesji (laczymy sie z cleanSession — PubSubClient
// nie umie inaczej), wiec po kazdym zerwaniu trzeba je zlozyc od nowa.
//
// (v180) Byla to `subscribeAuto()` z jednym tematem. Nazwa i tresc urosly o drugi
// (<prefix>/dom/stan), ale MECHANIZM zostal JEDEN: jedna funkcja, jedno miejsce
// wywolania, ta sama chwila w cyklu polaczenia. Druga funkcja wolana obok byla by
// drugim miejscem, ktore trzeba pamietac po kazdej zmianie w tryConnect() — a to
// dokladnie ten rodzaj rozjazdu, ktory ten plik juz raz kosztowal (patrz "38/22"
// przy sendDiscovery).
//
// Temat skladamy TU, a nie raz przy starcie: prefiks moze sie zmienic z panelu WWW,
// a configChanged() zrywa polaczenie — czyli po zmianie ustawien i tak przechodzimy
// tedy. Bufory maja 40 B, a najdluzszy temat to 23 znaki prefiksu + "/auto/stan",
// czyli 33 B z zerem; snprintf i tak by przycial.
void subscribeTopics() {
  snprintf(gAutoTopic, sizeof(gAutoTopic), "%s/auto/stan", settings().mqttPrefix);
  if (!gCli->subscribe(gAutoTopic)) {
    LOG("MQTT: nie udalo sie zasubskrybowac %s\n", gAutoTopic);
  }
  snprintf(gCostTopic, sizeof(gCostTopic), "%s/dom/stan", settings().mqttPrefix);
  if (!gCli->subscribe(gCostTopic)) {
    LOG("MQTT: nie udalo sie zasubskrybowac %s\n", gCostTopic);
  }
}

// ------------------------------------------------------------------ transport --

void teardown(bool sayGoodbye) {
  if (gCli != nullptr) {
    if (sayGoodbye && gCli->connected()) {
      gCli->publish(gAvail, "offline", true);
      gCli->disconnect();
    }
    delete gCli;
    gCli = nullptr;
  }
  if (gSock != nullptr) {
    gSock->stop();
    delete gSock;
    gSock = nullptr;
  }
}

bool ensureClient() {
  if (gCli != nullptr) {
    return true;
  }
  gSock = new (std::nothrow) WiFiClient();
  if (gSock == nullptr) {
    return false;
  }
  gCli = new (std::nothrow) PubSubClient(*gSock);
  if (gCli == nullptr) {
    delete gSock;
    gSock = nullptr;
    return false;
  }
  if (!gCli->setBufferSize(kBufSize)) {  // malloc 512 B — moze nie wyjsc przy fragmentacji
    teardown(false);
    return false;
  }
  gCli->setKeepAlive(kKeepAliveS);
  gCli->setSocketTimeout(kSockTimeoutS);
  // (v174) Callback podpinamy PRZED connect() i raz na zycie obiektu — inaczej
  // pierwszy retained pakiet, ktory broker dosyla natychmiast po SUBSCRIBE, trafilby
  // w pusty wskaznik i przepadl bez sladu.
  gCli->setCallback(onMessage);
  return true;
}

void makeIds() {
  if (gDevId[0] == '\0') {
    uint8_t mac[6] = {};
    WiFi.macAddress(mac);
    snprintf(gDevId, sizeof(gDevId), "pg_%02x%02x%02x", mac[3], mac[4], mac[5]);
  }
  snprintf(gAvail, sizeof(gAvail), "%s/status", settings().mqttPrefix);
}

// (P1-1) TYLKO podwaja odstep — NIE dotyka gLastTryAt. Zapis czasu proby nalezy do
// wolajacego (loop() nizej), bo to on wie, KIEDY faktycznie zaczela sie proba.
void backoff() {
  gBackoffMs = (gBackoffMs >= kBackoffMaxMs / 2) ? kBackoffMaxMs : gBackoffMs * 2;
}

bool tryConnect() {
  const Settings& s = settings();
  makeIds();

  if (!ensureClient()) {
    setErr("Za mało RAM na klienta MQTT");
    backoff();
    return false;
  }

  gSock->setConnectionTimeout(kConnTimeoutMs);
  gCli->setServer(s.mqttHost, s.mqttPort);

  const char* user = (s.mqttUser[0] != '\0') ? s.mqttUser : nullptr;
  const char* pass = (s.mqttPass[0] != '\0') ? s.mqttPass : nullptr;

  // LWT: broker sam ogłosi "offline", gdy urzadzenie zniknie bez pozegnania.
  const bool ok = gCli->connect(gDevId, user, pass, gAvail, 0, true, "offline", true);
  if (!ok) {
    const int st = gCli->state();
    setErr(stateText(st));
    LOG("MQTT: brak polaczenia z %s:%u (stan %d)\n", s.mqttHost, s.mqttPort, st);
    gSock->stop();  // nie zostawiaj wiszacego gniazda
    backoff();
    return false;
  }

  gBackoffMs = kBackoffMinMs;
  ++diag().mqttConnects;
  clearErr();
  LOG("MQTT: polaczono z %s:%u jako %s, heap %lu\n", s.mqttHost, s.mqttPort, gDevId,
      static_cast<unsigned long>(ESP.getFreeHeap()));

  gCli->publish(gAvail, "online", true);
  // (v174) SUBSKRYPCJA PRZED discovery, nie po. sendDiscovery() wysyla 40 pakietow
  // z vTaskDelay miedzy nimi, czyli trwa grubo ponad sekunde — a przez ten czas
  // ekran AUTO nie mialby jeszcze skad wziac danych, choc broker ma je gotowe jako
  // retained i odda w tej samej milisekundzie, w ktorej dostanie SUBSCRIBE.
  // (v180) Dotyczy tak samo drugiego tematu (<prefix>/dom/stan), choc TAM retained
  // NIE MA: automatyka publikuje koszt bez flagi, wiec po polaczeniu i tak czekamy
  // do minuty na pierwsza wiadomosc. Tym bardziej nie ma powodu dokladac do tego
  // czekania jeszcze sekundy discovery.
  subscribeTopics();
  sendDiscovery();
  // (P1-1) millis(), NIE 0: przy uptime > 24,85 dnia static_cast<int32_t>(millis() - 0)
  // jest UJEMNY, wiec sprawdzenie w loop() (`millis() - gNextDevAt >= 0`) wypadaloby
  // falszywie i publishDevice() nie ruszyloby az do 49,7 dnia (dokladnie ten sam bug,
  // co przy backoffie — 0 jako sentynel "od razu" myli sie z prawdziwym stemplem 0).
  // millis() ustawia "termin" na TERAZ, wiec sprawdzenie wypada prawdziwe od razu,
  // niezaleznie od aktualnego uptime.
  gNextDevAt = millis();  // od razu wyslij telemetrie urzadzenia
  return true;
}

// -------------------------------------------------------------------- publish --

bool pubState(const char* group, const char* json, int len) {
  if (gCli == nullptr || !gCli->connected()) {
    return false;
  }
  char topic[48];
  const int tn = snprintf(topic, sizeof(topic), "%s/%s/state", settings().mqttPrefix, group);
  if (tn < 0 || tn >= static_cast<int>(sizeof(topic))) {
    return false;
  }
  // retain: po restarcie HA od razu widzi ostatnie wartosci, nie czeka 15 minut
  const bool ok = gCli->publish(topic, reinterpret_cast<const uint8_t*>(json),
                                static_cast<unsigned int>(len), true);
  if (ok) {
    diag().mqttOkAt = millis();
    ++diag().mqttPublished;
    clearErr();
  } else {
    setErr("Broker odrzucił publikację");
  }
  return ok;
}

void publishDevice() {
  char p[160];
  const int n = snprintf(p, sizeof(p),
                         "{\"cpu\":%.1f,\"heap\":%lu,\"up\":%lu,\"rssi\":%d,\"fw\":%d}",
                         temperatureRead(),
                         static_cast<unsigned long>(ESP.getFreeHeap()),
                         static_cast<unsigned long>(millis() / 1000), WiFi.RSSI(),
                         FW_VERSION);
  if (n > 0 && n < static_cast<int>(sizeof(p))) {
    pubState("dev", p, n);
  }
}

// Opis pogody dla HA — pelniejszy niz trzyliterowa etykieta z ekranu.
const char* wxDescription(int code) {
  switch (code) {
    case 0: return "Bezchmurnie";
    case 1: return "Głównie słonecznie";
    case 2: return "Częściowe zachmurzenie";
    case 3: return "Pochmurno";
    case 45:
    case 48: return "Mgła";
    case 51:
    case 53:
    case 55: return "Mżawka";
    case 56:
    case 57: return "Marznąca mżawka";
    case 61: return "Słaby deszcz";
    case 63: return "Deszcz";
    case 65: return "Ulewa";
    case 66:
    case 67: return "Marznący deszcz";
    case 71: return "Słaby śnieg";
    case 73: return "Śnieg";
    case 75: return "Intensywny śnieg";
    case 77: return "Śnieg ziarnisty";
    case 80:
    case 81: return "Przelotny deszcz";
    case 82: return "Gwałtowna ulewa";
    case 85:
    case 86: return "Przelotny śnieg";
    case 95: return "Burza";
    case 96:
    case 99: return "Burza z gradem";
    default: return "Pogoda";
  }
}

}  // namespace

// ===================================================================== API =====

void configChanged() {
  gReconfig = true;
}

// (v174) Migawka stanu auta dla warstwy rysowania — pelny opis przy deklaracji
// w MqttClient.h.
bool autoSnapshot(AutoModel& out) {
  if (gAutoMx == nullptr) return false;
  // Krotki timeout zamiast portMAX_DELAY: wola to petla rysujaca, a jedyny
  // konkurent trzyma mutex na czas przepisania 44 B. Gdyby mimo to nie wyszlo,
  // klatka zostaje przy poprzednich danych — a nie zacina sie ekran.
  if (xSemaphoreTake(gAutoMx, pdMS_TO_TICKS(5)) != pdTRUE) return false;
  const bool have = gAutoRx.atMs != 0;
  if (have) out = gAutoRx;
  xSemaphoreGive(gAutoMx);
  return have;
}

// (v180) Migawka stanu domu (koszt zakupu z sieci) — pelny opis przy deklaracji
// w MqttClient.h. Linia w linie to samo, co autoSnapshot() wyzej, z tym samym
// mutexem i tym samym krotkim timeoutem: wola to petla rysujaca, a nieudane wziecie
// zostawia klatke przy poprzednich danych zamiast zacinac ekran.
bool costSnapshot(CostModel& out) {
  if (gAutoMx == nullptr) return false;
  if (xSemaphoreTake(gAutoMx, pdMS_TO_TICKS(5)) != pdTRUE) return false;
  const bool have = gCostRx.atMs != 0;
  if (have) out = gCostRx;
  xSemaphoreGive(gAutoMx);
  return have;
}

// (v175) Zamowienie z panelu OLED — pelny opis przy deklaracji w MqttClient.h.
// Wolane z petli rysowania (rdzen 1); TU nie ma ani jednego dotkniecia gCli.
void requestAutoMode(const char* mode) {
  if (mode == nullptr || mode[0] == '\0') return;
  if (gAutoMx != nullptr && xSemaphoreTake(gAutoMx, pdMS_TO_TICKS(5)) == pdTRUE) {
    strncpy(gModeReq, mode, sizeof(gModeReq) - 1);
    gModeReq[sizeof(gModeReq) - 1] = '\0';
    xSemaphoreGive(gAutoMx);
    // Stan podnosimy DOPIERO po udanym zapisie napisu — inaczej netTask mogl by
    // zobaczyc "czeka" przy pustym albo poprzednim trybie.
    gModeReqState = 1;
    return;
  }
  // Nie udalo sie wziac mutexu (5 ms): zamowienie NIE zostalo zlozone i panel ma
  // sie o tym dowiedziec, zamiast czekac 10 s na potwierdzenie czegos, czego nikt
  // nie wyslal.
  gModeReqState = 3;
}

uint8_t autoModeReqState() {
  return gModeReqState;
}

void loop() {
  const Settings& s = settings();

  // (v175) Zamowienie trybu z panelu OLED zalatwiamy PRZED wszystkimi wczesnymi
  // powrotami ponizej. Kazdy z nich (MQTT wylaczony, brak WiFi, backoff) jest
  // powodem, dla ktorego wysylka sie NIE UDA — a panel ma dostac odpowiedz "nie
  // wyslano" zaraz, nie po ustaniu przyczyny. flushModeRequest() sam sprawdza
  // gCli/connected(), wiec dziala poprawnie w kazdym z tych stanow.
  flushModeRequest();

  // MQTT wylaczony: oddaj stertę i nie udawaj bledu na ekranie statystyk.
  if (!s.hasMqtt()) {
    if (gCli != nullptr) {
      teardown(true);
      LOG("MQTT: wyłączony — klient zwolniony\n");
    }
    gReconfig = false;
    clearErr();
    diag().mqttOkAt = 0;
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (gReconfig) {
    gReconfig = false;
    teardown(true);
    gBackoffMs = kBackoffMinMs;
    // (P1-1) 0 jest tu bezpieczne (w odroznieniu od gNextDevAt wyzej), bo ponizsze
    // sprawdzenie liczy UPLYW czasu (odejmowanie bez znaku), nie porownuje adresu w
    // przyszlosci: millis() - 0 to po prostu millis(), niemal zawsze >= gBackoffMs.
    gLastTryAt = 0;
    diag().mqttOkAt = 0;
  }

  if (gCli != nullptr && gCli->connected()) {
    // Keepalive I ODBIOR: to wlasnie tutaj, w srodku tego wywolania, PubSubClient
    // wola onMessage() dla kazdego przychodzacego PUBLISH (<prefix>/auto/stan).
    // Czyli callback biegnie w netTasku — patrz komentarz przy onMessage().
    gCli->loop();
    if (static_cast<int32_t>(millis() - gNextDevAt) >= 0) {
      publishDevice();
      gNextDevAt = millis() + kDevPublishMs;
    }
    return;
  }

  // (P1-1) Uplyw czasu OD OSTATNIEJ PROBY, odejmowanie BEZ ZNAKU — nie "czy minelismy
  // absolutny termin w przyszlosci" jak dawniej. Stary wzor (gNextTryAt = millis() +
  // backoff, sprawdzane przez static_cast<int32_t>(millis() - gNextTryAt) < 0) mial
  // ukryty sentynel: gNextTryAt == 0 mialo znaczyc "wolno od razu", ale po ~24,85 dnia
  // uptime static_cast<int32_t>(millis()) samo z siebie jest UJEMNE, wiec ten sam
  // warunek dawal "jeszcze nie czas" az do przekrecenia millis() przy 49,7 dnia —
  // reconnect byl zablokowany na ~25 dni. Odejmowanie bez znaku nie ma tego problemu:
  // (millis() - gLastTryAt) zawija sie poprawnie niezaleznie od aktualnego uptime,
  // dopoki sam odstep miedzy probami miesci sie w 32 bitach (miesci sie z ogromnym
  // zapasem — max backoff to 5 minut).
  if (millis() - gLastTryAt < gBackoffMs) {
    return;  // backoff — broker moze sobie lezec, urzadzenie dziala dalej
  }
  gLastTryAt = millis();
  tryConnect();
}

// Czujniki BLE — jeden JSON na wszystkie, klucze s0t/s0h/s0b/s0r ... s3r.
// Wysylamy tylko te, ktore maja swiezy odczyt; czujnik, ktory zamilkl, po prostu
// znika z payloadu, a HA pokaze ostatnia znana wartosc (retained).
void publishBle() {
  if (!settings().hasMqtt() || gCli == nullptr || !gCli->connected()) {
    return;
  }

  // PULAPKA, ktora sama nie wybuchla tylko dzieki limitowi 4 — i wybuchlaby przy
  // naiwnym podniesieniu go na 6:
  //   snprintf() zwraca dlugosc, jaka BY zapisal, a nie ile zapisal. Przy obcieciu
  //   `n` przekracza sizeof(p). Wtedy `sizeof(p) - n` (size_t minus int!) przekreca
  //   sie na ogromna liczbe, a `p + n` wskazuje POZA bufor. Domykajaca klamra za
  //   petla pisala by wprost w stos netTask.
  // Dlatego: rezerwujemy miejsce PRZED rekordem, zamiast sprawdzac szkode po fakcie.
  // Najdluzszy rekord: ,"s5t":-12.3,"s5h":100.0,"s5b":100,"s5r":-100 = 46 B.
  constexpr int kRec = 64;    // zapas na jeden komplet pol czujnika
  char p[384];
  const int cap = static_cast<int>(sizeof(p));
  int n = snprintf(p, sizeof(p), "{");
  bool any = false;

  for (int i = 0; i < ble::count(); ++i) {
    if (n > cap - kRec) break;   // nie zaczynamy rekordu, na ktory nie ma miejsca

    const ble::Sensor s = ble::get(i);
    if (!s.valid) continue;

    const Settings::BleCfg* cfg = settings().bleFind(s.mac);
    if (cfg == nullptr) continue;  // nieskonfigurowany — nie ma dla niego encji w HA

    // slot musi sie zgadzac z tym z discovery (indeks w settings, nie w ble::)
    int slot = -1;
    for (int k = 0; k < Settings::BLE_USABLE; ++k) {
      if (&settings().ble[k] == cfg) slot = k;
    }
    if (slot < 0) continue;

    if (any) n += snprintf(p + n, cap - n, ",");
    if (s.hasTemp) n += snprintf(p + n, cap - n, "\"s%dt\":%.1f,", slot, s.tempC);
    if (s.hasHum) n += snprintf(p + n, cap - n, "\"s%dh\":%.1f,", slot, s.humidity);
    n += snprintf(p + n, cap - n, "\"s%db\":%d,\"s%dr\":%d", slot, s.batteryPct,
                  slot, s.rssi);
    any = true;
  }

  n += snprintf(p + n, cap - n, "}");
  if (any && n > 0 && n < static_cast<int>(sizeof(p))) {
    pubState("ble", p, n);
  }
}

void publishPv(const PvModel& pv, bool ok) {
  if (!settings().hasMqtt() || gCli == nullptr || !gCli->connected()) {
    return;
  }

  const PvSnapshot& d = pv.data;
  if (ok) {
    gPvCache.todayKwh = d.energyTodayKwh;
    gPvCache.totalKwh = d.energyTotalKwh;
    gPvCache.tempC = d.inverterTempC;
    gPvCache.have = true;
  } else if (!gPvCache.have) {
    return;  // nigdy nie odczytalismy falownika — nie ma co wysylac
  }

  const char* status = ok ? pvStatusLabel(d.statusCode) : "Offline";

  // (v179) 256 B zamiast 224 po dolozeniu `gin`/`gout`. Przeliczony najgorszy
  // przypadek ladunku to 194 B (rozpiska nizej), wiec 224 by wystarczylo — ale
  // z zapasem 30 B, a ten bufor rosnie przy KAZDYM nowym polu i nikt tego zapasu
  // nie widzi, dopoki snprintf po cichu nie utnie JSON-a i pubState() go nie
  // odrzuci. To stos netTaska, nie statyczny RAM: 32 B kosztuje tyle, co nic.
  char p[256];
  const int cap = static_cast<int>(sizeof(p));
  int n = snprintf(
      p, sizeof(p),
      "{\"ac\":%ld,\"dc\":%ld,\"grid\":%ld,\"house\":%ld,"
      "\"today\":%.2f,\"total\":%.2f,\"temp\":%.1f,\"status\":\"%s\"",
      ok ? static_cast<long>(d.powerAcW) : 0L, ok ? static_cast<long>(d.powerDcW) : 0L,
      ok ? static_cast<long>(d.gridPowerW) : 0L, ok ? static_cast<long>(d.houseLoadW) : 0L,
      gPvCache.todayKwh, gPvCache.totalKwh, gPvCache.tempC, status);

  // ===== (v179) LICZNIKI MIERNIKA — WARTOWNIK -1 POMIJAMY, NIE ZERUJEMY ========
  //
  // TO JEST NAJWAZNIEJSZA DECYZJA W TEJ FUNKCJI. Obie encje sa `total_increasing`,
  // a HA traktuje KAZDY spadek takiego licznika jako przekrecenie miernika: bierze
  // nowa (nizsza) wartosc za poczatek nowego cyklu i dolicza cala dotychczasowa
  // sume do statystyki dlugoterminowej. Wyslanie tu `-1` — albo `0`, albo `null` —
  // raz jeden trwale rozjechaloby dane Panelu Energia, a naprawa to reczne
  // grzebanie w bazie `statistics` Home Assistanta, nie restart urzadzenia.
  //
  // PvData.h ustawia meterImportKwh/meterExportKwh na -1.f, gdy odczytu NIE BYLO
  // (wartownik jest -1, a nie 0, wlasnie dlatego, ze 0 jest prawdziwa wartoscia
  // swiezo wymienionego miernika — pelny wywod stoi tam przy deklaracji pol).
  // Wartosc ujemna oznacza wiec "nie wiem", a jedyna uczciwa odpowiedz na "nie
  // wiem" w JSON-ie stanu to CISZA: pole znika z ladunku, val_tpl w HA renderuje
  // sie na pusty napis, a MQTT sensor pusty ladunek IGNORUJE i zostawia encje na
  // poprzednim stanie. Dokladnie to samo robi publishBle() z czujnikiem, ktory
  // zamilkl — tam tez pole po prostu wypada z payloadu.
  //
  // DLACZEGO BEZ WLASNEGO CACHE (jak gPvCache dla today/total). gPvCache istnieje
  // po to, zeby nieudany odczyt nie wyslal ZERA tam, gdzie zero jest nieodroznialne
  // od pomiaru. Tutaj taki cache byl by zbedny: wartownik -1 jest jawnie nie-
  // -pomiarem, wiec da sie go odsiac bez pamietania czegokolwiek, a przeniesienie
  // ostatniego dobrego odczytu i tak juz zrobil netTask (pogoda-gdynia.ino, galaz
  // `!meterEnergyFresh`). Drugi cache byl by druga kopia tej samej prawdy.
  //
  // ROZLICZENIE BAJTOW (najgorszy przypadek ladunku, wartosci na granicach typow):
  //   nazwy pol + interpunkcja stalej czesci ......................  68 B
  //   ac/dc/grid/house, int32 "-2147483648" x4 ....................  44 B
  //   today/total, %.2f z rejestru int32/100 ("21474836.00") x2 ....  22 B
  //   temp, %.1f z int16/10 ("-3276.8") ...........................   7 B
  //   status, najdluzsza etykieta "Praca (derating)" ..............  16 B
  //   ,"gin":  + wartosc %.2f (int32/100, max 11 znakow) ..........  18 B
  //   ,"gout": + wartosc %.2f (int32/100, max 11 znakow) ..........  19 B
  //                                                          RAZEM  194 B
  // Pakiet MQTT: 1 B naglowka stalego + 2 B dlugosci zmiennej + 2 B dlugosci
  // tematu + 32 B tematu (23 znaki maks. prefiksu + "/pv/state") + 194 B ladunku
  // = 231 B, czyli 45% bufora kBufSize. Najwiekszym pakietem zostaje dalej retained
  // config encji (430 B) — bufora NIE ruszamy.
  if (d.meterImportKwh >= 0.f) {
    n = addf(p, cap, n, ",\"gin\":%.2f", d.meterImportKwh);
  }
  if (d.meterExportKwh >= 0.f) {
    n = addf(p, cap, n, ",\"gout\":%.2f", d.meterExportKwh);
  }
  // Zaokraglenie do 2 miejsc, bo gain rejestru wynosi 100 — trzecia cyfra po
  // przecinku byla by szumem po dzieleniu, nie rozdzielczoscia miernika.
  n = addf(p, cap, n, "}");

  // addf() przy przepelnieniu zwraca dlugosc >= cap i zostawia bufor obciety,
  // wiec ten warunek jest tu takze bramka na uciety JSON — lepiej nie wyslac nic
  // niz wyslac polowe ladunku, ktorej HA nie sparsuje.
  if (n > 0 && n < cap) {
    pubState("pv", p, n);
  }
}

// (v197) Tryb obiegu to JEDYNE pole tekstowe pieca, ktore trafiloby do naszego
// JSON-a wprost z odpowiedzi Viessmanna. Mapa, a NIE przepisanie napisu: cudzy
// tekst w naszym ladunku to cudzy cudzyslow w naszym JSON-ie. Mapa zamyka te droge
// raz na zawsze i przy okazji daje polskie nazwy. Nieznany tryb ma isc jako "Inny",
// a nie zniknac — surowa wartosc i tak zostaje w /api/diag do diagnozy.
const char* circuitModeLabel(const char* mode) {
  if (strcmp(mode, "standby") == 0) return "Czuwanie";
  if (strcmp(mode, "heating") == 0) return "Grzanie";
  if (strcmp(mode, "dhw") == 0) return "CWU";
  if (strcmp(mode, "dhwAndHeating") == 0) return "CWU i grzanie";
  if (strcmp(mode, "forcedNormal") == 0) return "Wymuszony";
  if (strcmp(mode, "forcedReduced") == 0) return "Obnizony";
  return "Inny";
}

// (v197) PIEC -> HOME ASSISTANT.
//
// Do v196 firmware czytal ViCare co 3 minuty i nie publikowal z tego ANI JEDNEGO
// pola. Home Assistant nie mial o piecu zadnej encji, wiec caly sezon grzewczy
// przechodzilby bez sladu w statystykach dlugoterminowych — a danych, ktorych sie
// nie zebralo, nie da sie odzyskac (ta sama lekcja co przy panelu Energia:
// "historii przed 26.08 nie ma").
//
// KAZDE POLE POD WLASNA FLAGA has*. Model oddaje 0 zarowno wtedy, gdy piec przyslal
// zero, jak i wtedy, gdy cechy w ogole nie bylo w odpowiedzi. Pole bez pokrycia
// ZNIKA z ladunku — dokladnie tak, jak `gin`/`gout` w publishPv(): MQTT sensor pusty
// ladunek IGNORUJE i zostawia encje na poprzednim stanie.
//
// DLA CZTERECH LICZNIKOW TO JEST KRYTYCZNE, nie kosmetyczne. `gh`/`gd`/`eh`/`ed` sa
// `total_increasing`, a licznik jest DOBOWY i zeruje sie o polnocy. HA to obsluguje,
// bo spadek total_increasing traktuje jako reset. Ale zero wyslane W SRODKU DOBY,
// dlatego ze cecha nie doszla, wyglada dla HA dokladnie tak samo jak ten reset:
// reszta doby zostanie doliczona DRUGI RAZ do statystyki dlugoterminowej, a naprawa
// to reczne grzebanie w tabeli `statistics`, nie restart urzadzenia.
//
// ROZLICZENIE BAJTOW (najgorszy przypadek, wartosci na granicach typow):
//   {"sup":-3276.8 ....................................................  14 B
//   ,"mod": + int32 na granicy ("-2147483648") ........................  18 B
//   ,"bh":  + %.1f ("999999.9") .......................................  16 B
//   ,"bs":  + uint32 ("4294967295") ...................................  16 B
//   ,"gh":  ,"gd":  ,"eh":  ,"ed":  + %.2f ("99999.99") x4 ............  60 B
//   ,"tgt": + %.1f ....................................................  14 B
//   ,"mode":" + najdluzsza etykieta "CWU i grzanie" + " ................  23 B
//   } .................................................................   1 B
//                                                              RAZEM   162 B
// Pakiet: 1 B naglowka + 2 B dlugosci + 2 B dlugosci tematu + 32 B tematu
// (23 znaki maks. prefiksu + "/pc/state") + 162 B = 199 B, czyli 39% kBufSize.
// Najwiekszym pakietem zostaje dalej retained config encji (430 B) — bufora NIE
// ruszamy. Bufor lokalny 224 B stoi na stosie netTaska, nie w statyku.
void publishBoiler(const vi::Model& m) {
  if (!settings().hasMqtt() || gCli == nullptr || !gCli->connected()) {
    return;
  }

  char p[224];
  const int cap = static_cast<int>(sizeof(p));
  int n = addf(p, cap, 0, "{");
  bool any = false;

  if (m.hasSupplyTemp) {
    n = addf(p, cap, n, "\"sup\":%.1f", m.supplyTempC);
    any = true;
  }
  if (m.hasModulation) {
    n = addf(p, cap, n, any ? ",\"mod\":%d" : "\"mod\":%d", m.modulationPct);
    any = true;
  }
  if (m.hasBurnerHours) {
    n = addf(p, cap, n, any ? ",\"bh\":%.1f" : "\"bh\":%.1f", m.burnerHours);
    any = true;
  }
  if (m.hasBurnerStarts) {
    n = addf(p, cap, n, any ? ",\"bs\":%lu" : "\"bs\":%lu",
             static_cast<unsigned long>(m.burnerStarts));
    any = true;
  }
  if (m.hasGasHeat) {
    n = addf(p, cap, n, any ? ",\"gh\":%.2f" : "\"gh\":%.2f", m.gasHeatM3);
    any = true;
  }
  if (m.hasGasDhw) {
    n = addf(p, cap, n, any ? ",\"gd\":%.2f" : "\"gd\":%.2f", m.gasDhwM3);
    any = true;
  }
  if (m.hasPowerHeat) {
    n = addf(p, cap, n, any ? ",\"eh\":%.2f" : "\"eh\":%.2f", m.powerHeatKwh);
    any = true;
  }
  if (m.hasPowerDhw) {
    n = addf(p, cap, n, any ? ",\"ed\":%.2f" : "\"ed\":%.2f", m.powerDhwKwh);
    any = true;
  }
  if (m.hasCircuitTarget) {
    n = addf(p, cap, n, any ? ",\"tgt\":%.1f" : "\"tgt\":%.1f", m.circuitTargetC);
    any = true;
  }
  if (m.circuitMode[0] != '\0') {
    n = addf(p, cap, n, any ? ",\"mode\":\"%s\"" : "\"mode\":\"%s\"",
             circuitModeLabel(m.circuitMode));
    any = true;
  }

  // Pusty ladunek "{}" nie niesie nic, a kosztuje pakiet co 3 minuty i nadpisuje
  // retained wiadomosc. Odczyt bez ANI JEDNEJ znanej cechy to nie jest stan pieca.
  if (!any) {
    return;
  }
  n = addf(p, cap, n, "}");

  // addf() przy przepelnieniu zwraca dlugosc >= cap i zostawia bufor obciety —
  // ten warunek jest wiec takze bramka na uciety JSON. Lepiej nie wyslac nic niz
  // wyslac polowe ladunku, ktorej HA nie sparsuje.
  if (n > 0 && n < cap) {
    pubState("pc", p, n);
  }
}

void publishWeather(const WeatherModel& w) {
  if (!settings().hasMqtt() || gCli == nullptr || !gCli->connected() || !w.ready) {
    return;
  }
  const WeatherSnapshot& c = w.current;

  char p[256];
  const int n = snprintf(
      p, sizeof(p),
      "{\"temp\":%.1f,\"feels\":%.1f,\"hum\":%d,\"pres\":%.1f,\"wind\":%.1f,"
      "\"cloud\":%d,\"uv\":%.1f,\"rain\":%.1f,\"desc\":\"%s\"}",
      c.tempC, c.feelsC, c.humidity, c.pressureHpa, c.windKmh, c.cloudCover, c.uvIndex,
      c.precipMm, wxDescription(c.weatherCode));
  if (n > 0 && n < static_cast<int>(sizeof(p))) {
    pubState("wx", p, n);
  }
}

}  // namespace mqttha
