#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace crosspoint {
namespace bdf {

// Extracted from CustomFontGlyphSource to share the cache arena across variants.
struct CachedGlyph {
  uint32_t codepoint;
  uint8_t bbxW;
  uint8_t bbxH;
  int8_t bbxOffX;
  int8_t bbxOffY;
  uint8_t advance;
  // ceil(bbxW / 8) * bbxH bytes, MSB-first per row.
  const uint8_t* bitmap;
  size_t bitmapBytes;
};

class CustomFontSharedCache {
 public:
  CustomFontSharedCache() = default;
  ~CustomFontSharedCache();

  CustomFontSharedCache(const CustomFontSharedCache&) = delete;
  CustomFontSharedCache& operator=(const CustomFontSharedCache&) = delete;

  // Ensures the slab size is large enough to hold at least `requiredBytes` per glyph.
  // If `requiredBytes` is larger than the current max, the cache is flushed and reallocated.
  bool ensureMaxBitmapBytes(size_t requiredBytes);

  // Configure max cached glyphs based on a byte budget.
  bool setCacheBudget(size_t budgetBytes);

  // Directly configure max cached glyphs.
  bool setCacheCap(size_t slots);
  size_t cacheCap() const { return cacheCap_; }
  size_t maxBitmapBytes() const { return maxBitmapBytes_; }

  void clearCache();

  // Returns true if hit (and populates *outGlyph).
  bool lookup(uint8_t variant, uint32_t codepoint, const CachedGlyph** outGlyph);

  // Allocates a slot for a newly decoded glyph. Evicts LRU if full.
  // Returns a pointer to the bitmap buffer where the caller should decode the pixels.
  // The cache takes a copy of the metadata.
  uint8_t* allocateSlot(uint8_t variant, uint32_t codepoint, const CachedGlyph& metadata, const CachedGlyph** outGlyph);

  // Call if decoding fails after allocating a slot, to free it.
  void abortSlot(uint8_t variant, uint32_t codepoint);

 private:
  static constexpr uint16_t kNil = 0xFFFF;

  struct Slot {
    CachedGlyph glyph{};
    uint32_t mapKey = 0;
    uint16_t prev = kNil;
    uint16_t next = kNil;
    bool occupied = false;
  };

  uint32_t makeKey(uint8_t variant, uint32_t codepoint) const {
    return (codepoint & 0x0FFFFFFF) | (static_cast<uint32_t>(variant) << 28);
  }

  bool allocSlab_();
  void clearSlab_();
  void lruUnlink_(uint16_t idx);
  void lruPushFront_(uint16_t idx);
  uint16_t takeFreeSlot_();
  uint16_t evictLru_();
  void returnToFreeList_(uint16_t idx);

  size_t maxBitmapBytes_ = 0;
  size_t cacheCap_ = 1;

  std::vector<uint8_t> bitmapSlab_;
  std::vector<Slot> slots_;
  std::unordered_map<uint32_t, uint16_t> cacheMap_;
  uint16_t lruHead_ = kNil;
  uint16_t lruTail_ = kNil;
  uint16_t freeHead_ = kNil;
};

}  // namespace bdf
}  // namespace crosspoint
