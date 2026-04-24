#include "CustomFontGlyphSource.h"

#include <Arduino.h>
#include <Logging.h>
#include <esp_heap_caps.h>

#include <cctype>
#include <cstring>

namespace crosspoint {
namespace bdf {

namespace {

// Parse one hex character. Returns -1 on non-hex.
int hexNibble(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
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
    // Reject older versions. v1 indexes exist in the wild from prior builds;
    // they used bdfOffset (STARTCHAR position) which is wrong for the new
    // decodeBitmap. Treating them as "not installed" routes the file back
    // through the install prompt which rebuilds a v2 index.
    LOG_INF("BDF", "rejecting idx %s (magic=%08X version=%u, want version=%u) — reinstall required",
            idxPath, static_cast<unsigned>(hdr_.magic), static_cast<unsigned>(hdr_.version),
            static_cast<unsigned>(kBdfIndexVersion));
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

  // Phase 2.1: slurp the whole index into RAM for fonts that fit. Eliminates
  // the log2(N) SD seeks every cold glyph paid. Above the cap we stay on
  // disk — Unifont (57 K × 16 B ≈ 912 KB) has no hope of fitting.
  const size_t indexBytes = static_cast<size_t>(glyphCount_) * sizeof(IndexEntry);
  if (glyphCount_ > 0 && indexBytes <= kInMemoryIndexCapBytes) {
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    // Keep a 32 KB margin so the index alloc itself does not split the
    // block we want for a glyph slab / epub section ZIP dict.
    constexpr size_t kIndexAllocSafetyMargin = 32 * 1024;
    if (indexBytes + kIndexAllocSafetyMargin <= largest) {
      auto buf = std::unique_ptr<IndexEntry[]>(new (std::nothrow) IndexEntry[glyphCount_]);
      if (buf) {
        if (idxFile_.read(buf.get(), indexBytes) == static_cast<int>(indexBytes)) {
          indexArray_ = std::move(buf);
          // Close the idx file — every lookup from here on is pure RAM.
          idxFile_.close();
        }
      }
    }
  }

  return true;
}

void CustomFontGlyphSource::close() {
  if (idxOpen_) {
    // idxFile_ may already be closed if we slurped the index at open time.
    idxFile_.close();
    bdfFile_.close();
    idxOpen_ = false;
  }
  indexArray_.reset();
  glyphCount_ = 0;
  maxBitmapBytes_ = 0;
}

bool CustomFontGlyphSource::readIndexEntryFromFile(uint32_t indexPos, IndexEntry& out) {
  const size_t fileOffset = sizeof(IndexHeader) + indexPos * sizeof(IndexEntry);
  if (!idxFile_.seekSet(fileOffset)) return false;
  return idxFile_.read(&out, sizeof(out)) == static_cast<int>(sizeof(out));
}

const CustomFontGlyphSource::Glyph* CustomFontGlyphSource::lookup(uint32_t codepoint) {
  if (!idxOpen_ || glyphCount_ == 0 || !sharedCache_) return nullptr;

  const Glyph* cached = nullptr;
  if (sharedCache_->lookup(variantIndex_, codepoint, &cached)) {
    return cached;
  }

  // Binary search — either against the in-RAM array (zero I/O) or against
  // the .idx file (one seek + one read per step).
  uint32_t lo = 0;
  uint32_t hi = glyphCount_;
  IndexEntry hit{};
  bool found = false;
  if (indexArray_) {
    const IndexEntry* arr = indexArray_.get();
    while (lo < hi) {
      const uint32_t mid = lo + (hi - lo) / 2;
      const IndexEntry& e = arr[mid];
      if (e.codepoint == codepoint) {
        hit = e;
        found = true;
        break;
      }
      if (e.codepoint < codepoint) lo = mid + 1;
      else hi = mid;
    }
  } else {
    while (lo < hi) {
      const uint32_t mid = lo + (hi - lo) / 2;
      IndexEntry e{};
      if (!readIndexEntryFromFile(mid, e)) return nullptr;
      if (e.codepoint == codepoint) {
        hit = e;
        found = true;
        break;
      }
      if (e.codepoint < codepoint) lo = mid + 1;
      else hi = mid;
    }
  }
  if (!found) return nullptr;

  Glyph meta;
  meta.codepoint = hit.codepoint;
  meta.bbxW = hit.bitmapW;
  meta.bbxH = hit.bitmapH;
  meta.bbxOffX = hit.bbxOffX;
  meta.bbxOffY = hit.bbxOffY;
  meta.advance = hit.advance;
  meta.bitmapBytes = static_cast<size_t>((hit.bitmapW + 7) / 8) * static_cast<size_t>(hit.bitmapH);

  uint8_t* dst = sharedCache_->allocateSlot(variantIndex_, codepoint, meta, &cached);
  if (!dst) return nullptr;

  if (!decodeBitmap(hit, dst, sharedCache_->maxBitmapBytes())) {
    sharedCache_->abortSlot(variantIndex_, codepoint);
    return nullptr;
  }

  return cached;
}

bool CustomFontGlyphSource::decodeBitmap(const IndexEntry& e, uint8_t* dst, size_t dstCap) {
  const size_t bytesPerRow = (e.bitmapW + 7) / 8;
  const size_t totalBytes = bytesPerRow * e.bitmapH;
  if (totalBytes > dstCap) return false;  // Glyph exceeds font bounding box — rejected.
  if (totalBytes == 0) return true;        // zero-size glyph (e.g. SPACE) — empty bitmap is valid

  // v2 index points directly at the first hex row — no more BITMAP token
  // scan. bitmapOffset=0 means "no bitmap for this glyph" (malformed BDF).
  if (e.bitmapOffset == 0) return false;
  if (!bdfFile_.seekSet(e.bitmapOffset)) return false;

  std::memset(dst, 0, totalBytes);

  const size_t hexCharsPerRow = bytesPerRow * 2;
  // Phase 2.3 bulk read. Each row is hexCharsPerRow hex digits followed by
  // 1 or 2 newline bytes (\n or \r\n). Worst case: 2 newline bytes per row.
  // +16 slack for a final EOF case. Stack buffer; cap enforces a sensible
  // per-glyph limit (any font above this already failed under the old
  // per-row path).
  constexpr size_t kBulkBufSize = 4096;
  const size_t bulkBytes = hexCharsPerRow * e.bitmapH + 2 * e.bitmapH + 16;
  if (bulkBytes > kBulkBufSize) {
    static bool warnedBulk = false;
    if (!warnedBulk) {
      warnedBulk = true;
      LOG_INF("BDF", "glyph bitmap too large for bulk decode (%u B > %u) — glyph skipped",
              static_cast<unsigned>(bulkBytes), static_cast<unsigned>(kBulkBufSize));
    }
    return false;
  }
  char bulk[kBulkBufSize];
  const int got = bdfFile_.read(bulk, bulkBytes);
  if (got <= 0) return false;

  // Hex-parse in RAM. Walk the buffer skipping \n / \r; collect nibbles in
  // pairs. After `totalBytes * 2` nibbles we stop — any trailing whitespace
  // or ENDCHAR token is ignored.
  size_t nibbleCount = 0;
  const size_t nibblesNeeded = totalBytes * 2;
  uint8_t pending = 0;
  for (int i = 0; i < got && nibbleCount < nibblesNeeded; ++i) {
    const unsigned char c = static_cast<unsigned char>(bulk[i]);
    if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
    const int nib = hexNibble(c);
    if (nib < 0) return false;  // unexpected char before we finished the bitmap
    if ((nibbleCount & 1) == 0) {
      pending = static_cast<uint8_t>(nib << 4);
    } else {
      dst[nibbleCount / 2] = pending | static_cast<uint8_t>(nib);
    }
    ++nibbleCount;
  }
  return nibbleCount == nibblesNeeded;
}

}  // namespace bdf
}  // namespace crosspoint
