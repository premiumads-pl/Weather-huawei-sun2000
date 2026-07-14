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

constexpr int kZoom = 7;      // wyzej RainViewer zwraca "Zoom Level Not Supported"
constexpr int kTilePx = 256;
constexpr size_t kMaxPng = 90000;

// Bufory w PSRAM. 7 klatek po 224x172 B = 270 kB — w SRAM nie do pomyslenia.
uint8_t* gFrames[FRAMES] = {};
uint8_t* gTile = nullptr;    // 256x256 poziomow, bufor roboczy jednego kafelka
Frame gMeta[FRAMES];
int gCount = 0;
uint32_t gUpdatedAt = 0;
bool gDemo = false;
bool gRain = false;
char gErr[48] = "brak danych";
SemaphoreHandle_t gMx = nullptr;

// --- geometria: nasza mapa vs kafelek RainViewera ---
// Wyliczone raz przy starcie; caly obszar (18.0-19.2 E, 54.30-54.84 N) miesci sie
// w jednym kafelku z=7.
int gTileX = 0, gTileY = 0;
float gPxX0 = 0, gPxX1 = 0, gPxY0 = 0, gPxY1 = 0;  // wycinek kafelka w pikselach

float mercY(float latDeg) {
  const float lat = latDeg * static_cast<float>(M_PI) / 180.f;
  return (1.f - logf(tanf(lat) + 1.f / cosf(lat)) / static_cast<float>(M_PI)) / 2.f;
}

void computeGeometry() {
  const float n = static_cast<float>(1 << kZoom);

  const float gx0 = (gmapw::LON_MIN + 180.f) / 360.f * n;
  const float gx1 = (gmapw::LON_MAX + 180.f) / 360.f * n;
  const float gy0 = mercY(gmapw::LAT_MAX) * n;   // gora mapy = mniejszy y
  const float gy1 = mercY(gmapw::LAT_MIN) * n;

  gTileX = static_cast<int>(gx0);
  gTileY = static_cast<int>(gy0);

  gPxX0 = (gx0 - gTileX) * kTilePx;
  gPxX1 = (gx1 - gTileX) * kTilePx;
  gPxY0 = (gy0 - gTileY) * kTilePx;
  gPxY1 = (gy1 - gTileY) * kTilePx;

  LOG("Radar mapa: kafelek z=%d x=%d y=%d, wycinek %.0f-%.0f / %.0f-%.0f px", kZoom,
      gTileX, gTileY, gPxX0, gPxX1, gPxY0, gPxY1);
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

  gPng->getLineAsRGB565(d, rgb, PNG_RGB565_BIG_ENDIAN, 0xffffffff);
  gPng->getAlphaMask(d, alpha, 1);

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

  const int size = http.getSize();
  if (size <= 0 || static_cast<size_t>(size) > kMaxPng) {
    http.end();
    return false;
  }
  *buf = static_cast<uint8_t*>(ps_malloc(size));   // PSRAM — nie ruszamy SRAM-u
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

// Kafelek -> klatka mapy (przeskalowanie najblizszym sasiadem; radar i tak ma
// grubsze piksele niz nasza mapa, wiec nie ma czego wygladzac).
void resample(uint8_t* dst) {
  for (int my = 0; my < H; ++my) {
    const float ty = gPxY0 + (gPxY1 - gPxY0) * (my + 0.5f) / H;
    const int tyi = static_cast<int>(ty);
    if (tyi < 0 || tyi >= kTilePx) {
      memset(dst + my * W, 0, W);
      continue;
    }
    const uint8_t* src = gTile + tyi * kTilePx;
    for (int mx = 0; mx < W; ++mx) {
      const float tx = gPxX0 + (gPxX1 - gPxX0) * (mx + 0.5f) / W;
      const int txi = static_cast<int>(tx);
      dst[my * W + mx] = (txi >= 0 && txi < kTilePx) ? src[txi] : 0;
    }
  }
}

}  // namespace

bool begin() {
  if (gMx == nullptr) gMx = xSemaphoreCreateMutex();
  if (!psramFound()) {
    snprintf(gErr, sizeof(gErr), "brak PSRAM");
    return false;
  }

  for (int i = 0; i < FRAMES; ++i) {
    gFrames[i] = static_cast<uint8_t*>(ps_calloc(W * H, 1));
    if (gFrames[i] == nullptr) {
      snprintf(gErr, sizeof(gErr), "brak PSRAM na klatki");
      return false;
    }
  }
  gTile = static_cast<uint8_t*>(ps_malloc(kTilePx * kTilePx));
  if (gTile == nullptr) {
    snprintf(gErr, sizeof(gErr), "brak PSRAM na kafelek");
    return false;
  }

  computeGeometry();
  LOG("Radar mapa: %d klatek x %d B w PSRAM", FRAMES, W * H);
  return true;
}

