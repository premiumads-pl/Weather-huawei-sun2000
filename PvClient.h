#pragma once

#include <cstdint>

#include "PvData.h"

class PvClient {
 public:
  // night = pora, o której falownikowi wolno spać (patrz pvMayBeAsleep()).
  // Nie zmienia to sposobu odpytywania — tylko to, jak nazwiemy brak odpowiedzi:
  // "Falownik uśpiony" (stan neutralny) zamiast "Falownik nie odpowiada" (błąd).
  // Jeśli falownik odpowie mimo nocy, dane lecą normalnie.
  bool fetch(PvModel& out, bool night = false);

 private:
  bool ensureConnected();
  bool readRegs(uint16_t addr, uint16_t count, uint16_t* out);
  bool readRegsRetry(uint16_t addr, uint16_t count, uint16_t* out, int tries = 3);
  bool readS32(uint16_t addr, int32_t& out);
  bool readU32(uint16_t addr, uint32_t& out);
  bool readS16(uint16_t addr, int gain, float& out);
  bool readU16(uint16_t addr, uint16_t& out);
  // (v165) Oba liczniki energii miernika JEDNA ramka (37119..37122, 4 rejestry).
  // Nie dwoma readS32 — patrz uzasadnienie w PvClient.cpp przy wywolaniu.
  bool readMeterEnergy(float& exportKwh, float& importKwh);
  bool warmUp();
};
