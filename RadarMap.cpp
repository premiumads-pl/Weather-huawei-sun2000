#include "RadarMap.h"

#include "Log.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <PNGdec.h>
#include <WiFiClient.h>

#include <cmath>
#include <cstring>

namespace radarmap {
namespace {

// v110: 7 -> 6. Przy oknie gmapr (300 km) zoom 7 rozciagalby okno przez TRZY kafle
// w poziomie zamiast dwoch (sprawdzone numerycznie: gx 69.79-71.44 na z=7, kontra
// 34.89-35.72 na z=6) — mniej pobran, a rozdzielczosc kafla i tak jest grubsza od
// potrzebnej (radar ma piksele grubsze niz nasza mapa, patrz resample() nizej).
// Zoom > 7 nie dziala niezaleznie od tego: RainViewer zwraca "Zoom Level Not Supported".
constexpr int kZoom = 6;
constexpr int kTilePx = 256;
constexpr size_t kMaxPng = 90000;

// Bufory w PSRAM. 13 klatek x 320x172 B (55 040 B/klatka) = 715 520 B (~715 kB)
// — w SRAM nie do pomyslenia (budzet SRAM jest <76000 B na CALY program, patrz
// tools/release.sh). Wzgledem 7 klatek (do v109) to +330 kB PSRAM — nie liczy
// sie do bariery SRAM, bo ps_calloc() nizej nie rusza sterty wewnetrznej.
uint8_t* gFrames[FRAMES] = {};
uint8_t* gTile = nullptr;    // 256x256 poziomow, bufor roboczy jednego kafelka
Frame gMeta[FRAMES];
int gCount = 0;
uint32_t gUpdatedAt = 0;
// Komplet buforow stoi. begin() alokowal 7 klatek w petli i przy porazce wychodzil,
// zostawiajac czesc wskaznikow waznych, a reszte NULL — a setDemo() sprawdzal tylko
// gFrames[0] i pisal do wszystkich siedmiu, czyli pod adres 0. Jedna flaga zamiast
// siedmiu sprawdzen wskaznika.
bool gReady = false;
bool gDemo = false;
bool gRain = false;
bool gWantFetch = false;
char gErr[48] = "brak danych";
SemaphoreHandle_t gMx = nullptr;

// --- geometria: nasza mapa vs kafelki RainViewera (v110: MULTI-TILE) ---
// Do v109 cala mapa (111 km, gmapw) miescila sie w JEDNYM kaflu z=7 — gTileX/gTileY
// bylo para liczb, a "wycinek kafelka" (gPxX0..gPxY1) obszarem w pikselach TEGO
// jednego kafla. Przy 300 km (gmapr) okno rozciaga sie przez WIECEJ NIZ JEDEN kafel
// (patrz naglowek RadarMap.h) — gTileX0/X1/Y0/Y1 nizej sa wiec ZAKRESEM (WLACZNIE),
// a gGx0/gGx1/gGy0/gGy1 to polozenie krawedzi CALEGO okna w jednostkach kafla,
// liczone GLOBALNIE (nie w obrebie jednego kafla, jak dawne gPxX0..gPxY1) — to na
// nich resample() nizej sprawdza, do KTOREGO kafla nalezy dany piksel rastra.
int gTileX0 = 0, gTileX1 = 0;   // zakres kafli w poziomie (WLACZNIE)
int gTileY0 = 0, gTileY1 = 0;   // zakres kafli w pionie (WLACZNIE)
float gGx0 = 0, gGx1 = 0, gGy0 = 0, gGy1 = 0;  // krawedzie okna, jednostki kafla (globalne)

float mercY(float latDeg) {
  const float lat = latDeg * static_cast<float>(M_PI) / 180.f;
  return (1.f - logf(tanf(lat) + 1.f / cosf(lat)) / static_cast<float>(M_PI)) / 2.f;
}

void computeGeometry() {
  const float n = static_cast<float>(1 << kZoom);

  gGx0 = (gmapr::LON_MIN + 180.f) / 360.f * n;
  gGx1 = (gmapr::LON_MAX + 180.f) / 360.f * n;
  gGy0 = mercY(gmapr::LAT_MAX) * n;   // gora mapy = mniejszy y
  gGy1 = mercY(gmapr::LAT_MIN) * n;

  // Zakres kafli DYNAMICZNIE z powyzszych krawedzi — NIE na sztywno (dzis wychodzi
  // x=34..35, y=20..20, ale wpisanie tych liczb wprost przezyloby ciszej niz trzeba
  // kolejna zmiane granic gmapr albo kZoom). floor(), bo kafel k pokrywa jednostki
  // kafla w przedziale [k, k+1) — a wiec numer kafla to CALA czesc wspolrzednej.
  gTileX0 = static_cast<int>(floorf(gGx0));
  gTileX1 = static_cast<int>(floorf(gGx1));
  gTileY0 = static_cast<int>(floorf(gGy0));
  gTileY1 = static_cast<int>(floorf(gGy1));

  LOG("Radar mapa: kafle z=%d x=%d..%d y=%d..%d (%d szt.), okno merc %.3f-%.3f / %.3f-%.3f",
      kZoom, gTileX0, gTileX1, gTileY0, gTileY1,
      (gTileX1 - gTileX0 + 1) * (gTileY1 - gTileY0 + 1), gGx0, gGx1, gGy0, gGy1);
}

// Paleta RainViewera -> poziom 0..5 (ta sama logika co w RadarClient).
uint8_t levelFromRgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (a < 20) return 0;
  if (b < 40 && r > 230) {
    if (g < 100) return 5;
    if (g < 190) return 4;
    return 3;
  }
  if (b > r) {
    if (b >= 200) return 1;
    if (b >= 140) return 2;
    return 3;
  }
  return (a >= 60) ? 1 : 0;
}

// --- dekoder PNG: kafelek -> gTile (256x256 poziomow) ---
PNG* gPng = nullptr;

int pngLine(PNGDRAW* d) {
  static uint16_t rgb[kTilePx];
  static uint8_t alpha[kTilePx];

  // Szerokosc byla pilnowana (x < kTilePx), wysokosc nie — a gTile ma 256 wierszy.
  // Adres prosi o /256/, wiec dzis przychodzi 256x256; to jest jednak zaufanie do
  // zdalnego serwera, nie kontrakt lokalny. Wyzszy kafelek pisalby poza bufor
  // w PSRAM, czyli cicha korupcja sterty. (RadarClient sprawdza draw->y od dawna.)
  if (d->y < 0 || d->y >= kTilePx) return 1;

  // MUSI byc LITTLE_ENDIAN — patrz obszerny komentarz w RadarClient.cpp. Skrotowo:
  // przy BIG_ENDIAN PNGdec robi __builtin_bswap16() przed zapisem do tablicy, a my
  // czytamy ja jako uint16_t na little-endian i rozbieramy tak, jakby bajty byly na
  // miejscu. Kanaly wychodzily przemieszane i poziom 5 (ULEWA) byl nieosiagalny dla
  // kazdego koloru z palety RainViewera — ekran radaru pokazywal front burzowy
  // w jednym kolorze zamiast gradientu.
  gPng->getLineAsRGB565(d, rgb, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);
  // Prog 60, nie 1: maska oddaje bit (alpha >= prog), wiec przy progu 1 kazdy piksel
  // o alfie >= 1 szedl jako w pelni nieprzezroczysty. Artefakty na krawedzi echa
  // (alpha rzedu kilku) wpadaly wtedy do "wet" i mogly wlaczyc ekran radaru przy
  // suchym niebie — czyli dokladnie to, przed czym broni komentarz przy gRain nizej.
  gPng->getAlphaMask(d, alpha, 60);

  uint8_t* row = gTile + d->y * kTilePx;
  for (int x = 0; x < d->iWidth && x < kTilePx; ++x) {
    const uint16_t c = rgb[x];
    const uint8_t r = ((c >> 11) & 0x1F) << 3;
    const uint8_t g = ((c >> 5) & 0x3F) << 2;
    const uint8_t b = (c & 0x1F) << 3;
    // getAlphaMask pakuje 8 pikseli na bajt (1 = nieprzezroczysty)
    const bool opaque = (alpha[x >> 3] >> (7 - (x & 7))) & 1;
    row[x] = levelFromRgba(r, g, b, opaque ? 255 : 0);
  }
  return 1;
}

// Zbiornik na kafelek — patrz PngSink w RadarClient.cpp. Do v51 bylo tu
// http.getSize() + getStreamPtr()->readBytes(): getSize() zwraca -1 przy
// Transfer-Encoding: chunked (wtedy "size <= 0" i cichy return false), a surowy
// strumien niesie naglowki porcji. Dzialalo tylko dlatego, ze RainViewer wysyla
// dzis Content-Length. writeToStream() rozpakowuje porcje i znosi oba warianty.
// writeToStream() chce Stream*, nie Print* — stad puste metody odczytu.
class PngSink : public Stream {
 public:
  ~PngSink() { free(buf_); }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* d, size_t n) override {
    if (len_ + n > cap_) {
      size_t want = cap_ ? cap_ * 2 : 8192;
      while (want < len_ + n) want *= 2;
      if (want > kMaxPng) want = kMaxPng;
      if (len_ + n > want) return 0;   // twardy limit — zwrot 0 przerywa transfer
      uint8_t* p = static_cast<uint8_t*>(buf_ ? ps_realloc(buf_, want)
                                             : ps_malloc(want));   // PSRAM
      if (p == nullptr) return 0;
      buf_ = p;
      cap_ = want;
    }
    memcpy(buf_ + len_, d, n);
    len_ += n;
    return n;
  }
  uint8_t* take() {
    uint8_t* p = buf_;
    buf_ = nullptr;
    cap_ = len_ = 0;
    return p;
  }
  size_t size() const { return len_; }

