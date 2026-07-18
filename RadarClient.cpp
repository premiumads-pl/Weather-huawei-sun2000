#include "RadarClient.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <PNGdec.h>
#include <WiFiClient.h>
#include <cmath>
#include <cstring>
#include <new>

#include "Log.h"
#include "Settings.h"

namespace {

constexpr int kZoom = 7;          // maks. wspierany przez RainViewer (>=8 = placeholder)
constexpr int kTile = 256;
constexpr int kRadiusPx = 2;      // próbkujemy 5x5 px (~3,5 km) i bierzemy maksimum
constexpr size_t kMaxPng = 40000;

// Obiekt PNG waży ~47 kB (PNGdec trzyma 32 kB słownika inflate). Jako zmienna
// globalna zjadał tyle RAM-u NA STAŁE i pogoda nie miała już z czego sparsować
// JSON-a. Tworzymy go więc tylko na czas dekodowania i od razu zwalniamy.
constexpr uint32_t kMinHeapForRadar = 64000;
bool gPngInPsram = false;   // dekoder siedzi w PSRAM -> zwalniamy inaczej


PNG* png = nullptr;
uint8_t* gPng = nullptr;
size_t gPngLen = 0;

int gWantX = 0, gWantY = 0;
uint8_t gBestLevel = 0;

// Paleta RainViewer to gradient natężenia:
//   beż półprzezroczysty  -> mżawka / ślady
//   niebieski (im ciemniejszy, tym mocniej) -> deszcz
//   żółty -> pomarańczowy -> czerwony -> ulewa / burza
uint8_t levelFromRgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (a < 20) {
    return 0;
  }
  if (b < 40 && r > 230) {       // ciepłe kolory = mocny opad
    if (g < 100) return 5;       // czerwony
    if (g < 190) return 4;       // pomarańczowy
    return 3;                    // żółty
  }
  if (b > r) {                   // rodzina niebieska
    if (b >= 200) return 1;      // jasnoniebieski
    if (b >= 140) return 2;
    return 3;                    // granatowy = już porządnie leje
  }
  return (a >= 60) ? 1 : 0;      // beż = mżawka / ślady
}

int pngDraw(PNGDRAW* draw) {
  if (draw->y < gWantY - kRadiusPx || draw->y > gWantY + kRadiusPx) {
    return 1;
  }
  static uint16_t line[kTile];
  static uint8_t alpha[kTile];
  // MUSI byc LITTLE_ENDIAN. Przy PNG_RGB565_BIG_ENDIAN PNGdec robi na koniec
  // __builtin_bswap16() (png.inl; na S3 to samo w SIMD: s3_simd_rgb565.S), a my
  // czytamy line[x] jako uint16_t na procesorze little-endian — czyli dostawalismy
  // wartosc z ZAMIENIONYMI bajtami i rozbieralismy ja ponizej tak, jakby bajty byly
  // na miejscu. W bitach 11-15 nie siedzial wtedy czerwony, tylko trzy mlodsze bity
  // zielonego sklejone z dwoma starszymi niebieskiego.
  // Sprawdzone na palecie pobranej z realnych kafelkow: BIG dawal 2/8 kolorow
  // poprawnie, LITTLE daje 8/8. Czerwien ulewy (255,68,0) wychodzila jako poziom 1
  // ("mzawka") — poziom 5 byl nieosiagalny dla kazdego koloru z palety RainViewera.
  png->getLineAsRGB565(draw, line, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);
  // Prog 60, nie 1: getAlphaMask oddaje BIT (alpha >= prog), a nie wartosc, wiec
  // ponizsze "a" jest zawsze 0 albo 255. Przy progu 1 warunek (a >= 60) ? 1 : 0
  // w levelFromRgba nie mogl juz nigdy zwrocic 0 i rozroznienie "bez polprzezroczysty
  // = slad vs mzawka" nie istnialo — piksel o alfie 3 (artefakt na krawedzi echa)
  // szedl jako mzawka. Prog 60 przenosi te granice tam, gdzie ja opisuje paleta.
  // Sprawdzone na realnych kafelkach: alfe czesciowa ma WYLACZNIE bez; niebieski,
  // zolty, pomaranczowy i czerwony maja zawsze 255, wiec prog ich nie dotyka.
  png->getAlphaMask(draw, alpha, 60);

  for (int dx = -kRadiusPx; dx <= kRadiusPx; ++dx) {
    const int x = gWantX + dx;
    if (x < 0 || x >= kTile) continue;
    const uint16_t c = line[x];
    const uint8_t a = (alpha[x >> 3] & (0x80 >> (x & 7))) ? 255 : 0;
    const uint8_t r = ((c >> 11) & 0x1F) << 3;
    const uint8_t g = ((c >> 5) & 0x3F) << 2;
    const uint8_t b = (c & 0x1F) << 3;
    const uint8_t lv = levelFromRgba(r, g, b, a);
    if (lv > gBestLevel) {
      gBestLevel = lv;
    }
  }
  return 1;
}

