#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <functional>

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

// One glyph as yielded by readAllGlyphs(). Bitmap bytes are NOT included —
// the index builder only needs metrics + offset; runtime decode pulls the
// bitmap on demand using bitmapOffset.
struct BdfGlyphMeta {
  uint32_t codepoint;     // ENCODING value (skipped if < 0)
  uint32_t bdfOffset;     // byte offset where "STARTCHAR" begins (debug / legacy)
  uint32_t bitmapOffset;  // byte offset of the first hex row, i.e. the byte
                          // AFTER the "BITMAP\n" line. 0 if this glyph had
                          // no BITMAP (rare, malformed).
  uint8_t bbxW;           // BBX W
  uint8_t bbxH;           // BBX H
  int8_t bbxOffX;         // BBX X
  int8_t bbxOffY;         // BBX Y
  uint8_t advance;        // DWIDTH x; 0 if absent
};

// Per-glyph callback. Return false to abort enumeration.
using BdfGlyphCallback = std::function<bool(const BdfGlyphMeta& g)>;

struct BdfEnumResult {
  uint32_t glyphsYielded = 0;
  uint32_t glyphsSkipped = 0;  // missing/invalid ENCODING
  bool ok = false;
  const char* error = nullptr;
};

class BdfParser {
 public:
  // Reads the BDF header (STARTFONT...CHARS N), then stops at the first
  // STARTCHAR. Does NOT enumerate glyphs.
  //
  // The parser yields and resets the watchdog every `wdtTickEvery` lines so
  // a 9 MB Unifont scan does not trip the task watchdog.
  static BdfHeader readHeader(HalFile& in, size_t wdtTickEvery = 256);

  // Enumerate every glyph (STARTCHAR..ENDCHAR block) in the file. Caller is
  // responsible for seeking `in` to the byte right after readHeader returned
  // — i.e. the first STARTCHAR line (or use rewind + re-call readHeader).
  //
  // For each glyph, fires `cb(meta)`. If `cb` returns false, enumeration
  // stops with ok=true. Glyphs without a parseable ENCODING (e.g. ENCODING
  // -1) are counted as skipped, not yielded.
  //
  // Bitmap bytes are NOT yielded — the consumer can re-open the file at
  // `meta.bdfOffset` and parse STARTCHAR..ENDCHAR independently.
  static BdfEnumResult readAllGlyphs(HalFile& in, const BdfGlyphCallback& cb, size_t wdtTickEvery = 32);
};

}  // namespace bdf
}  // namespace crosspoint