 private:
  uint8_t* buf_ = nullptr;
  size_t cap_ = 0, len_ = 0;
};

bool httpGet(const char* url, uint8_t** buf, size_t* len, String* text) {
  WiFiClient client;
  client.setTimeout(12);

  HTTPClient http;
  http.setTimeout(12000);
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, url)) return false;

  if (http.GET() != HTTP_CODE_OK) {
    http.end();
    return false;
  }
  if (text != nullptr) {
    *text = http.getString();
    http.end();
    return true;
  }

  // getSize() == -1 przy chunked — to nie blad, tylko brak Content-Length.
  // Limit sprawdzamy, gdy serwer podal rozmiar; reszte przypilnuje PngSink.
  const int size = http.getSize();
  if (size > 0 && static_cast<size_t>(size) > kMaxPng) {
    http.end();
    return false;
  }

  PngSink sink;
  const int written = http.writeToStream(&sink);
  http.end();
  if (written <= 0 || sink.size() == 0) {
    return false;
  }
  *len = sink.size();
  *buf = sink.take();
  return *buf != nullptr;
}

// JEDEN kafelek (tileX, tileY) -> WLASNY WYCINEK klatki mapy (przeskalowanie
// najblizszym sasiadem; radar i tak ma piksele grubsze niz nasza mapa, wiec nie ma
// czego wygladzac). Wolane RAZ NA KAFEL — 2x na klatke przy dzisiejszych granicach
// gmapr (patrz fetch() nizej) — i kazde wywolanie dotyka WYLACZNIE pikseli rastra,
// ktorych globalne polozenie wpada w [tileX,tileX+1) x [tileY,tileY+1), czyli w TEN
// kafel; reszty rastra (nalezacej do INNEGO kafla) NIE RUSZA — pisze ja OSOBNE
// wywolanie resample() dla sasiedniego kafla. Bo [tileX0..tileX1] x [tileY0..tileY1]
// z computeGeometry() pokrywa z definicji CALE okno, suma wywolan dla wszystkich
// kafli jednej klatki zapisuje kazdy piksel DOKLADNIE raz — bez dziur i bez
// nadpisan (przy zalozeniu, ze wszystkie kafle klatki dojda; jesli nie, invalidate()
// w fetch() zeruje CALA klatke zamiast zostawiac pol-obrazu, patrz komentarz tam).
//
// X i Y NIE sa traktowane tak samo (patrz tools/gen_map.py, sekcja PROJEKCJA):
//   - X: Web Mercator jest DOKLADNIE liniowy wzgledem dlugosci geogr., wiec
//     interpolacja liniowa miedzy gGx0/gGx1 to dokladny wzor, nie przyblizenie.
//   - Y: Mercator jest NIELINIOWY wzgledem szerokosci geogr. gGy0/gGy1 to mercY()
//     policzone NA KRAWEDZIACH calego okna (raz, w computeGeometry) — miedzy nimi
//     interpolujemy LINIOWO po numerze wiersza, czyli udajemy, ze krzywa Mercatora
//     na tym odcinku jest prosta. Blad tego uproszczenia dla wysokosci gmapr
//     (161 km) to maks. ok. 0,76 px (policzone numerycznie, patrz tools/gen_map.py)
//     — ponizej 1 piksela, wiec bezpieczne; ZACHOWANE bez zmian z wersji
//     jednokaflowej (ta metoda dzialala juz przed v110, multi-tile jej nie dotyczy).
void resample(uint8_t* dst, int tileX, int tileY) {
  for (int my = 0; my < H; ++my) {
    // Globalna pozycja Y tego wiersza rastra, w jednostkach kafla.
    const float gy = gGy0 + (gGy1 - gGy0) * (my + 0.5f) / H;
    if (static_cast<int>(floorf(gy)) != tileY) continue;   // wiersz spoza TEGO kafla

    const float ty = (gy - tileY) * kTilePx;
    const int tyi = static_cast<int>(ty);
    // Poza [0,256) tylko przy skrajnym zaokragleniu float NA SAMEJ granicy kafla
    // (matematycznie gy-tileY in [0,1) z definicji floor() wyzej, wiec ty in [0,256)
    // powinno zachodzic zawsze) — zerujemy zamiast zostawiac stare dane sprzed
    // poprzedniego cyklu, to samo zabezpieczenie co w wersji jednokaflowej.
    const uint8_t* src = (tyi >= 0 && tyi < kTilePx) ? gTile + tyi * kTilePx : nullptr;

    uint8_t* row = dst + my * W;
    for (int mx = 0; mx < W; ++mx) {
      // Globalna pozycja X tej kolumny, w jednostkach kafla — liniowo (patrz wyzej).
      const float gx = gGx0 + (gGx1 - gGx0) * (mx + 0.5f) / W;
      if (static_cast<int>(floorf(gx)) != tileX) continue;   // kolumna spoza TEGO kafla

      if (src == nullptr) {
        row[mx] = 0;
        continue;
      }
      const float tx = (gx - tileX) * kTilePx;
      const int txi = static_cast<int>(tx);
      row[mx] = (txi >= 0 && txi < kTilePx) ? src[txi] : 0;
    }
  }
}