// Zbiornik na kafelek. Do v51 bralismy http.getSize() + getStreamPtr()->readBytes()
// — a to jest dokladnie ta sama mina, ktora kosztowala nas miesiace przy Viessmannie:
//   1. getSize() zwraca -1 przy Transfer-Encoding: chunked -> "size <= 0" -> cichy
//      return false, zanim ktokolwiek dotknie strumienia;
//   2. getStreamPtr() oddaje SUROWY strumien, razem z naglowkami porcji
//      ("1f4a\r\n\x89PNG..."), wiec openRAM i tak odrzucilby to jako zly PNG.
// Dzialalo wylacznie dlatego, ze RainViewer wysyla dzis Content-Length. Zmiana CDN-a
// po ich stronie albo proxy po drodze i radar milknie, a komunikat wskazuje na siec.
// writeToStream() rozpakowuje porcje za nas i radzi sobie z obiema postaciami.
// writeToStream() chce Stream*, nie Print* — stad puste metody odczytu.
class PngSink : public Stream {
 public:
  PngSink() : psram_(psramFound()) {}
  ~PngSink() { free(buf_); }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* d, size_t n) override {
    if (len_ + n > cap_) {
      size_t want = cap_ ? cap_ * 2 : 8192;   // realny kafelek to ~0,5-5 kB
      while (want < len_ + n) want *= 2;
      if (want > kMaxPng) want = kMaxPng;
      if (len_ + n > want) return 0;   // twardy limit — zwrot 0 przerywa transfer
      uint8_t* p = static_cast<uint8_t*>(
          buf_ ? (psram_ ? ps_realloc(buf_, want) : realloc(buf_, want))
               : (psram_ ? ps_malloc(want) : malloc(want)));
      if (p == nullptr) return 0;
      buf_ = p;
      cap_ = want;
    }
    memcpy(buf_ + len_, d, n);
    len_ += n;
    return n;
  }
  // oddaje wlasnosc bufora wolajacemu (zwalnia go free())
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
  bool psram_ = false;
};

// RainViewer serwuje wszystko po zwyklym HTTP. Rezygnacja z TLS oszczedza ~40 kB
// sterty — dokladnie tyle, ile brakowalo dekoderowi PNG.
bool httpGet(const char* url, uint8_t** buf, size_t* len, String* text) {
  WiFiClient client;
  client.setTimeout(12);

  HTTPClient http;
  http.setTimeout(12000);
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, url)) {
    return false;
  }
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  if (text != nullptr) {
    *text = http.getString();
    http.end();
    return true;
  }

  // getSize() == -1 przy chunked; to nie jest blad, tylko brak Content-Length.
  // Sprawdzamy limit tylko wtedy, gdy serwer w ogole podal rozmiar — reszte
  // przypilnuje sam PngSink.
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

}  // namespace

const char* radarLabel(uint8_t level) {
  switch (level) {
    case 1: return "mżawka";
    case 2: return "deszcz";
    case 3: return "silny deszcz";
    case 4: return "bardzo silny";
    case 5: return "ULEWA";
    default: return "bez opadu";
  }
}

void releasePng() {
  if (png == nullptr) return;
  if (gPngInPsram) {
    png->~PNG();
    free(png);
  } else {
    delete png;
  }
  png = nullptr;
  gPngInPsram = false;
}

