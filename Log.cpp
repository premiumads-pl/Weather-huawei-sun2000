#include "Log.h"

#include <cstdarg>
#include <cstring>

namespace {
constexpr size_t kSize = 3072;
char gBuf[kSize];
size_t gHead = 0;
bool gWrapped = false;
Diag gDiag;
portMUX_TYPE gMux = portMUX_INITIALIZER_UNLOCKED;

void put(char c) {
  gBuf[gHead] = c;
  gHead = (gHead + 1) % kSize;
  if (gHead == 0) {
    gWrapped = true;
  }
}
}  // namespace

Diag& diag() {
  return gDiag;
}

void logPrintf(const char* fmt, ...) {
  char line[192];

  const uint32_t up = millis() / 1000;
  const int pre = snprintf(line, sizeof(line), "[%02lu:%02lu:%02lu] ",
                           static_cast<unsigned long>(up / 3600),
                           static_cast<unsigned long>((up / 60) % 60),
                           static_cast<unsigned long>(up % 60));

  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line + pre, sizeof(line) - pre, fmt, ap);
  va_end(ap);

  Serial.print(line);

  portENTER_CRITICAL(&gMux);
  for (const char* p = line; *p; ++p) {
    put(*p);
  }
  if (line[strlen(line) - 1] != '\n') {
    put('\n');
  }
  portEXIT_CRITICAL(&gMux);

  const uint32_t h = ESP.getFreeHeap();
  if (h < gDiag.minHeap) {
    gDiag.minHeap = h;
  }
}

String logDump() {
  String out;
  out.reserve(kSize + 64);
  portENTER_CRITICAL(&gMux);
  if (gWrapped) {
    for (size_t i = gHead; i < kSize; ++i) {
      out += gBuf[i];
    }
  }
  for (size_t i = 0; i < gHead; ++i) {
    out += gBuf[i];
  }
  portEXIT_CRITICAL(&gMux);
  return out;
}