// Porazka alokacji zostawia jednoznaczny stan: albo stoi komplet, albo nic.
void releaseAll() {
  for (int i = 0; i < FRAMES; ++i) {
    free(gFrames[i]);
    gFrames[i] = nullptr;
  }
  free(gTile);
  gTile = nullptr;
}

// Klatka, ktora nie doszla albo sie nie zdekodowala, NIE moze zostac z poprzedniego
// cyklu. gCount i tak melduje komplet, a rysowanie animuje wszystkie klatki
// 0..count-1, wiec stary obrazek pojawilby sie w srodku animacji jako pomiar
// z innej chwili — front "przeskakuje" albo cofa sie (to samo, co opisuje komentarz
// przy setDemo). Zerujemy piksele: brak echa to uczciwe "nie mam danych", stary
// obrazek to falszywy pomiar. Czas slotu znamy z JSON-a, wiec podpis zostaje prawdziwy.
void invalidate(int i, uint32_t epoch, int32_t offsetMin) {
  xSemaphoreTake(gMx, portMAX_DELAY);
  if (gFrames[i] != nullptr) memset(gFrames[i], 0, W * H);
  gMeta[i].epoch = epoch;
  gMeta[i].offsetMin = offsetMin;
  gMeta[i].valid = false;
  xSemaphoreGive(gMx);
}

}  // namespace

