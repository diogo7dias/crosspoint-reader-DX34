#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "BdfIndex.h"

namespace crosspoint {
namespace bdf {

// Runtime read-only access to a custom BDF font that has already been indexed.
// On open() it reads the .idx header (kept in RAM) and keeps both the BDF and
// .idx files open. lookup(cp) does a binary search in the .idx (via SD seek)
// and lazy-decodes the bitmap from the BDF on cache miss.
//
// Cache: single most-recent glyph. Sufficient for Slice 2a verification (one
// codepoint dumped at a time). Phase 2b will replace this with a real LRU
// once GfxRenderer is dispatching multiple glyphs per page.
class CustomFontGlyphSource {
 public:
  CustomFontGlyphSource() = default;
  ~CustomFontGlyphSource();

  CustomFontGlyphSource(const CustomFontGlyphSource&) = delete;
  CustomFontGlyphSource& operator=(const CustomFontGlyphSource&) = delete;

  // Open both the .idx and the BDF. On failure returns false and leaves the
  // source in the closed state (lookup will return nullptr).
  bool open(const char* bdfPath, const char* idxPath);
  void close();
  bool isOpen() const { return idxOpen_; }

  uint32_t glyphCount() const { return glyphCount_; }
  int8_t fontBbxW() const { return hdr_.fontBbxW; }
  int8_t fontBbxH() const { return hdr_.fontBbxH; }
  int8_t fontAscent() const { return hdr_.fontAscent; }
  int8_t fontDescent() const { return hdr_.fontDescent; }

  struct Glyph {
    uint32_t codepoint;
    uint8_t bbxW;
    uint8_t bbxH;
    int8_t bbxOffX;
    int8_t bbxOffY;
    uint8_t advance;
    // ceil(bbxW / 8) * bbxH bytes, MSB-first per row, padded to byte
    // boundary at the end of each row. Borrowed pointer; valid until the
    // next lookup() call.
    const uint8_t* bitmap;
    size_t bitmapBytes;
  };

  // O(log N) binary search in .idx + bitmap decode. Returns nullptr if the
  // codepoint is not in the index.
  const Glyph* lookup(uint32_t codepoint);

 private:
  bool readIndexEntry(uint32_t indexPos, IndexEntry& out);
  bool decodeBitmap(const IndexEntry& e);

  HalFile bdfFile_;
  HalFile idxFile_;
  bool idxOpen_ = false;

  IndexHeader hdr_{};
  uint32_t glyphCount_ = 0;

  // 1-slot cache.
  bool cacheValid_ = false;
  Glyph cachedGlyph_{};
  std::vector<uint8_t> bitmapBuf_;
};

}  // namespace bdf
}  // namespace crosspoint
