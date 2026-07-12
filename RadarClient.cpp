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

PNG* png = nullptr;
volatile bool gWantMem = false;
volatile bool gMemReady = false;
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
  png->getLineAsRGB565(draw, line, PNG_RGB565_BIG_ENDIAN, 0xffffffff);
  png->getAlphaMask(draw, alpha, 1);

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

  const int size = http.getSize();
  if (size <= 0 || static_cast<size_t>(size) > kMaxPng) {
    http.end();
    return false;
  }
  *buf = static_cast<uint8_t*>(malloc(size));
  if (*buf == nullptr) {
    http.end();
    return false;
  }
  Stream* s = http.getStreamPtr();
  const int got = s->readBytes(*buf, size);
  http.end();
  if (got != size) {
    free(*buf);
    *buf = nullptr;
    return false;
  }
  *len = size;
  return true;
}

}  // namespace

bool radarNeedsMemory() {
  return gWantMem && !gMemReady;
}

void radarMemoryReady() {
  gMemReady = true;
}

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
  const char* host = doc["host"] | "https://tilecache.rainviewer.com";
  if (strncmp(host, "https://", 8) == 0) {
    host += 8;   // pobieramy kafelek po HTTP — bez TLS starcza RAM-u na dekoder
  }
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
  // kafelek, jego bufor rozbija stertę na kawałki i alokacja pada mimo 76 kB
  // "wolnych". Dlatego bierzemy duży blok, póki sterta jest jeszcze cała.
  // poproś UI o oddanie bufora ekranu
  gWantMem = true;
  gMemReady = false;
  for (int i = 0; i < 60 && !gMemReady; ++i) {
    delay(50);
  }

  const uint32_t largest = ESP.getMaxAllocHeap();
  png = new (std::nothrow) PNG();
  if (png == nullptr) {
    snprintf(out.errorMsg, sizeof(out.errorMsg), "Blok %lukB < 47kB",
             static_cast<unsigned long>(largest / 1024));
    LOG("Radar: alokacja dekodera padla. heap=%lu, blok=%lu, PNG=%u B\n",
        static_cast<unsigned long>(heap), static_cast<unsigned long>(largest),
        static_cast<unsigned>(sizeof(PNG)));
    gWantMem = false;
    return false;
  }
  LOG("Radar: dekoder OK (blok %lu B, PNG=%u B)\n", static_cast<unsigned long>(largest),
      static_cast<unsigned>(sizeof(PNG)));

  // --- 4. pobierz kafelek i zdekoduj ---
  if (!httpGet(url, &gPng, &gPngLen, nullptr)) {
    delete png;
    png = nullptr;
    gWantMem = false;
    strncpy(out.errorMsg, "Radar: brak kafelka", sizeof(out.errorMsg) - 1);
    return false;
  }

  if (png->openRAM(gPng, gPngLen, pngDraw) != PNG_SUCCESS) {
    delete png;
    png = nullptr;
    free(gPng);
    gPng = nullptr;
    gWantMem = false;
    strncpy(out.errorMsg, "Radar: zły PNG", sizeof(out.errorMsg) - 1);
    return false;
  }
  png->decode(nullptr, 0);
  png->close();
  delete png;
  png = nullptr;
  free(gPng);
  gPng = nullptr;
  gWantMem = false;   // UI może odtworzyć bufor

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