bool begin() {
  if (gMx == nullptr) gMx = xSemaphoreCreateMutex();
  gReady = false;
  if (!psramFound()) {
    snprintf(gErr, sizeof(gErr), "brak PSRAM");
    return false;
  }

  for (int i = 0; i < FRAMES; ++i) {
    gFrames[i] = static_cast<uint8_t*>(ps_calloc(W * H, 1));
    if (gFrames[i] == nullptr) {
      releaseAll();
      snprintf(gErr, sizeof(gErr), "brak PSRAM na klatki");
      return false;
    }
  }
  gTile = static_cast<uint8_t*>(ps_malloc(kTilePx * kTilePx));
  if (gTile == nullptr) {
    releaseAll();
    snprintf(gErr, sizeof(gErr), "brak PSRAM na kafelek");
    return false;
  }

  computeGeometry();
  gReady = true;
  LOG("Radar mapa: %d klatek x %d B w PSRAM", FRAMES, W * H);
  return true;
}

bool wantsFetch() {
  return gWantFetch;
}

bool fetch() {
  if (!gReady || gDemo) return false;   // w symulacji nie nadpisujemy klatek
  gWantFetch = false;

  String js;
  if (!httpGet("http://api.rainviewer.com/public/weather-maps.json", nullptr, nullptr, &js)) {
    snprintf(gErr, sizeof(gErr), "brak listy klatek");
    return false;
  }

  JsonDocument filter;
  filter["host"] = true;
  filter["radar"]["past"][0]["time"] = true;
  filter["radar"]["past"][0]["path"] = true;

  JsonDocument doc;
  if (deserializeJson(doc, js, DeserializationOption::Filter(filter)) !=
      DeserializationError::Ok) {
    snprintf(gErr, sizeof(gErr), "zly JSON");
    return false;
  }

  // API zwraca host ZE SCHEMATEM ("https://tilecache..."). Bez obciecia budowalismy
  // adres "http://https://..." i zadna klatka nie dochodzila. Kafelki bierzemy po
  // czystym HTTP — TLS nie jest tu potrzebny, a oszczedza sterte.
  const char* host = doc["host"] | "https://tilecache.rainviewer.com";
  if (strncmp(host, "https://", 8) == 0) host += 8;
  else if (strncmp(host, "http://", 7) == 0) host += 7;
  JsonArrayConst past = doc["radar"]["past"];
  const int n = past.size();
  // Do v109 bralismy co DRUGA klatke (7 z 13, co 20 min), wiec i=0 siegalo indeksu
  // n-13 i sama liczba FRAMES nie wystarczala — trzeba bylo 2*FRAMES-1. Teraz
  // FRAMES=13 i bierzemy KAZDA klatke, wiec i=0 siega n-FRAMES: te dwie liczby sa
  // tym samym, kNeed = FRAMES bez mnoznika.
  // Straznik sam w sobie zostaje z tego samego powodu co zawsze: RainViewer po
  // awarii lub restarcie odbudowuje historie i przez kilkanascie minut oddaje
  // mniej niz 13 klatek. Bez tego warunku ujemne idx po prostu nie ruszylyby
  // starych danych w gFrames, a gCount = FRAMES i return true zglaszalyby komplet,
  // mimo ze czesc klatek zostalaby sprzed poprzedniego cyklu.
  constexpr int kNeed = FRAMES;
  if (n < kNeed) {
    snprintf(gErr, sizeof(gErr), "tylko %d z %d klatek", n, kNeed);
    return false;
  }

  // Bierzemy KAZDA klatke z konca: -120, -110, ..., 0 minut (RainViewer i tak
  // oddaje co 10 min — nie ma juz powodu co druga pomijac, patrz naglowek pliku).
  const time_t nowT = time(nullptr);
  int ok = 0;

  for (int i = 0; i < FRAMES; ++i) {
    const int idx = n - 1 - (FRAMES - 1 - i);   // po strazniku zawsze >= 0

    JsonObjectConst f = past[idx];
    const char* path = f["path"];
    const uint32_t t = f["time"] | 0U;
    const int32_t off = (nowT > 1700000000 && t > 0)
                            ? (static_cast<int32_t>(t) - static_cast<int32_t>(nowT)) / 60
                            : 0;
    if (path == nullptr || t == 0) {
      invalidate(i, 0, 0);
      continue;
    }

    // v110: KAZDA klatka to teraz TYLE pobran, ile kafli pokrywa okno gmapr (2 przy
    // dzisiejszych granicach — patrz gTileX0/X1/Y0/Y1 z computeGeometry). Klatka
    // liczy sie jako udana TYLKO gdy WSZYSTKIE jej kafle doszly i zdekodowaly sie:
    // pol mapy z tej klatki i pol z poprzedniego cyklu (sprzed 10 min) to gorsze
    // klamstwo niz uczciwy brak danych (patrz komentarz przy invalidate() wyzej),
    // wiec pojedynczy nieudany kafel kasuje CALA klatke, nie tylko swoj wycinek —
    // stad frameOk zamiast pojedynczego "continue" per-kafel.
    bool frameOk = true;
    for (int ty = gTileY0; ty <= gTileY1 && frameOk; ++ty) {
      for (int tx = gTileX0; tx <= gTileX1 && frameOk; ++tx) {
        char url[160];
        snprintf(url, sizeof(url), "http://%s%s/%d/%d/%d/%d/0/0_0.png", host, path, kTilePx,
                 kZoom, tx, ty);

        uint8_t* png = nullptr;
        size_t len = 0;
        if (!httpGet(url, &png, &len, nullptr)) {
          LOG("Radar mapa: klatka %d, kafel x=%d y=%d nie doszedl", i, tx, ty);
          frameOk = false;
          break;
        }

        // Dekoder (46 kB) w PSRAM — w SRAM nie ma tak duzego ciaglego bloku.
        void* mem = ps_malloc(sizeof(PNG));
        gPng = mem != nullptr ? new (mem) PNG() : nullptr;
        if (gPng == nullptr) {
          free(png);
          snprintf(gErr, sizeof(gErr), "brak pamieci na dekoder");
          return false;   // awaria pamieci jest globalna — nie ma sensu ciagnac dalej
        }

        memset(gTile, 0, kTilePx * kTilePx);
        bool decoded = false;
        if (gPng->openRAM(png, len, pngLine) == PNG_SUCCESS) {
          decoded = gPng->decode(nullptr, 0) == PNG_SUCCESS;
          gPng->close();
        }
        gPng->~PNG();
        free(gPng);
        gPng = nullptr;
        free(png);

        if (!decoded) {
          LOG("Radar mapa: klatka %d, kafel x=%d y=%d nie zdekodowany", i, tx, ty);
          frameOk = false;
          break;
        }

        xSemaphoreTake(gMx, portMAX_DELAY);
        resample(gFrames[i], tx, ty);
        xSemaphoreGive(gMx);

        // Pobran jest teraz 13x(liczba kafli) zamiast 13 — oddajemy procesor PO
        // KAZDYM kaflu, nie tylko po kazdej klatce (inaczej netTask trzymalby
        // radio/CPU dwa razy dluzej niz do v109 bez ani jednej okazji na przelaczenie
        // watkow; sam fetch() trwa przez to ~2x dluzej, ale to raz na 10 minut).
        vTaskDelay(pdMS_TO_TICKS(20));
      }
    }

    if (!frameOk) {
      invalidate(i, t, off);
      continue;
    }

    xSemaphoreTake(gMx, portMAX_DELAY);
    gMeta[i].epoch = t;
    gMeta[i].offsetMin = off;
    gMeta[i].valid = true;
    xSemaphoreGive(gMx);
    ++ok;
  }

  if (ok == 0) {
    snprintf(gErr, sizeof(gErr), "zadna klatka nie doszla");
    return false;
  }

  // Czy w ogole cokolwiek pada? Ekran bez opadu to pusta mapa — nie ma go po co
  // pokazywac. Liczymy piksele z opadem, a nie sam fakt "niezerowy" — pojedyncze
  // artefakty na krawedzi kafelka nie moga wlaczac ekranu.
  int wet = 0;
  for (int i = 0; i < FRAMES; ++i) {
    if (!gMeta[i].valid) continue;
    for (int k = 0; k < W * H; k += 7) {   // co 7. piksel wystarczy
      if (gFrames[i][k] > 0) ++wet;
    }
  }
  gRain = wet > 60;   // ~420 pikseli opadu w calej animacji

  gCount = FRAMES;
  gUpdatedAt = millis();
  gErr[0] = '\0';
  LOG("Radar mapa: %d/%d klatek, ostatnia %+ld min, opad: %s (%d)", ok, FRAMES,
      static_cast<long>(gMeta[FRAMES - 1].offsetMin), gRain ? "JEST" : "brak", wet);
  return true;
}

