#include "CustomFontGlyphSource.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

#include <cctype>
#include <cstring>

namespace crosspoint {
namespace bdf {

namespace {

constexpr size_t kBitmapLineBuf = 192;

// Parse one hex character. Returns -1 on non-hex.
int hexNibble(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

int readBitmapLine(HalFile& in, char* buf, size_t bufSize) {
  if (bufSize == 0) return -1;
  size_t len = 0;
  bool sawAny = false;
  while (true) {
    const int c = in.read();
    if (c < 0) {
      if (!sawAny) return -1;
      break;
    }
    sawAny = true;
    if (c == '\n') break;
    if (c == '\r') continue;
    if (len + 1 < bufSize) buf[len++] = static_cast<char>(c);
  }
  buf[len] = '\0';
  return static_cast<int>(len);
}

}  // namespace

CustomFontGlyphSource::~CustomFontGlyphSource() { close(); }

bool CustomFontGlyphSource::open(const char* bdfPath, const char* idxPath) {
  close();

  idxFile_ = Storage.open(idxPath);
  if (!idxFile_) return false;

  if (idxFile_.read(&hdr_, sizeof(hdr_)) != static_cast<int>(sizeof(hdr_))) {
    idxFile_.close();
    return false;
  }
  if (hdr_.magic != kBdfIndexMagic || hdr_.version != kBdfIndexVersion) {
    idxFile_.close();
    return false;
  }
  glyphCount_ = hdr_.glyphCount;

  bdfFile_ = Storage.open(bdfPath);
  if (!bdfFile_) {
    idxFile_.close();
    return false;
  }

  // Upper-bound bitmap size from the font's bounding box. Per-glyph BBX may
  // be smaller; decodeBitmap writes only the bytes actually used and the
  // unused tail of the slot is never read (bitmapBytes bounds the consumer).
  const int8_t bbxW = hdr_.fontBbxW > 0 ? hdr_.fontBbxW : 1;
  const int8_t bbxH = hdr_.fontBbxH > 0 ? hdr_.fontBbxH : 1;
  maxBitmapBytes_ = static_cast<size_t>((bbxW + 7) / 8) * static_cast<size_t>(bbxH);
  if (maxBitmapBytes_ == 0) maxBitmapBytes_ = 1;

  idxOpen_ = true;
  if (!allocSlab_()) {
    close();
    return false;
  }
  return true;
}

void CustomFontGlyphSource::close() {
  if (idxOpen_) {
    idxFile_.close();
    bdfFile_.close();
    idxOpen_ = false;
  }
  clearSlab_();
  glyphCount_ = 0;
  maxBitmapBytes_ = 0;
}

bool CustomFontGlyphSource::setCacheCap(size_t slots) {
  if (slots == 0) slots = 1;
  if (slots > 65534) slots = 65534;
  if (slots == cacheCap_) return true;
  const size_t prevCap = cacheCap_;
  cacheCap_ = slots;
  if (idxOpen_ && !allocSlab_()) {
    cacheCap_ = prevCap;
    close();
    return false;
  }
  return true;
}

bool CustomFontGlyphSource::allocSlab_() {
  // Rebuild cache structures from scratch. Called from open() (cold) and
  // setCacheCap() when the cap actually changes. All prior cached glyphs
  // are dropped — callers holding a Glyph* from before this call must not
  // dereference it.
  //
  // vector::clear() + assign(smaller) does NOT shrink capacity, so the old
  // (possibly tens of KB) heap block stays attached to the vector. Swap with
  // a default-constructed container guarantees the release, which is what
  // trimCache(1) relies on to reclaim contiguous heap for the epub section
  // ZIP dict.
  std::unordered_map<uint32_t, uint16_t>().swap(cacheMap_);
  std::vector<Slot>().swap(slots_);
  std::vector<uint8_t>().swap(bitmapSlab_);

  // -fno-exceptions: a failed std::vector::assign aborts via operator new
  // rather than throwing. Pre-check the largest contiguous free block so
  // we can bail gracefully and let the caller continue without this font.
  // Safety margin only applies when the slab is big — trim paths allocate
  // a few hundred bytes and MUST succeed (otherwise setCacheCap closes the
  // source, leaving the font unrenderable and the book with blank text).
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

  // Thread every slot into the free list via `next`.
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

void CustomFontGlyphSource::clearSlab_() {
  // See allocSlab_ — swap idiom forces heap release where clear() alone
  // would keep the underlying allocation attached.
  std::unordered_map<uint32_t, uint16_t>().swap(cacheMap_);
  std::vector<Slot>().swap(slots_);
  std::vector<uint8_t>().swap(bitmapSlab_);
  freeHead_ = kNil;
  lruHead_ = kNil;
  lruTail_ = kNil;
}

void CustomFontGlyphSource::lruUnlink_(uint16_t idx) {
  Slot& s = slots_[idx];
  if (s.prev != kNil) slots_[s.prev].next = s.next;
  else lruHead_ = s.next;
  if (s.next != kNil) slots_[s.next].prev = s.prev;
  else lruTail_ = s.prev;
  s.prev = kNil;
  s.next = kNil;
}

void CustomFontGlyphSource::lruPushFront_(uint16_t idx) {
  Slot& s = slots_[idx];
  s.prev = kNil;
  s.next = lruHead_;
  if (lruHead_ != kNil) slots_[lruHead_].prev = idx;
  lruHead_ = idx;
  if (lruTail_ == kNil) lruTail_ = idx;
}

uint16_t CustomFontGlyphSource::takeFreeSlot_() {
  if (freeHead_ == kNil) return kNil;
  const uint16_t idx = freeHead_;
  freeHead_ = slots_[idx].next;
  slots_[idx].prev = kNil;
  slots_[idx].next = kNil;
  return idx;
}

uint16_t CustomFontGlyphSource::evictLru_() {
  if (lruTail_ == kNil) return kNil;
  const uint16_t idx = lruTail_;
  Slot& s = slots_[idx];
  cacheMap_.erase(s.glyph.codepoint);
  s.occupied = false;
  lruUnlink_(idx);
  return idx;
}

void CustomFontGlyphSource::returnToFreeList_(uint16_t idx) {
  Slot& s = slots_[idx];
  s.occupied = false;
  s.prev = kNil;
  s.next = freeHead_;
  freeHead_ = idx;
}

bool CustomFontGlyphSource::readIndexEntry(uint32_t indexPos, IndexEntry& out) {
  const size_t fileOffset = sizeof(IndexHeader) + indexPos * sizeof(IndexEntry);
  if (!idxFile_.seekSet(fileOffset)) return false;
  return idxFile_.read(&out, sizeof(out)) == static_cast<int>(sizeof(out));
}

const CustomFontGlyphSource::Glyph* CustomFontGlyphSource::lookup(uint32_t codepoint) {
  if (!idxOpen_ || glyphCount_ == 0 || cacheCap_ == 0) return nullptr;

  // Cache hit: promote to MRU and return the slab-backed pointer.
  const auto mapIt = cacheMap_.find(codepoint);
  if (mapIt != cacheMap_.end()) {
    const uint16_t idx = mapIt->second;
    if (lruHead_ != idx) {
      lruUnlink_(idx);
      lruPushFront_(idx);
    }
    return &slots_[idx].glyph;
  }

  // Binary search the .idx by codepoint.
  uint32_t lo = 0;
  uint32_t hi = glyphCount_;
  IndexEntry hit{};
  bool found = false;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    IndexEntry e{};
    if (!readIndexEntry(mid, e)) return nullptr;
    if (e.codepoint == codepoint) {
      hit = e;
      found = true;
      break;
    }
    if (e.codepoint < codepoint) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (!found) return nullptr;

  // Grab a slot — from the free list first, fall back to evicting LRU.
  uint16_t idx = takeFreeSlot_();
  if (idx == kNil) idx = evictLru_();
  if (idx == kNil) return nullptr;

  uint8_t* dst = bitmapSlab_.data() + static_cast<size_t>(idx) * maxBitmapBytes_;
  if (!decodeBitmap(hit, dst, maxBitmapBytes_)) {
    returnToFreeList_(idx);
    return nullptr;
  }

  Slot& s = slots_[idx];
  s.glyph.codepoint = hit.codepoint;
  s.glyph.bbxW = hit.bitmapW;
  s.glyph.bbxH = hit.bitmapH;
  s.glyph.bbxOffX = hit.bbxOffX;
  s.glyph.bbxOffY = hit.bbxOffY;
  s.glyph.advance = hit.advance;
  s.glyph.bitmap = dst;
  s.glyph.bitmapBytes = static_cast<size_t>((hit.bitmapW + 7) / 8) * static_cast<size_t>(hit.bitmapH);
  s.occupied = true;
  lruPushFront_(idx);
  cacheMap_[codepoint] = idx;
  return &s.glyph;
}

bool CustomFontGlyphSource::decodeBitmap(const IndexEntry& e, uint8_t* dst, size_t dstCap) {
  if (!bdfFile_.seekSet(e.bdfOffset)) return false;

  const size_t bytesPerRow = (e.bitmapW + 7) / 8;
  const size_t totalBytes = bytesPerRow * e.bitmapH;
  if (totalBytes > dstCap) return false;  // Glyph exceeds font bounding box — rejected.
  if (totalBytes == 0) return true;        // zero-size glyph (e.g. SPACE) — empty bitmap is valid
  std::memset(dst, 0, totalBytes);

  char line[kBitmapLineBuf];
  // Walk lines until BITMAP token.
  bool sawBitmap = false;
  for (int safety = 0; safety < 64; ++safety) {
    const int n = readBitmapLine(bdfFile_, line, sizeof(line));
    if (n < 0) return false;
    if (std::strncmp(line, "BITMAP", 6) == 0 && (line[6] == '\0' || line[6] == ' ' || line[6] == '\t')) {
      sawBitmap = true;
      break;
    }
  }
  if (!sawBitmap) return false;

  // Read H rows. Each row is `bytesPerRow * 2` hex chars (BDF pads each row
  // to a whole byte). Tolerate trailing whitespace or CR (already stripped).
  for (size_t row = 0; row < e.bitmapH; ++row) {
    const int n = readBitmapLine(bdfFile_, line, sizeof(line));
    if (n < 0) return false;
    if (static_cast<size_t>(n) < bytesPerRow * 2) return false;
    for (size_t b = 0; b < bytesPerRow; ++b) {
      const int hi = hexNibble(static_cast<unsigned char>(line[b * 2]));
      const int lo = hexNibble(static_cast<unsigned char>(line[b * 2 + 1]));
      if (hi < 0 || lo < 0) return false;
      dst[row * bytesPerRow + b] = static_cast<uint8_t>((hi << 4) | lo);
    }
  }
  return true;
}

}  // namespace bdf
}  // namespace crosspoint
