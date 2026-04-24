#include "CustomFontSharedCache.h"

#include <esp_heap_caps.h>

namespace crosspoint {
namespace bdf {

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
  size_t slots = maxBitmapBytes_ > 0 ? (budgetBytes / maxBitmapBytes_) : 1;
  if (slots < 1) slots = 1;
  return setCacheCap(slots);
}

void CustomFontSharedCache::clearCache() {
  clearSlab_();
  allocSlab_(); // re-init with same cap
}

bool CustomFontSharedCache::allocSlab_() {
  std::unordered_map<uint32_t, uint16_t>().swap(cacheMap_);
  std::vector<Slot>().swap(slots_);
  std::vector<uint8_t>().swap(bitmapSlab_);

  constexpr size_t kHeapSafetyMargin = 32 * 1024;
  constexpr size_t kLargeSlabThreshold = 2048;
  const size_t slabBytes = cacheCap_ * maxBitmapBytes_;
  const size_t requiredMargin = slabBytes >= kLargeSlabThreshold ? kHeapSafetyMargin : 0;
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (slabBytes + requiredMargin > largest) {
    return false;
  }

  slots_.resize(cacheCap_);
  bitmapSlab_.assign(cacheCap_ * maxBitmapBytes_, 0);
  cacheMap_.reserve(cacheCap_);

  for (size_t i = 0; i < cacheCap_; ++i) {
    slots_[i].prev = kNil;
    slots_[i].next = (i + 1 < cacheCap_) ? static_cast<uint16_t>(i + 1) : kNil;
    slots_[i].occupied = false;
  }
  freeHead_ = cacheCap_ > 0 ? 0 : kNil;
  lruHead_ = kNil;
  lruTail_ = kNil;
  return true;
}

void CustomFontSharedCache::clearSlab_() {
  std::unordered_map<uint32_t, uint16_t>().swap(cacheMap_);
  std::vector<Slot>().swap(slots_);
  std::vector<uint8_t>().swap(bitmapSlab_);
  freeHead_ = kNil;
  lruHead_ = kNil;
  lruTail_ = kNil;
}

void CustomFontSharedCache::lruUnlink_(uint16_t idx) {
  Slot& s = slots_[idx];
  if (s.prev != kNil) slots_[s.prev].next = s.next;
  else lruHead_ = s.next;
  if (s.next != kNil) slots_[s.next].prev = s.prev;
  else lruTail_ = s.prev;
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
  if (cacheCap_ == 0 || maxBitmapBytes_ == 0) return false;
  const uint32_t key = makeKey(variant, codepoint);
  const auto mapIt = cacheMap_.find(key);
  if (mapIt != cacheMap_.end()) {
    const uint16_t idx = mapIt->second;
    if (lruHead_ != idx) {
      lruUnlink_(idx);
      lruPushFront_(idx);
    }
    if (outGlyph) *outGlyph = &slots_[idx].glyph;
    return true;
  }
  return false;
}

uint8_t* CustomFontSharedCache::allocateSlot(uint8_t variant, uint32_t codepoint, const CachedGlyph& metadata, const CachedGlyph** outGlyph) {
  if (cacheCap_ == 0 || maxBitmapBytes_ == 0) return nullptr;
  uint16_t idx = takeFreeSlot_();
  if (idx == kNil) idx = evictLru_();
  if (idx == kNil) return nullptr;

  uint8_t* dst = bitmapSlab_.data() + static_cast<size_t>(idx) * maxBitmapBytes_;
  
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
