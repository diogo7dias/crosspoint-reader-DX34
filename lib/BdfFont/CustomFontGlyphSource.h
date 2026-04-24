#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "BdfIndex.h"
#include "CustomFontSharedCache.h"

namespace crosspoint {
namespace bdf {

// Runtime read-only access to a custom BDF font that has already been indexed.
// On open() it reads the .idx header (kept in RAM), and for small fonts it
// pulls the full IndexEntry[] into RAM and closes the .idx file — making
// every subsequent lookup() pure-RAM binary search. Large fonts (Unifont,
// 57 K glyphs → 912 KB) exceed the in-RAM cap and fall back to seek-based
// search against the still-open .idx file. lookup(cp) caches the decoded
// bitmap via CustomFontSharedCache.
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

  void setSharedCache(CustomFontSharedCache* cache, uint8_t variantIndex) {
    sharedCache_ = cache;
    variantIndex_ = variantIndex;
  }
  size_t maxBitmapBytes() const { return maxBitmapBytes_; }

  uint32_t glyphCount() const { return glyphCount_; }
  int8_t fontBbxW() const { return hdr_.fontBbxW; }
  int8_t fontBbxH() const { return hdr_.fontBbxH; }
  int8_t fontAscent() const { return hdr_.fontAscent; }
  int8_t fontDescent() const { return hdr_.fontDescent; }

  using Glyph = CachedGlyph;

  // O(log N) binary search in .idx + bitmap decode. Returns nullptr if the
  // codepoint is not in the index. On cache hit the slot is promoted to
  // most-recently-used; on miss a new slot is materialized and the oldest
  // slot is evicted if the cache is full.
  const Glyph* lookup(uint32_t codepoint);

 private:
  bool readIndexEntryFromFile(uint32_t indexPos, IndexEntry& out);
  bool decodeBitmap(const IndexEntry& e, uint8_t* dst, size_t dstCap);

  // Cap on the in-RAM index array. Above this, we fall back to seek-based
  // binary search against the still-open idxFile_. 16 KB / 16 B = 1024
  // glyph entries — covers every normal font; Unifont (57 K) stays on disk.
  static constexpr size_t kInMemoryIndexCapBytes = 16 * 1024;

  HalFile bdfFile_;
  HalFile idxFile_;
  bool idxOpen_ = false;
  // When populated, idxFile_ is closed and every lookup is pure-RAM. Array
  // is always sorted ascending by codepoint (enforced at build time).
  std::unique_ptr<IndexEntry[]> indexArray_;

  IndexHeader hdr_{};
  uint32_t glyphCount_ = 0;
  size_t maxBitmapBytes_ = 0;

  CustomFontSharedCache* sharedCache_ = nullptr;
  uint8_t variantIndex_ = 0;
};

}  // namespace bdf
}  // namespace crosspoint
