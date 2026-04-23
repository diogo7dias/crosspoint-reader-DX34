#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>

namespace crosspoint {
namespace bdf {

struct BdfHeader {
  uint32_t glyphCount = 0;
  int16_t bbxW = 0;
  int16_t bbxH = 0;
  int16_t bbxOffX = 0;
  int16_t bbxOffY = 0;
  int16_t ascent = 0;
  int16_t descent = 0;
  uint16_t pointSize = 0;
  bool ok = false;
  // Static string literal — never owned, never freed.
  const char* error = nullptr;
};

class BdfParser {
 public:
  // Reads the BDF header (STARTFONT...CHARS N), then stops at the first
  // STARTCHAR. Does NOT enumerate glyphs (that is Phase 2).
  //
  // The parser yields and resets the watchdog every `wdtTickEvery` lines so
  // a 9 MB Unifont scan does not trip the task watchdog.
  static BdfHeader readHeader(HalFile& in, size_t wdtTickEvery = 256);
};

}  // namespace bdf
}  // namespace crosspoint
