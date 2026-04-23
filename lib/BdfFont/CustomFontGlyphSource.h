#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <list>
#include <unordered_map>
#include <vector>

#include "BdfIndex.h"

namespace crosspoint {
namespace bdf {

// Runtime read-only access to a custom BDF font that has already been indexed.
// On open() it reads the .idx header (kept in RAM) and keeps both the BDF and
// .idx files open. lookup(cp) does a binary search in the .idx (via SD seek)
// and lazy-decodes the bitmap from the BDF on cache miss.
//
// Cache: bounded LRU of decoded glyphs. Default cap (1 slot) preserves the
// Phase 2a verification path; callers that need real render throughput should
// bump via setCacheCap(). Each slot owns its bitmap bytes, so pointers
// returned by lookup() are stable until that slot is evicted or the source is
// closed.
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

  // Configure max cached glyphs. Must be >=1. Can be called at any time;
  // shrinks/evicts synchronously.
  void setCacheCap(size_t slots);
  size_t cacheCap() const { return cacheCap_; }

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
    // boundary at the end of each row. Borrowed pointer owned by an LRU
    // slot; valid until that slot is evicted or the source is closed.
    const uint8_t* bitmap;
    size_t bitmapBytes;
  };

  // O(log N) binary search in .idx + bitmap decode. Returns nullptr if the
  // codepoint is not in the index. On cache hit the slot is promoted to
  // most-recently-used; on miss a new slot is materialized and the oldest
  // slot is evicted if the cache is full.
  const Glyph* lookup(uint32_t codepoint);

 private:
  struct CacheSlot {
    Glyph glyph{};
    std::vector<uint8_t> bitmap;
  };
  using SlotList = std::list<CacheSlot>;

  bool readIndexEntry(uint32_t indexPos, IndexEntry& out);
  bool decodeBitmap(const IndexEntry& e, std::vector<uint8_t>& out);

  HalFile bdfFile_;
  HalFile idxFile_;
  bool idxOpen_ = false;

  IndexHeader hdr_{};
  uint32_t glyphCount_ = 0;

  size_t cacheCap_ = 1;
  SlotList cacheList_;  // front = most recently used
  std::unordered_map<uint32_t, SlotList::iterator> cacheMap_;
};

}  // namespace bdf
}  // namespace crosspoint
