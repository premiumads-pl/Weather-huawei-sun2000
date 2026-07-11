#pragma once

#include <cstdint>

#include "PvData.h"

class PvClient {
 public:
  bool fetch(PvModel& out);

 private:
  bool readRegs(uint16_t addr, uint16_t count, uint16_t* out);
  bool readRegsRetry(uint16_t addr, uint16_t count, uint16_t* out, int tries = 3);
  bool readS32(uint16_t addr, int32_t& out);
  bool readU32(uint16_t addr, uint32_t& out);
  bool readS16(uint16_t addr, int gain, float& out);
  bool readU16(uint16_t addr, uint16_t& out);
  bool warmUp();
};
