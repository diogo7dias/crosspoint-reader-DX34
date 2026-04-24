#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
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
// Cache: bounded LRU of decoded glyphs, backed by a single pre-allocated
// bitmap slab + slot array + intrusive index list. After open() + setCacheCap(),
// lookup() performs zero heap allocations. This avoids fragmenting the heap
// with ~cap glyph-sized mallocs during render — fragmentation is what forces
// the reader's OOM-reboot screen when createSectionFile() cannot coalesce a
// 32 KB ZIP dictionary block.
//
// The default cap is 1 (enough for Phase 2a verification). Callers that need
// real render throughput should bump via setCacheCap(). Pointers returned by
// lookup() are stable until that slot is evicted, setCacheCap changes the
// cap, or the source is closed.
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

  // Configure max cached glyphs. Must be >=1; clamped to 65534. Changing
  // the cap while open clears all cached glyphs and reallocates the slab.
  // Returns false if reallocation failed due to heap pressure — source is
  // left closed in that case (caller should treat the font as unavailable).
  bool setCacheCap(size_t slots);
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
    // boundary at the end of each row. Borrowed pointer into the slab;
    // valid until that slot is evicted or the source is closed.
    const uint8_t* bitmap;
    size_t bitmapBytes;
  };

  // O(log N) binary search in .idx + bitmap decode. Returns nullptr if the
  // codepoint is not in the index. On cache hit the slot is promoted to
  // most-recently-used; on miss a new slot is materialized and the oldest
  // slot is evicted if the cache is full.
  const Glyph* lookup(uint32_t codepoint);

 private:
  static constexpr uint16_t kNil = 0xFFFF;

  struct Slot {
    Glyph glyph{};
    uint16_t prev = kNil;  // MRU neighbour (or kNil if head / free)
    uint16_t next = kNil;  // LRU neighbour (or kNil if tail / end of free list)
    bool occupied = false;
  };

  bool readIndexEntry(uint32_t indexPos, IndexEntry& out);
  bool decodeBitmap(const IndexEntry& e, uint8_t* dst, size_t dstCap);

  // Returns false if required slab size cannot fit in the largest free heap
  // block (plus safety margin). On failure the source is left closed.
  bool allocSlab_();
  void clearSlab_();
  void lruUnlink_(uint16_t idx);
  void lruPushFront_(uint16_t idx);
  uint16_t takeFreeSlot_();
  uint16_t evictLru_();
  void returnToFreeList_(uint16_t idx);

  HalFile bdfFile_;
  HalFile idxFile_;
  bool idxOpen_ = false;

  IndexHeader hdr_{};
  uint32_t glyphCount_ = 0;
  size_t maxBitmapBytes_ = 0;

  size_t cacheCap_ = 1;

  // Pre-allocated storage. Sized at allocSlab_() (open / setCacheCap).
  // Single contiguous bitmap arena: slot i owns bytes
  // [i * maxBitmapBytes_, (i + 1) * maxBitmapBytes_).
  std::vector<uint8_t> bitmapSlab_;
  std::vector<Slot> slots_;
  std::unordered_map<uint32_t, uint16_t> cacheMap_;
  uint16_t lruHead_ = kNil;  // most recently used
  uint16_t lruTail_ = kNil;  // least recently used
  uint16_t freeHead_ = kNil;
};

}  // namespace bdf
}  // namespace crosspoint