int count() {
  return gCount;
}

const Frame& frame(int i) {
  static Frame empty{};
  if (i < 0 || i >= FRAMES) return empty;
  return gMeta[i];
}

uint8_t levelAt(int i, int x, int y) {
  if (i < 0 || i >= FRAMES || gFrames[i] == nullptr) return 0;
  if (x < 0 || x >= W || y < 0 || y >= H) return 0;
  return gFrames[i][y * W + x];
}

uint32_t updatedAt() {
  return gUpdatedAt;
}

const char* lastError() {
  return gErr;
}

bool hasRain() {
  return gRain;
}

bool demo() {
  return gDemo;
}

// Sztuczny front: pas deszczu przesuwajacy sie z zachodu na wschod, z jadrem
// ulewy w srodku. Kazda klatka to inne polozenie — dokladnie tak, jak wygladalby
// prawdziwy front. Sluzy do obejrzenia wizualizacji, gdy nie pada.
void setDemo(bool on) {
  // Bez kompletu buforow nie ma czego wypelnic. Sprawdzenie samego gFrames[0] nie
  // wystarczalo: petla nizej pisze do wszystkich FRAMES klatek, a begin() mogl
  // zostawic czesc z nich pod NULL-em (patrz gReady).
  if (on && !gReady) return;
  gDemo = on;
  if (!on) {
    gCount = 0;
    gRain = false;
    gUpdatedAt = 0;
    gWantFetch = true;   // wracamy na prawdziwe dane — i to od razu
    return;
  }

  const time_t nowT = time(nullptr);
  xSemaphoreTake(gMx, portMAX_DELAY);

  // KLUCZOWE: front nie moze przeskakiwac o wiecej niz ~polowe wlasnej szerokosci
  // na klatke. Pierwsza wersja robila 73 px skoku przy pasie 124 px szerokim —
  // oko dopasowywalo wtedy lewa krawedz nowej klatki do prawej krawedzi starej
  // i widzialo ruch DO TYLU (efekt kola wozu). Animacja szla do przodu, ale
  // wygladala na cofajaca sie.
  // Krok to 155 px / (FRAMES-1), wiec skaluje sie SAM wraz z FRAMES: przy 7
  // klatkach (do v109) wychodzilo 25,8 px skoku przy pasie ~92 px, przy 13
  // (co 10 min zamiast co 20) wychodzi 12,9 px — margines do polowy pasa tylko
  // urosl, wiec przejscie 7->13 nie wymagalo tu zadnej zmiany wzoru, tylko
  // uaktualnienia liczb w tym komentarzu.
  for (int i = 0; i < FRAMES; ++i) {
    const float cx = 30.f + 155.f * (i / static_cast<float>(FRAMES - 1));

    for (int y = 0; y < H; ++y) {
      for (int x = 0; x < W; ++x) {
        // ukosny front (jak na Baltyku) + falowanie krawedzi
        const float fx = x + (y - H / 2) * 0.35f;
        const float d = fabsf(fx - cx - sinf(y * 0.06f + i) * 10.f);

        uint8_t lv = 0;
        if (d < 10.f) lv = 5;
        else if (d < 18.f) lv = 4;
        else if (d < 26.f) lv = 3;
        else if (d < 36.f) lv = 2;
        else if (d < 46.f) lv = 1;

        // dziury w opadzie — inaczej wyglada jak pomalowany pas
        if (lv > 1 && ((x * 7 + y * 13 + i * 31) % 11) == 0) --lv;
        gFrames[i][y * W + x] = lv;
      }
    }
    // 10, nie 20: symulacja ma imitowac PRAWDZIWY cykl (13 klatek co 10 min = te
    // same 2 h wstecz co fetch() wyzej). "20" tutaj dawaloby metadane klamiace
    // 4 h wstecz (12*20=240 min), podczas gdy narysowany front pokrywa tylko 2 h
    // (cx idzie po tym samym zakresie 155 px co przed 7->13) — os czasu pod mapa
    // (WeatherUi::drawViewRadar) pokazywalaby wtedy zle godziny przy kazdej klatce.
    gMeta[i].epoch = nowT > 1700000000 ? nowT - (FRAMES - 1 - i) * 10 * 60 : 0;
    gMeta[i].offsetMin = -(FRAMES - 1 - i) * 10;
    gMeta[i].valid = true;
  }
  xSemaphoreGive(gMx);

  gCount = FRAMES;
  gRain = true;
  gUpdatedAt = millis();
  LOG("Radar mapa: SYMULACJA wlaczona (sztuczny front W->E)");
}

}  // namespace radarmap