bool fetch() {
  if (gTile == nullptr || gDemo) return false;   // w symulacji nie nadpisujemy klatek

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
  if (n < FRAMES) {
    snprintf(gErr, sizeof(gErr), "tylko %d klatek", n);
    return false;
  }

  // Bierzemy co druga klatke z konca: -120, -100, ..., 0 minut.
  const time_t nowT = time(nullptr);
  int ok = 0;

  for (int i = 0; i < FRAMES; ++i) {
    const int idx = n - 1 - (FRAMES - 1 - i) * 2;
    if (idx < 0) continue;

    JsonObjectConst f = past[idx];
    const char* path = f["path"];
    const uint32_t t = f["time"] | 0U;
    if (path == nullptr || t == 0) continue;

    char url[160];
    snprintf(url, sizeof(url), "http://%s%s/%d/%d/%d/%d/0/0_0.png", host, path, kTilePx,
             kZoom, gTileX, gTileY);

    uint8_t* png = nullptr;
    size_t len = 0;
    if (!httpGet(url, &png, &len, nullptr)) {
      LOG("Radar mapa: klatka %d nie doszla", i);
      continue;
    }

    // Dekoder (46 kB) w PSRAM — w SRAM nie ma tak duzego ciaglego bloku.
    void* mem = ps_malloc(sizeof(PNG));
    gPng = mem != nullptr ? new (mem) PNG() : nullptr;
    if (gPng == nullptr) {
      free(png);
      snprintf(gErr, sizeof(gErr), "brak pamieci na dekoder");
      return false;
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
      LOG("Radar mapa: klatka %d nie zdekodowana", i);
      continue;
    }

    xSemaphoreTake(gMx, portMAX_DELAY);
    resample(gFrames[i]);
    gMeta[i].epoch = t;
    gMeta[i].offsetMin = nowT > 1700000000 ? (static_cast<int32_t>(t) -
                                              static_cast<int32_t>(nowT)) / 60 : 0;
    gMeta[i].valid = true;
    xSemaphoreGive(gMx);
    ++ok;

    vTaskDelay(pdMS_TO_TICKS(20));   // oddajemy procesor miedzy klatkami
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
  gDemo = on;
  if (!on) {
    gCount = 0;
    gRain = false;
    gUpdatedAt = 0;
    return;
  }
  if (gFrames[0] == nullptr) return;

  const time_t nowT = time(nullptr);
  xSemaphoreTake(gMx, portMAX_DELAY);

  for (int i = 0; i < FRAMES; ++i) {
    // srodek frontu wedruje przez cala mape w ciagu 7 klatek
    const float cx = -60.f + (W + 120.f) * (i / static_cast<float>(FRAMES - 1));

    for (int y = 0; y < H; ++y) {
      for (int x = 0; x < W; ++x) {
        // ukosny front (jak na Baltyku) + falowanie krawedzi
        const float fx = x + (y - H / 2) * 0.35f;
        const float d = fabsf(fx - cx - sinf(y * 0.06f + i) * 12.f);

        uint8_t lv = 0;
        if (d < 14.f) lv = 5;
        else if (d < 24.f) lv = 4;
        else if (d < 34.f) lv = 3;
        else if (d < 48.f) lv = 2;
        else if (d < 62.f) lv = 1;

        // dziury w opadzie — inaczej wyglada jak pomalowany pas
        if (lv > 1 && ((x * 7 + y * 13 + i * 31) % 11) == 0) --lv;
        gFrames[i][y * W + x] = lv;
      }
    }
    gMeta[i].epoch = nowT > 1700000000 ? nowT - (FRAMES - 1 - i) * 20 * 60 : 0;
    gMeta[i].offsetMin = -(FRAMES - 1 - i) * 20;
    gMeta[i].valid = true;
  }
  xSemaphoreGive(gMx);

  gCount = FRAMES;
  gRain = true;
  gUpdatedAt = millis();
  LOG("Radar mapa: SYMULACJA wlaczona (sztuczny front W->E)");
}

}  // namespace radarmap
