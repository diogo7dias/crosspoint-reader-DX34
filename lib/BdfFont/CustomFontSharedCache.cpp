#include "CustomFontSharedCache.h"

#include <esp_heap_caps.h>

#include <cstdlib>

namespace crosspoint {
namespace bdf {

// Permanent BSS reservation for the decoded-glyph bitmap slab. Sized to the
// kCustomFontCacheBudgetBytes constant used by CustomFontManager when it
// calls setCacheBudget. Previously this buffer was heap_caps_malloc'd fresh
// on every registerWithRenderer / restoreSlab, but by the time createSection
// File's 82 s layout pass had run the heap was routinely fragmented enough
// that a 16 KB contiguous alloc failed → `restored 0 custom font caches
// (failed=1)` → every lookup() hit the metrics fallback → blank text on the
// first page after a font switch. Single-active-custom-font invariant
// (enforced in CustomFontManager::registerWithRenderer) means exactly one
// CustomFontSharedCache ever points into this buffer at a time, and access
// is single-threaded from the main loop, so no locking is needed.
//
// Cost: 16 KB permanent SRAM (BSS). The device has ~400 KB SRAM total and
// the dedicated-reading workload benefits far more from a bulletproof font
// path than from the flex that heap-allocating this region provided.
constexpr size_t kStaticSlabBytes = 16 * 1024;
static uint8_t gStaticBitmapSlab[kStaticSlabBytes];

CustomFontSharedCache::~CustomFontSharedCache() { clearSlab_(); }

bool CustomFontSharedCache::ensureMaxBitmapBytes(size_t requiredBytes) {
  if (requiredBytes <= maxBitmapBytes_) return true;
  // Stage locally — if allocSlab_() fails due to heap pressure we must not
  // leave maxBitmapBytes_ bumped, or setCacheBudget's slots-from-bytes math
  // will use a size that has no backing slab (→ cacheCap_>0 but bitmapSlab_
  // empty → null-deref on allocateSlot).
  const size_t prevMax = maxBitmapBytes_;
  maxBitmapBytes_ = requiredBytes;
  if (allocSlab_()) return true;
  // Grow failed. allocSlab_ already dropped the old slab. Roll back max
  // and try to re-init at the prior size so at least the smaller variants
  // can still cache. If that also fails the cache is empty (miss on every
  // lookup → callers fall back to returning nullptr → glyph skipped).
  maxBitmapBytes_ = prevMax;
  if (prevMax > 0) allocSlab_();
  return false;
}

bool CustomFontSharedCache::setCacheCap(size_t slots) {
  if (slots == 0) slots = 1;
  if (slots > 65534) slots = 65534;
  if (slots == cacheCap_) return true;
  const size_t prevCap = cacheCap_;
  cacheCap_ = slots;
  if (maxBitmapBytes_ > 0 && !allocSlab_()) {
    cacheCap_ = prevCap;
    clearSlab_();
    return false;
  }
  return true;
}

bool CustomFontSharedCache::setCacheBudget(size_t budgetBytes) {
  lastBudgetBytes_ = budgetBytes;
  if (maxBitmapBytes_ == 0) return setCacheCap(1);

  // Try the requested budget first. If the slab alloc fails we fall back
  // toward a 1-slot cache rather than giving up — the caller (the font
  // register path) used to bail with a "heap floor reached" log when
  // largest was just 5–10 KB short of the full 16 KB budget, which left
  // the renderer with no custom font at all and triggered "Font N not
  // found" spam + blank text on every page (captured 2026-04-24).
  // Degraded-but-working beats not-registered; lookup() falls back to the
  // decode-on-each-call path via the metrics fallback when the cache is
  // tiny, and we re-grow the slab on the next releaseSlab/restoreSlab
  // cycle when heap pressure eases.
  size_t slots = budgetBytes / maxBitmapBytes_;
  if (slots < 1) slots = 1;
  while (slots >= 1) {
    if (setCacheCap(slots)) return true;
    if (slots == 1) break;
    // Halve the slot count and retry. The allocator may have had room for
    // fewer slots but not the full slab; give it a chance before declaring
    // the font unusable.
    slots /= 2;
    if (slots < 1) slots = 1;
  }
  return false;
}

void CustomFontSharedCache::clearCache() {
  clearSlab_();
  allocSlab_();  // re-init with same cap
}

void CustomFontSharedCache::releaseSlab() {
  // Snapshot the live shape before dropping it so restoreSlab() can rebuild
  // an identical cache. lastBudgetBytes_ is carried through because
  // setCacheBudget() is the real source of truth for "how big was this
  // supposed to be" — cap * maxBytes may have been rounded.
  if (bitmapSlab_ != nullptr || cacheCap_ > 0) {
    savedCap_ = cacheCap_;
    savedMaxBitmapBytes_ = maxBitmapBytes_;
    savedBudgetBytes_ = lastBudgetBytes_;
  }
  clearSlab_();
  cacheCap_ = 0;
  maxBitmapBytes_ = 0;
}

bool CustomFontSharedCache::restoreSlab() {
  if (savedMaxBitmapBytes_ == 0) return false;  // nothing to restore
  maxBitmapBytes_ = savedMaxBitmapBytes_;
  // Prefer the original byte-budget; falls back to the saved slot count if
  // we never went through setCacheBudget (shouldn't happen in production).
  if (savedBudgetBytes_ > 0) {
    lastBudgetBytes_ = savedBudgetBytes_;
    size_t slots = savedBudgetBytes_ / savedMaxBitmapBytes_;
    if (slots < 1) slots = 1;
    cacheCap_ = slots;
  } else {
    cacheCap_ = savedCap_ > 0 ? savedCap_ : 1;
  }
  const bool ok = allocSlab_();
  // Clear the save slots regardless — a failed restore should not re-attempt
  // every frame. If allocation failed the cache stays empty; glyphs will
  // miss and the decode path will skip them (same as first-boot behaviour).
  savedCap_ = 0;
  savedMaxBitmapBytes_ = 0;
  savedBudgetBytes_ = 0;
  return ok;
}

bool CustomFontSharedCache::allocSlab_() {
  // Drop the prior slot metadata tables. Unlike the old heap-slab design we
  // no longer free the bitmap buffer itself — gStaticBitmapSlab lives in
  // BSS for the life of the program.
  std::unordered_map<uint32_t, uint16_t>().swap(cacheMap_);
  std::vector<Slot>().swap(slots_);
  bitmapSlab_ = nullptr;
  slabBytes_ = 0;

  if (cacheCap_ == 0 || maxBitmapBytes_ == 0) {
    // Nothing to allocate. Reset LRU/free pointers so callers see a clean
    // closed state (lookup/allocateSlot short-circuit on cacheCap_==0).
    freeHead_ = kNil;
    lruHead_ = kNil;
    lruTail_ = kNil;
    return true;
  }

  // Clamp cacheCap_ so (cacheCap_ * maxBitmapBytes_) fits in the static
  // buffer. Fonts with larger bbx get fewer slots but still work — no
  // failure path. In practice every real-world font the user can install
  // uses < 800 B per glyph, so 16 KB / 800 ≈ 20 slots is still a usable
  // cache even in the worst case.
  if (cacheCap_ * maxBitmapBytes_ > kStaticSlabBytes) {
    cacheCap_ = kStaticSlabBytes / maxBitmapBytes_;
    if (cacheCap_ == 0) cacheCap_ = 1;
  }

  // Point at the static buffer. No malloc, no heap pressure, no failure.
  // Only the trailing slots_.resize() / cacheMap_.reserve() calls touch
  // the heap, and those allocations are in the low-KB range that the
  // allocator has never struggled with in practice.
  bitmapSlab_ = gStaticBitmapSlab;
  slabBytes_ = cacheCap_ * maxBitmapBytes_;

  slots_.resize(cacheCap_);
  cacheMap_.reserve(cacheCap_);

  for (size_t i = 0; i < cacheCap_; ++i) {
    slots_[i].prev = kNil;
    slots_[i].next = (i + 1 < cacheCap_) ? static_cast<uint16_t>(i + 1) : kNil;
    slots_[i].occupied = false;
  }
  freeHead_ = 0;
  lruHead_ = kNil;
  lruTail_ = kNil;
  return true;
}

void CustomFontSharedCache::clearSlab_() {
  std::unordered_map<uint32_t, uint16_t>().swap(cacheMap_);
  std::vector<Slot>().swap(slots_);
  // bitmapSlab_ points into the static gStaticBitmapSlab buffer when active;
  // just forget the pointer, never free.
  bitmapSlab_ = nullptr;
  slabBytes_ = 0;
  freeHead_ = kNil;
  lruHead_ = kNil;
  lruTail_ = kNil;
}

void CustomFontSharedCache::lruUnlink_(uint16_t idx) {
  Slot& s = slots_[idx];
  if (s.prev != kNil)
    slots_[s.prev].next = s.next;
  else
    lruHead_ = s.next;
  if (s.next != kNil)
    slots_[s.next].prev = s.prev;
  else
    lruTail_ = s.prev;
  s.prev = kNil;
  s.next = kNil;
}

void CustomFontSharedCache::lruPushFront_(uint16_t idx) {
  Slot& s = slots_[idx];
  s.prev = kNil;
  s.next = lruHead_;
  if (lruHead_ != kNil) slots_[lruHead_].prev = idx;
  lruHead_ = idx;
  if (lruTail_ == kNil) lruTail_ = idx;
}

uint16_t CustomFontSharedCache::takeFreeSlot_() {
  if (freeHead_ == kNil) return kNil;
  const uint16_t idx = freeHead_;
  freeHead_ = slots_[idx].next;
  slots_[idx].prev = kNil;
  slots_[idx].next = kNil;
  return idx;
}

uint16_t CustomFontSharedCache::evictLru_() {
  if (lruTail_ == kNil) return kNil;
  const uint16_t idx = lruTail_;
  Slot& s = slots_[idx];
  cacheMap_.erase(s.mapKey);
  s.occupied = false;
  lruUnlink_(idx);
  return idx;
}

void CustomFontSharedCache::returnToFreeList_(uint16_t idx) {
  Slot& s = slots_[idx];
  s.occupied = false;
  s.prev = kNil;
  s.next = freeHead_;
  freeHead_ = idx;
}

bool CustomFontSharedCache::lookup(uint8_t variant, uint32_t codepoint, const CachedGlyph** outGlyph) {
  if (cacheCap_ == 0 || maxBitmapBytes_ == 0 || bitmapSlab_ == nullptr) {
    stats_.misses += 1;
    return false;
  }
  const uint32_t key = makeKey(variant, codepoint);
  const auto mapIt = cacheMap_.find(key);
  if (mapIt != cacheMap_.end()) {
    const uint16_t idx = mapIt->second;
    if (lruHead_ != idx) {
      lruUnlink_(idx);
      lruPushFront_(idx);
    }
    if (outGlyph) *outGlyph = &slots_[idx].glyph;
    stats_.hits += 1;
    return true;
  }
  stats_.misses += 1;
  return false;
}

uint8_t* CustomFontSharedCache::allocateSlot(uint8_t variant, uint32_t codepoint, const CachedGlyph& metadata,
                                             const CachedGlyph** outGlyph) {
  if (cacheCap_ == 0 || maxBitmapBytes_ == 0 || bitmapSlab_ == nullptr) return nullptr;
  uint16_t idx = takeFreeSlot_();
  if (idx == kNil) {
    idx = evictLru_();
    if (idx != kNil) stats_.evictions += 1;  // count ONLY real evictions, not free-slot promotions
  }
  if (idx == kNil) return nullptr;

  uint8_t* dst = bitmapSlab_ + static_cast<size_t>(idx) * maxBitmapBytes_;

  Slot& s = slots_[idx];
  s.glyph = metadata;
  s.glyph.bitmap = dst;
  s.mapKey = makeKey(variant, codepoint);
  s.occupied = true;

  lruPushFront_(idx);
  cacheMap_[s.mapKey] = idx;

  if (outGlyph) *outGlyph = &s.glyph;
  return dst;
}

void CustomFontSharedCache::abortSlot(uint8_t variant, uint32_t codepoint) {
  const uint32_t key = makeKey(variant, codepoint);
  const auto mapIt = cacheMap_.find(key);
  if (mapIt != cacheMap_.end()) {
    const uint16_t idx = mapIt->second;
    cacheMap_.erase(mapIt);
    lruUnlink_(idx);
    returnToFreeList_(idx);
  }
}

}  // namespace bdf
}  // namespace crosspoint
