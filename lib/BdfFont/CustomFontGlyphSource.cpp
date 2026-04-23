#include "CustomFontGlyphSource.h"

#include <Arduino.h>
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

  idxOpen_ = true;
  cacheList_.clear();
  cacheMap_.clear();
  return true;
}

void CustomFontGlyphSource::close() {
  if (idxOpen_) {
    idxFile_.close();
    bdfFile_.close();
    idxOpen_ = false;
  }
  cacheList_.clear();
  cacheMap_.clear();
}

void CustomFontGlyphSource::setCacheCap(size_t slots) {
  if (slots == 0) slots = 1;
  cacheCap_ = slots;
  while (cacheList_.size() > cacheCap_) {
    auto& slot = cacheList_.back();
    cacheMap_.erase(slot.glyph.codepoint);
    cacheList_.pop_back();
  }
}

bool CustomFontGlyphSource::readIndexEntry(uint32_t indexPos, IndexEntry& out) {
  const size_t fileOffset = sizeof(IndexHeader) + indexPos * sizeof(IndexEntry);
  if (!idxFile_.seekSet(fileOffset)) return false;
  return idxFile_.read(&out, sizeof(out)) == static_cast<int>(sizeof(out));
}

const CustomFontGlyphSource::Glyph* CustomFontGlyphSource::lookup(uint32_t codepoint) {
  if (!idxOpen_ || glyphCount_ == 0) return nullptr;

  // Cache hit: promote to MRU and return borrowed pointer into the slot.
  const auto mapIt = cacheMap_.find(codepoint);
  if (mapIt != cacheMap_.end()) {
    if (mapIt->second != cacheList_.begin()) {
      cacheList_.splice(cacheList_.begin(), cacheList_, mapIt->second);
    }
    auto& slot = cacheList_.front();
    slot.glyph.bitmap = slot.bitmap.data();
    slot.glyph.bitmapBytes = slot.bitmap.size();
    return &slot.glyph;
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

  std::vector<uint8_t> bitmap;
  if (!decodeBitmap(hit, bitmap)) return nullptr;

  // Evict oldest if at cap (check BEFORE insert to keep map iters stable).
  if (cacheList_.size() >= cacheCap_) {
    auto& evict = cacheList_.back();
    cacheMap_.erase(evict.glyph.codepoint);
    cacheList_.pop_back();
  }

  cacheList_.emplace_front();
  auto& inserted = cacheList_.front();
  inserted.bitmap = std::move(bitmap);
  inserted.glyph.codepoint = hit.codepoint;
  inserted.glyph.bbxW = hit.bitmapW;
  inserted.glyph.bbxH = hit.bitmapH;
  inserted.glyph.bbxOffX = hit.bbxOffX;
  inserted.glyph.bbxOffY = hit.bbxOffY;
  inserted.glyph.advance = hit.advance;
  inserted.glyph.bitmap = inserted.bitmap.data();
  inserted.glyph.bitmapBytes = inserted.bitmap.size();
  cacheMap_[codepoint] = cacheList_.begin();
  return &inserted.glyph;
}

bool CustomFontGlyphSource::decodeBitmap(const IndexEntry& e, std::vector<uint8_t>& out) {
  // Seek to the STARTCHAR line for this glyph. Then walk lines until BITMAP,
  // then collect H rows of hex into out.
  if (!bdfFile_.seekSet(e.bdfOffset)) return false;

  const size_t bytesPerRow = (e.bitmapW + 7) / 8;
  const size_t totalBytes = bytesPerRow * e.bitmapH;
  out.assign(totalBytes, 0);
  if (totalBytes == 0) return true;  // zero-size glyph (e.g. SPACE) — empty bitmap is valid

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
      out[row * bytesPerRow + b] = static_cast<uint8_t>((hi << 4) | lo);
    }
  }
  return true;
}

}  // namespace bdf
}  // namespace crosspoint
