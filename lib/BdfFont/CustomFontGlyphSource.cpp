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

  return true;
}

void CustomFontGlyphSource::close() {
  if (idxOpen_) {
    idxFile_.close();
    bdfFile_.close();
    idxOpen_ = false;
  }

  glyphCount_ = 0;
  maxBitmapBytes_ = 0;
}



bool CustomFontGlyphSource::readIndexEntry(uint32_t indexPos, IndexEntry& out) {
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
  if (!bdfFile_.seekSet(e.bdfOffset)) return false;

  const size_t bytesPerRow = (e.bitmapW + 7) / 8;
  const size_t totalBytes = bytesPerRow * e.bitmapH;
  if (totalBytes > dstCap) return false;  // Glyph exceeds font bounding box — rejected.
  if (totalBytes == 0) return true;        // zero-size glyph (e.g. SPACE) — empty bitmap is valid
  std::memset(dst, 0, totalBytes);

  char header[32];
  // Walk lines until BITMAP token.
  bool sawBitmap = false;
  for (int safety = 0; safety < 64; ++safety) {
    const int n = readBitmapLine(bdfFile_, header, sizeof(header));
    if (n < 0) return false;
    if (std::strncmp(header, "BITMAP", 6) == 0 && (header[6] == '\0' || header[6] == ' ' || header[6] == '\t')) {
      sawBitmap = true;
      break;
    }
  }
  if (!sawBitmap) return false;

  const size_t hexCharsPerRow = bytesPerRow * 2;
  char hexBuf[128];
  if (hexCharsPerRow > sizeof(hexBuf)) {
    // Glyph wider than 512 px — one-shot log so we notice if a real font
    // hits this limit. Silently returning false otherwise looked like a
    // missing-glyph mystery during Phase 2b testing.
    static bool warned = false;
    if (!warned) {
      warned = true;
      LOG_INF("BDF", "glyph hex row too wide (%u bytes > %u buf) — glyph skipped",
              static_cast<unsigned>(hexCharsPerRow), static_cast<unsigned>(sizeof(hexBuf)));
    }
    return false;
  }

  for (size_t row = 0; row < e.bitmapH; ++row) {
    if (bdfFile_.read(hexBuf, hexCharsPerRow) != static_cast<int>(hexCharsPerRow)) return false;
    for (size_t b = 0; b < bytesPerRow; ++b) {
      const int hi = hexNibble(static_cast<unsigned char>(hexBuf[b * 2]));
      const int lo = hexNibble(static_cast<unsigned char>(hexBuf[b * 2 + 1]));
      if (hi < 0 || lo < 0) return false;
      dst[row * bytesPerRow + b] = static_cast<uint8_t>((hi << 4) | lo);
    }
    // drain until newline
    while (true) {
      int c = bdfFile_.read();
      if (c < 0 || c == '\n') break;
    }
  }
  return true;
}

}  // namespace bdf
}  // namespace crosspoint