bool RadarClient::fetch(RadarSnapshot& out) {
  out.valid = false;
  out.errorMsg[0] = '\0';

  // Radar jest najbardziej pamięciożerny z całego firmware'u. Jeśli sterty jest
  // mało, odpuszczamy ten cykl — prognoza i PV są ważniejsze.
  const uint32_t heap = ESP.getFreeHeap();
  if (heap < kMinHeapForRadar) {
    ++diag().radarSkips;
    snprintf(out.errorMsg, sizeof(out.errorMsg), "Za mało RAM (%lu B)",
             static_cast<unsigned long>(heap));
    return false;
  }

  // --- 1. najnowsza klatka ---
  String body;
  if (!httpGet("http://api.rainviewer.com/public/weather-maps.json", nullptr, nullptr,
               &body)) {
    strncpy(out.errorMsg, "Radar niedostępny", sizeof(out.errorMsg) - 1);
    return false;
  }

  JsonDocument filter;
  filter["host"] = true;
  filter["radar"]["past"][0]["time"] = true;
  filter["radar"]["past"][0]["path"] = true;

  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
    strncpy(out.errorMsg, "Radar: zły JSON", sizeof(out.errorMsg) - 1);
    return false;
  }
  JsonArrayConst past = doc["radar"]["past"];
  if (past.isNull() || past.size() == 0) {
    strncpy(out.errorMsg, "Radar: brak klatek", sizeof(out.errorMsg) - 1);
    return false;
  }
  // API zwraca host ZE SCHEMATEM ("https://tilecache..."). Bez obciecia budowalismy
  // adres "http://https://..." i zaden kafelek nie dochodzil. Obcinamy tez "http://"
  // i znosimy brak schematu — RadarMap robi tak od dawna, tu zostal rozjazd dwoch
  // kopii tej samej logiki. Kafelek bierzemy po czystym HTTP: bez TLS starcza
  // RAM-u na dekoder.
  const char* host = doc["host"] | "https://tilecache.rainviewer.com";
  if (strncmp(host, "https://", 8) == 0) host += 8;
  else if (strncmp(host, "http://", 7) == 0) host += 7;
  JsonObjectConst last = past[past.size() - 1];
  const char* path = last["path"];
  const uint32_t frameTime = last["time"] | 0;
  if (path == nullptr) {
    strncpy(out.errorMsg, "Radar: brak ścieżki", sizeof(out.errorMsg) - 1);
    return false;
  }

  // --- 2. kafelek dla naszych współrzędnych ---
  const double lat = settings().lat;
  const double lon = settings().lon;
  const double n = static_cast<double>(1 << kZoom);
  const double fx = (lon + 180.0) / 360.0 * n;
  const double latRad = lat * M_PI / 180.0;
  const double fy = (1.0 - asinh(tan(latRad)) / M_PI) / 2.0 * n;

  const int tx = static_cast<int>(fx);
  const int ty = static_cast<int>(fy);
  gWantX = static_cast<int>((fx - tx) * kTile);
  gWantY = static_cast<int>((fy - ty) * kTile);
  gBestLevel = 0;

  char url[160];
  snprintf(url, sizeof(url), "http://%s%s/%d/%d/%d/%d/0/0_0.png", host, path, kTile, kZoom,
           tx, ty);

  // --- 3. dekoder NAJPIERW ---
  // PNGdec potrzebuje jednego spójnego bloku ~47 kB. Jeśli najpierw pobierzemy
  // kafelek, jego bufor rozbija stertę na kawałki i alokacja pada mimo "wolnych"
  // kilobajtów. Dlatego bierzemy duży blok, póki sterta jest jeszcze cała.
  //
  // Od czasu rysowania w dwóch pasach bufor ekranu ma 66 kB zamiast 132 kB, więc
  // ten blok się mieści i NIE prosimy już UI o oddanie bufora (ekran nie zamiera).
  // Gdyby jednak zabrakło — po prostu odpuszczamy cykl i mówimy o tym w diagnostyce.
  // Dekoder (46 kB) idzie do PSRAM. Do v50 walczyl o ciagly blok w SRAM-ie i
  // przegrywal — a przez caly ten czas obok lezalo 2 MB nietknietej pamieci.
  //
  // Mierzymy najwiekszy wolny blok OBU pamieci i robimy to PRZED alokacja (potem sa
  // o 46 kB mniejsze), bo dopiero po niej wiadomo, ktora z nich dala rade — a log ma
  // pokazac liczbe opisujaca TE pamiec, w ktorej dekoder faktycznie wylądował.
  //
  // Do v107 log podawal zawsze ESP.getMaxAllocHeap(), czyli SRAM, choc dekoder od v50
  // idzie przez ps_malloc() do PSRAM-u. Wychodzilo z tego "dekoder OK (blok 42996 B,
  // PNG=45604 B)": blok MNIEJSZY niz to, co sie w nim rzekomo zmiescilo. Wygladalo na
  // cudem unikniete niepowodzenie i kazalo szukac oszczednosci w SRAM-ie, a te dwie
  // liczby opisywaly po prostu dwie rozne pamieci i nie mialy ze soba nic wspolnego.
  const uint32_t largestSram = ESP.getMaxAllocHeap();
  const bool hasPsram = psramFound();
  const uint32_t largestPsram = hasPsram ? ESP.getMaxAllocPsram() : 0;

  void* mem = hasPsram ? ps_malloc(sizeof(PNG)) : nullptr;
  png = mem != nullptr ? new (mem) PNG() : new (std::nothrow) PNG();
  gPngInPsram = (mem != nullptr);
  if (png == nullptr) {
    // Tu zawiodly OBIE drogi (PSRAM i SRAM), wiec podajemy oba bloki — przy jednej
    // liczbie nie widac, czy zabraklo PSRAM-u, czy go w ogole nie bylo.
    snprintf(out.errorMsg, sizeof(out.errorMsg), "Blok SRAM %lukB/PSRAM %lukB < 47kB",
             static_cast<unsigned long>(largestSram / 1024),
             static_cast<unsigned long>(largestPsram / 1024));
    LOG("Radar: alokacja dekodera padla. heap=%lu, blok SRAM=%lu, PSRAM=%lu, PNG=%u B\n",
        static_cast<unsigned long>(heap), static_cast<unsigned long>(largestSram),
        static_cast<unsigned long>(largestPsram), static_cast<unsigned>(sizeof(PNG)));
    return false;
  }
  // Nazwa pamieci stoi PRZY liczbie, nie tylko przy dekoderze — inaczej znowu trzeba by
  // z pamieci wiedziec, ktorej sterty dotyczy "blok".
  const char* pool = gPngInPsram ? "PSRAM" : "SRAM";
  LOG("Radar: dekoder OK w %s (najw. wolny blok %s: %lu B, PNG=%u B)\n", pool, pool,
      static_cast<unsigned long>(gPngInPsram ? largestPsram : largestSram),
      static_cast<unsigned>(sizeof(PNG)));

  // --- 4. pobierz kafelek i zdekoduj ---
  if (!httpGet(url, &gPng, &gPngLen, nullptr)) {
    releasePng();
    strncpy(out.errorMsg, "Radar: brak kafelka", sizeof(out.errorMsg) - 1);
    return false;
  }

  if (png->openRAM(gPng, gPngLen, pngDraw) != PNG_SUCCESS) {
    releasePng();
    free(gPng);
    gPng = nullptr;
    strncpy(out.errorMsg, "Radar: zły PNG", sizeof(out.errorMsg) - 1);
    return false;
  }
  png->decode(nullptr, 0);
  png->close();
  releasePng();
  free(gPng);
  gPng = nullptr;

  out.level = gBestLevel;
  const time_t now = time(nullptr);
  out.ageSec = (now > 1700000000 && frameTime > 0 && now > static_cast<time_t>(frameTime))
                   ? static_cast<uint32_t>(now - frameTime)
                   : 0;
  out.valid = true;

  LOG("Radar: poziom %d (%s), klatka sprzed %lu s, heap %lu->%lu\n", out.level,
      radarLabel(out.level), static_cast<unsigned long>(out.ageSec),
      static_cast<unsigned long>(heap), static_cast<unsigned long>(ESP.getFreeHeap()));
  return true;
}
