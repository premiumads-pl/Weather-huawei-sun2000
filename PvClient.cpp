#include "PvClient.h"

#include "Config.h"
#include "Settings.h"

#include <Arduino.h>
#include <WiFi.h>

namespace {
constexpr uint8_t kUnit = 1;
constexpr uint32_t kTimeoutMs = 2500;

WiFiClient gSock;
uint16_t gTid = 0xA100;
uint16_t gExpectTid = 0;

bool recvExact(uint8_t* buf, size_t n, uint32_t deadline) {
  size_t got = 0;
  while (got < n) {
    if (static_cast<int32_t>(deadline - millis()) <= 0) {
      return false;
    }
    if (!gSock.connected() && gSock.available() == 0) {
      return false;
    }
    const int r = gSock.read(buf + got, n - got);
    if (r <= 0) {
      delay(4);
      continue;
    }
    got += static_cast<size_t>(r);
  }
  return true;
}

}  // namespace

// Odczyt z weryfikacją Transaction ID. Falownik Huawei bywa wolny — spóźniona
// odpowiedź na poprzednie zapytanie zostaje odrzucona zamiast przesunąć wszystkie
// kolejne rejestry (klasyczny błąd desynchronizacji Modbus TCP).
bool PvClient::readRegs(uint16_t addr, uint16_t count, uint16_t* out) {
  const uint16_t tid = ++gTid;
  gExpectTid = tid;

  uint8_t req[12];
  req[0] = static_cast<uint8_t>(tid >> 8);
  req[1] = static_cast<uint8_t>(tid & 0xFF);
  req[2] = 0;
  req[3] = 0;
  req[4] = 0;
  req[5] = 6;
  req[6] = kUnit;
  req[7] = 0x03;
  req[8] = static_cast<uint8_t>(addr >> 8);
  req[9] = static_cast<uint8_t>(addr & 0xFF);
  req[10] = static_cast<uint8_t>(count >> 8);
  req[11] = static_cast<uint8_t>(count & 0xFF);

  if (gSock.write(req, sizeof(req)) != sizeof(req)) {
    return false;
  }

  const uint32_t deadline = millis() + kTimeoutMs;

  while (true) {
    uint8_t hdr[8];
    if (!recvExact(hdr, sizeof(hdr), deadline)) {
      return false;
    }
    const uint16_t rxTid = (static_cast<uint16_t>(hdr[0]) << 8) | hdr[1];
    const uint16_t len = (static_cast<uint16_t>(hdr[4]) << 8) | hdr[5];
    const uint8_t fc = hdr[7];
    if (len < 2) {
      return false;
    }

    uint8_t body[256];
    const size_t bodyLen = len - 2;  // po unit id i function code
    if (bodyLen > sizeof(body) || !recvExact(body, bodyLen, deadline)) {
      return false;
    }

    if (rxTid != tid) {
      continue;  // spóźniona odpowiedź — zignoruj i czekaj na właściwą
    }
    if (fc & 0x80) {
      return false;  // wyjątek Modbus
    }
    if (bodyLen < 1) {
      return false;
    }
    const uint8_t bytes = body[0];
    if (bytes < count * 2 || bodyLen < static_cast<size_t>(1 + bytes)) {
      return false;
    }
    for (uint16_t i = 0; i < count; ++i) {
      out[i] = (static_cast<uint16_t>(body[1 + i * 2]) << 8) | body[2 + i * 2];
    }
    return true;
  }
}

bool PvClient::readRegsRetry(uint16_t addr, uint16_t count, uint16_t* out, int tries) {
  for (int i = 0; i < tries; ++i) {
    if (readRegs(addr, count, out)) {
      return true;
    }
    delay(150);
  }
  return false;
}

bool PvClient::readS32(uint16_t addr, int32_t& out) {
  uint16_t regs[2]{};
  if (!readRegsRetry(addr, 2, regs)) {
    return false;
  }
  const uint32_t u = (static_cast<uint32_t>(regs[0]) << 16) | regs[1];
  out = static_cast<int32_t>(u);
  return true;
}

