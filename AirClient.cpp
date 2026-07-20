#include "AirClient.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <cstring>
#include <ctime>

namespace {

// Zwykly HTTP (bez TLS) — zweryfikowane przez wlasciciela na tym konkretnym API, nie
// zgadywane. Oszczedza ~40 kB sterty wzgledem WiFiClientSecure (patrz WeatherClient.cpp
// dla kontrastu — tamten serwer wymaga TLS). Na tym budzecie SRAM (limit 76000 B, patrz
// tools/release.sh) kazdy taki kB ma znaczenie.
constexpr const char* kHost = "pomorskie.cas.sensorbox.pl";
constexpr const char* kPath = "/webapp/data/averages";

// Kolejnosc MUSI zgadzac sie z ta w buildUrl() (i tak jest — spec API to gwarantuje:
// "values[i] odpowiada vars[i] w tej samej kolejnosci"), ale i tak NIE polegamy na
// samej pozycji: findVarIndex() nizej szuka kazdej nazwy w "vars" zwroconym przez
// serwer i dopiero POD TYM indeksem czyta values[]. Koszt to petla po 7 krotkich
// napisach — mikrosekundy — a w zamian kod jest odporny na ciche przemieszanie
// kolumn, ktorego nikt by nie zauwazyl (dwie sasiednie wartosci PM tej samej stacji
// wygladaja podobnie, wiec zamiana GA17<->GA24 nie rzucalaby sie w oczy na ekranie).
constexpr const char* kVarPm10Main = "GA17.PM10_k:A1h";
constexpr const char* kVarPm25Main = "GA17.PM25_k:A1h";
constexpr const char* kVarTaMain = "GA17.TA:A1h";
constexpr const char* kVarRhMain = "GA17.RH:A1h";
constexpr const char* kVarPaMain = "GA17.PA:A1h";
constexpr const char* kVarPm10Fb = "GA24.PM10_k:A1h";
constexpr const char* kVarPm25Fb = "GA24.PM25_k:A1h";

int findVarIndex(JsonArrayConst vars, const char* name) {
  for (size_t i = 0; i < vars.size(); ++i) {
    const char* v = vars[i];
    if (v != nullptr && strcmp(v, name) == 0) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

// Ostatnia (najnowsza) probka danej zmiennej. false = zmiennej nie bylo w odpowiedzi
// ALBO stacja oddala dla niej pusta tablice — spec API mowi wprost, ze [] znaczy
// "brak danych z tej stacji", wiec obie sytuacje traktujemy tak samo: nie mamy nic.
bool lastSample(JsonArrayConst values, JsonArrayConst vars, const char* name, float& v,
                uint32_t& t) {
  const int idx = findVarIndex(vars, name);
  if (idx < 0) {
    return false;
  }
  JsonArrayConst series = values[idx];
  if (series.isNull() || series.size() == 0) {
    return false;
  }
  JsonObjectConst last = series[series.size() - 1];
  v = last["v"].as<float>();
  t = last["t"].as<uint32_t>();
  return true;
}

// --- indeks ARMAAG dla Trojmiasta ---------------------------------------------------
// Tabela progow PODANA PRZEZ WLASCICIELA jako oficjalne zrodlo Fundacji ARMAAG dla
// tego regionu (Trojmiasto) — INNA niz ogolnopolska tabela GIOS/CAPP, wiec nie
// "poprawiac" tych liczb na podstawie jakiejkolwiek innej tabeli znalezionej gdzie
// indziej. Gorna granica kazdego przedzialu jest WLACZNIE (np. "0-20" dla PM10 konczy
// sie na 20,0; "20,1" zaczyna juz nastepny), std <= przy kazdym progu ponizej.
int indexForPm10(float v) {
  if (v <= 20.f) return 1;
  if (v <= 50.f) return 2;
  if (v <= 80.f) return 3;
  if (v <= 110.f) return 4;
  if (v <= 150.f) return 5;
  return 6;
}

int indexForPm25(float v) {
  if (v <= 13.f) return 1;
  if (v <= 35.f) return 2;
  if (v <= 55.f) return 3;
  if (v <= 75.f) return 4;
  if (v <= 110.f) return 5;
  return 6;
}

}  // namespace

const char* airIndexName(int index) {
  switch (index) {
    case 1: return "BARDZO DOBRE";
    case 2: return "DOBRE";
    case 3: return "ZADOWALAJĄCE";
    case 4: return "DOSTATECZNE";
    case 5: return "ZŁE";
    case 6: return "BARDZO ZŁE";
    default: return "BRAK DANYCH";
  }
}

bool AirClient::parsePayload(const char* json, std::size_t len, AirModel& out) const {
  // Filtr ArduinoJson — przepuszczamy WYLACZNIE "vars" (siedem krotkich nazw, kilkadziesiat
  // bajtow) i, z KAZDEJ probki w KAZDEJ z siedmiu list, tylko pola "t" i "v". Odrzucamy:
  // "r" (wartosc bez korekty), "s"/"os" (status probki), "a"/"oa" (opis diagnostyczny,
  // bywa dlugi — patrz przykladowy fragment w opisie API), "ov"/"or" (druga sciezka
  // wartosci). To sa metadane serii (odchylenie, kompletnosc, min/max) — dla ekranu i
  // /api/diag liczy sie tylko OSTATNIA wartosc i jej czas. Zmierzone na przykladowej
  // odpowiedzi z siedmioma zmiennymi: filtr sprowadza kilka kB JSON-a do kilkuset bajtow
  // (jedna probka bez filtra to ~230 B, z filtrem ~25 B — prawie 10x mniej), a przy
  // budzecie SRAM 76000 B (patrz tools/release.sh) to jest realna oszczednosc, nie
  // kosmetyka. Ten sam wzorzec filtra co w WeatherClient.cpp i RadarMap.cpp: tam gniazdo
  // jest jednopoziomowe (tablica obiektow), tu DWUPOZIOMOWE (tablica list, kazda lista to
  // tablica obiektow) — ArduinoJson stosuje PIERWSZY element kazdego zagniezdzonego
  // filtra-tablicy jako wzorzec dla WSZYSTKICH elementow prawdziwej tablicy, wiec
  // ["values"][0][0]["t"/"v"] filtruje kazda probke kazdej z siedmiu list tym samym
  // wzorcem. Zweryfikowane osobno (natywny test z ArduinoJson, ten sam naglowek co tu)
  // na autentycznym ksztalcie odpowiedzi z zadania — filtr dziala tez na PUSTEJ liscie
  // ([] zostaje [] po filtrze, patrz lastSample() wyzej).
  JsonDocument filter;
  filter["values"][0][0]["t"] = true;
  filter["values"][0][0]["v"] = true;
  filter["vars"] = true;

  JsonDocument doc;
  const DeserializationError err =
      deserializeJson(doc, json, len, DeserializationOption::Filter(filter));
  if (err) {
    snprintf(out.errorMsg, sizeof(out.errorMsg), "JSON: %s", err.c_str());
    return false;
  }

  JsonArrayConst vars = doc["vars"];
  JsonArrayConst values = doc["values"];
  if (vars.isNull() || values.isNull()) {
    strncpy(out.errorMsg, "Zła odpowiedź API", sizeof(out.errorMsg) - 1);
    return false;
  }

  float pm10Main = 0.f, pm25Main = 0.f, taMain = 0.f, rhMain = 0.f, paMain = 0.f;
  float pm10Fb = 0.f, pm25Fb = 0.f;
  uint32_t tPm10Main = 0, tPm25Main = 0, tTaMain = 0, tRhMain = 0, tPaMain = 0;
  uint32_t tPm10Fb = 0, tPm25Fb = 0;
  const bool hasPm10Main = lastSample(values, vars, kVarPm10Main, pm10Main, tPm10Main);
  const bool hasPm25Main = lastSample(values, vars, kVarPm25Main, pm25Main, tPm25Main);
  const bool hasTaMain = lastSample(values, vars, kVarTaMain, taMain, tTaMain);
  const bool hasRhMain = lastSample(values, vars, kVarRhMain, rhMain, tRhMain);
  const bool hasPaMain = lastSample(values, vars, kVarPaMain, paMain, tPaMain);
  const bool hasPm10Fb = lastSample(values, vars, kVarPm10Fb, pm10Fb, tPm10Fb);
  const bool hasPm25Fb = lastSample(values, vars, kVarPm25Fb, pm25Fb, tPm25Fb);

  const time_t now = time(nullptr);   // fetch() juz sprawdzil > 1700000000
  auto freshEnough = [&](bool has, uint32_t t) {
    return has && now >= static_cast<time_t>(t) &&
           static_cast<uint32_t>(now - static_cast<time_t>(t)) <= AIR_STALE_S;
  };

  // GA17 uznajemy za "zywa", jesli ma CHOC JEDEN swiezy pomiar PM — nie wymagamy obu
  // naraz. Stacja z dzialajacym czujnikiem PM10, ale akurat martwym PM2,5 (albo
  // odwrotnie), to WCIAZ glowna stacja wlasciciela, nie powod do przeskoku na Halicka.
  // Przeskakujemy dopiero, gdy Sandomierska nie ma KOMPLETNIE NIC swiezego do pokazania.
  const bool mainAlive =
      freshEnough(hasPm10Main, tPm10Main) || freshEnough(hasPm25Main, tPm25Main);
  out.usingFallback = !mainAlive;

  if (!out.usingFallback) {
    strncpy(out.stationName, "SANDOMIERSKA", sizeof(out.stationName) - 1);
    out.hasPm10 = hasPm10Main;
    out.pm10 = pm10Main;
    out.hasPm25 = hasPm25Main;
    out.pm25 = pm25Main;

    // "Wiek danych" na ekranie ma pokazywac MLODSZA z dwoch probek — to ona
    // realistycznie opisuje "jak swieze jest to, na co patrzysz".
    uint32_t age = 0;
    bool haveAge = false;
    if (hasPm10Main) { age = tPm10Main; haveAge = true; }
    if (hasPm25Main && (!haveAge || tPm25Main > age)) { age = tPm25Main; haveAge = true; }
    out.sampleEpoch = haveAge ? age : 0;

    // Temperatura/wilgotnosc/cisnienie — TYLKO gdy naprawde pokazujemy GA17 (patrz
    // uzasadnienie hasWeather w AirData.h: przy fallbacku ekran mowi "Halicka" i te
    // liczby spod Sandomierskiej byłyby klamstwem pod cudzym szyldem).
    out.hasWeather = hasTaMain && hasRhMain && hasPaMain;
    if (out.hasWeather) {
      out.tempC = taMain;
      out.rh = rhMain;
      out.pressureHpa = paMain;
    }
  } else {
    strncpy(out.stationName, "HALICKA", sizeof(out.stationName) - 1);
    out.hasPm10 = hasPm10Fb;
    out.pm10 = pm10Fb;
    out.hasPm25 = hasPm25Fb;
    out.pm25 = pm25Fb;

    uint32_t age = 0;
    bool haveAge = false;
    if (hasPm10Fb) { age = tPm10Fb; haveAge = true; }
    if (hasPm25Fb && (!haveAge || tPm25Fb > age)) { age = tPm25Fb; haveAge = true; }
    out.sampleEpoch = haveAge ? age : 0;
    out.hasWeather = false;   // GA24 nie ma TA/RH/PA w naszej liscie `vars` w ogole
  }

  if (!out.hasPm10 && !out.hasPm25) {
    // Ani glowna, ani zapasowa stacja nie dala NIC uzytecznego. Ekran (WeatherUi::
    // drawViewAir) i rotacja (WeatherUi::render) maja to potraktowac jak brak danych
    // w ogole — dokladnie tak, jak RADAR bez opadu.
    strncpy(out.errorMsg,
            out.usingFallback ? "Brak danych: GA17 i GA24" : "Brak danych: GA17",
            sizeof(out.errorMsg) - 1);
    out.ready = false;
    return false;
  }

  // Indeksy CZASTKOWE — trzymane OSOBNO (nie tylko `index` ogolny), zeby ekran mogl
  // pokolorowac kazda karte (PM10/PM2,5) wg JEJ WLASNEJ klasy (patrz AirData.h).
  out.indexPm10 = out.hasPm10 ? indexForPm10(out.pm10) : 0;
  out.indexPm25 = out.hasPm25 ? indexForPm25(out.pm25) : 0;

  // Indeks OGOLNY to WIEKSZY z dwoch czastkowych — NIGDY srednia. Uzasadnienie:
  // gdyby usrednic, dzien z PM10 = 155 µg/m3 (indeks 6, BARDZO ZLE — realny smog) i
  // PM2,5 = 5 µg/m3 (indeks 1) dalby (6+1)/2 zaokraglone do 3-4, czyli "znosnie" —
  // dokladnie w dniu, w ktorym nie wolno wychodzic na dwor. Powietrze oddycha sie
  // tym SKLADNIKIEM, ktory akurat jest najgorszy, nie ich przecietna, wiec maksimum
  // jest tu jedyna uczciwa funkcja.
  // NIE "POPRAWIAC" TEGO NA SREDNIA — gdyby kiedys ARMAAG zmienil metodologie,
  // zmienic TABELE progow (indexForPm10/indexForPm25 wyzej), ale zasada maksimum ma
  // zostac; to jest cala pointa tych dwoch linii.
  out.index = out.indexPm10 > out.indexPm25 ? out.indexPm10 : out.indexPm25;

  out.ready = true;
  out.errorMsg[0] = '\0';
  return true;
}

bool AirClient::fetch(AirModel& out) {
  out.errorMsg[0] = '\0';

  // Dane to srednie GODZINOWE — bez wazytwego czasu z NTP zapytanie nie ma ZADNEGO
  // sensu: start/end liczone od time(nullptr) wyszlyby gdzies kolo 1970 roku, a serwer
  // dostalby zakres sprzed pol wieku. Lepiej nie pytac wcale, niz zapytac o zly zakres
  // i (uczciwie) dostac pustke.
  const time_t now = time(nullptr);
  if (now < 1700000000) {
    strncpy(out.errorMsg, "Brak czasu NTP", sizeof(out.errorMsg) - 1);
    return false;
  }

  // Zakres 2 h wstecz, nie 1 h: dane to srednie 1-godzinne i nowa probka pojawia sie
  // z pewnym poslizgiem po pelnej godzinie. Krotszy zakres (np. dokladnie 1 h) potrafi
  // nie zlapac ani jednej pelnej probki, gdy serwer jeszcze nie opublikowal biezacej —
  // dwie godziny dają zapas, a i tak bierzemy wylacznie OSTATNI element kazdej serii.
  const long end = static_cast<long>(now);
  const long start = end - 7200;

  char url[400];
  snprintf(url, sizeof(url),
           "http://%s%s?type=chart&avg=1h&start=%ld&end=%ld"
           "&vars=GA17.PM10_k:A1h,GA17.PM25_k:A1h,GA17.TA:A1h,GA17.RH:A1h,GA17.PA:A1h,"
           "GA24.PM10_k:A1h,GA24.PM25_k:A1h",
           kHost, kPath, start, end);

  // WiFiClient zwykly (bez TLS) — patrz uzasadnienie kHost/kPath wyzej.
  WiFiClient client;
  client.setTimeout(12);
  HTTPClient http;
  http.setTimeout(12000);
  http.setReuse(false);
  if (!http.begin(client, url)) {
    strncpy(out.errorMsg, "HTTP begin fail", sizeof(out.errorMsg) - 1);
    return false;
  }

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    snprintf(out.errorMsg, sizeof(out.errorMsg), "HTTP %d", code);
    http.end();
    return false;
  }

  const String payload = http.getString();
  http.end();

  if (payload.length() < 20) {
    strncpy(out.errorMsg, "Pusta odpowiedź", sizeof(out.errorMsg) - 1);
    return false;
  }

  return parsePayload(payload.c_str(), payload.length(), out);
}