bool PvClient::readU32(uint16_t addr, uint32_t& out) {
  uint16_t regs[2]{};
  if (!readRegsRetry(addr, 2, regs)) {
    return false;
  }
  out = (static_cast<uint32_t>(regs[0]) << 16) | regs[1];
  return true;
}

bool PvClient::readS16(uint16_t addr, int gain, float& out) {
  uint16_t reg = 0;
  if (!readRegsRetry(addr, 1, &reg)) {
    return false;
  }
  const int16_t v = static_cast<int16_t>(reg);
  out = static_cast<float>(v) / static_cast<float>(gain);
  return true;
}

bool PvClient::readU16(uint16_t addr, uint16_t& out) {
  return readRegsRetry(addr, 1, &out);
}

bool PvClient::warmUp() {
  uint16_t regs[2]{};
  for (int i = 0; i < 4; ++i) {
    if (readRegs(32080, 2, regs)) {
      return true;
    }
    delay(250);
  }
  return false;
}

// Połączenie TCP trzymamy otwarte między odczytami. Huawei po każdym nowym
// handshake potrzebuje długiej rozgrzewki (nawet ~20 s), więc łączenie się od
// nowa co 30 s było głównym powodem, dlaczego dane pojawiały się z opóźnieniem.
bool PvClient::ensureConnected() {
  if (gSock.connected()) {
    // wyczyść ewentualne spóźnione ramki z poprzedniego cyklu
    while (gSock.available()) {
      gSock.read();
    }
    return true;
  }

  gSock.stop();
  if (!gSock.connect(settings().modbusHost, settings().modbusPort, 3000)) {
    return false;
  }
  gSock.setNoDelay(true);
  gSock.setTimeout(kTimeoutMs / 1000);
  delay(400);

  if (!warmUp()) {
    gSock.stop();
    return false;
  }
  Serial.println("Modbus: polaczono z falownikiem");
  return true;
}

bool PvClient::fetch(PvModel& out) {
  out.errorMsg[0] = '\0';

  if (WiFi.status() != WL_CONNECTED) {
    out.online = false;
    strncpy(out.errorMsg, "Brak WiFi", sizeof(out.errorMsg) - 1);
    return false;
  }
  if (!settings().hasInverter()) {
    out.online = false;
    strncpy(out.errorMsg, "Ustaw IP falownika w panelu", sizeof(out.errorMsg) - 1);
    return false;
  }

  if (!ensureConnected()) {
    out.online = false;
    strncpy(out.errorMsg, "Falownik nie odpowiada", sizeof(out.errorMsg) - 1);
    return false;
  }

  PvSnapshot snap{};
  int32_t s32 = 0;
  uint32_t u32 = 0;
  float f = 0.f;
  uint16_t u16 = 0;

  int fails = 0;
  if (readS32(32064, s32)) snap.powerDcW = s32; else ++fails;
  if (readS32(32080, s32)) snap.powerAcW = s32; else ++fails;
  if (readS32(37113, s32)) snap.gridPowerW = s32; else ++fails;
  if (readU32(32106, u32)) snap.energyTotalKwh = static_cast<float>(u32) / 100.f; else ++fails;
  if (readU32(32114, u32)) snap.energyTodayKwh = static_cast<float>(u32) / 100.f; else ++fails;
  if (readS16(32016, 10, f)) snap.pvVoltageV = f;
  if (readS16(32087, 10, f)) snap.inverterTempC = f;
  if (readU16(32086, u16)) snap.efficiencyPct = static_cast<float>(u16) / 100.f;
  if (readU16(32089, u16)) snap.statusCode = u16;
  if (readU16(37100, u16)) snap.meterOk = (u16 == 1);

  // Sesja się posypała — zamknij, żeby następny cykl zaczął od czystego handshake.
  if (fails >= 3) {
    gSock.stop();
    out.online = false;
    strncpy(out.errorMsg, "Modbus bez odpowiedzi", sizeof(out.errorMsg) - 1);
    return false;
  }

  // Zużycie domu = produkcja AC minus to, co poszło do sieci.
  snap.houseLoadW = snap.powerAcW - snap.gridPowerW;
  snap.valid = true;
  out.data = snap;
  out.online = true;
  return true;
}
