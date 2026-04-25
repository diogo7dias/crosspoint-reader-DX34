#include "EpdBinFontLoader.h"

#include <HalStorage.h>
#include <InflateReader.h>
#include <Logging.h>
#include <esp_task_wdt.h>

#include <cstdlib>
#include <cstring>

namespace crosspoint {
namespace binfont {

namespace {
constexpr const char* kModule = "CPBN";
constexpr uint8_t kMinSizePt = 25;
constexpr uint8_t kMaxSizePt = 40;

bool readExact(HalFile& f, void* dst, size_t count) {
  constexpr size_t kWdtEveryBytes = 8192;
  size_t got = 0, sinceWdt = 0;
  auto* out = static_cast<uint8_t*>(dst);
  while (got < count) {
    const int n = f.read(out + got, count - got);
    if (n <= 0) return false;
    got += static_cast<size_t>(n);
    sinceWdt += static_cast<size_t>(n);
    if (sinceWdt >= kWdtEveryBytes) {
      esp_task_wdt_reset();
      sinceWdt = 0;
    }
  }
  return true;
}

bool validateHeader(const Header& h, uint32_t fileBytes, std::string* error) {
  if (h.magic != kMagic) {
    if (error) *error = "bad magic (not CPBN)";
    return false;
  }
  if (h.version != kVersion) {
    if (error) *error = "unsupported version";
    return false;
  }
  if (h.bitsPerPixel != 2) {
    if (error) *error = "only 2 bits/pixel supported";
    return false;
  }
  if (h.sizePt < kMinSizePt || h.sizePt > kMaxSizePt) {
    if (error) *error = "size outside 25..40";
    return false;
  }
  if (h.variant > kVariantBoldItalic) {
    if (error) *error = "bad variant";
    return false;
  }
  if (h.glyphCount == 0 || h.glyphCount > kMaxGlyphs) {
    if (error) *error = "bad glyphCount";
    return false;
  }
  if (h.intervalCount == 0) {
    if (error) *error = "no unicode intervals";
    return false;
  }
  if (h.groupCount == 0 || h.groupCount > kMaxGroups) {
    if (error) *error = "bad groupCount";
    return false;
  }

  const uint64_t glyphsBytes = static_cast<uint64_t>(h.glyphCount) * sizeof(EpdGlyph);
  const uint64_t intervalsBytes = static_cast<uint64_t>(h.intervalCount) * sizeof(EpdUnicodeInterval);
  const uint64_t groupsBytes = static_cast<uint64_t>(h.groupCount) * sizeof(EpdFontGroup);
  const uint64_t expected = sizeof(Header) + glyphsBytes + intervalsBytes + groupsBytes + h.bitmapBlobSize;
  if (expected != fileBytes) {
    if (error) *error = "tables + blob do not match file size";
    return false;
  }
  return true;
}

}  // namespace

EpdBinFontLoader::~EpdBinFontLoader() { release(); }

void EpdBinFontLoader::release() {
  if (tablesBuf_) {
    free(tablesBuf_);
    tablesBuf_ = nullptr;
  }
  tablesBytes_ = 0;
  if (file_.isOpen()) file_.close();
  bitmapBlobFileOffset_ = 0;
  fontData_ = {};
  header_ = {};
}

bool EpdBinFontLoader::openFromFile(const std::string& path) {
  release();

  HalFile f;
  if (!Storage.openFileForRead(kModule, path, f)) {
    lastError_ = "open failed";
    return false;
  }
  const size_t fileSize = f.size();
  if (fileSize < sizeof(Header) || fileSize > kMaxFileBytes) {
    lastError_ = "file too small or too large";
    f.close();
    return false;
  }

  Header hdr;
  if (!readExact(f, &hdr, sizeof(hdr))) {
    lastError_ = "header read failed";
    f.close();
    return false;
  }
  if (!validateHeader(hdr, static_cast<uint32_t>(fileSize), &lastError_)) {
    f.close();
    return false;
  }

  // Allocate just the tables: header + glyph metadata + intervals +
  // groups. The bitmap blob stays on SD.
  const uint32_t tablesBytes = sizeof(Header) + hdr.glyphCount * sizeof(EpdGlyph) +
                               hdr.intervalCount * sizeof(EpdUnicodeInterval) + hdr.groupCount * sizeof(EpdFontGroup);
  uint8_t* buf = static_cast<uint8_t*>(malloc(tablesBytes));
  if (!buf) {
    lastError_ = "tables malloc failed";
    f.close();
    return false;
  }
  std::memcpy(buf, &hdr, sizeof(hdr));
  if (!readExact(f, buf + sizeof(hdr), tablesBytes - sizeof(hdr))) {
    lastError_ = "tables read failed";
    free(buf);
    f.close();
    return false;
  }

  // Smoke-test group 0: pull its compressed bytes from the file and
  // inflate. Catches a malformed or zlib-wrapped bitmap blob before
  // we hand the font to the renderer.
  const auto* groupsPtr = reinterpret_cast<const EpdFontGroup*>(
      buf + sizeof(Header) + hdr.glyphCount * sizeof(EpdGlyph) + hdr.intervalCount * sizeof(EpdUnicodeInterval));
  const auto& g0 = groupsPtr[0];
  if (g0.compressedSize > 0 && g0.uncompressedSize > 0 && g0.compressedSize <= hdr.bitmapBlobSize) {
    auto* compressed = static_cast<uint8_t*>(malloc(g0.compressedSize));
    auto* probe = static_cast<uint8_t*>(malloc(g0.uncompressedSize));
    if (!compressed || !probe) {
      free(compressed);
      free(probe);
      free(buf);
      f.close();
      lastError_ = "probe alloc failed";
      return false;
    }
    if (!f.seekSet(tablesBytes + g0.compressedOffset) || !readExact(f, compressed, g0.compressedSize)) {
      free(compressed);
      free(probe);
      free(buf);
      f.close();
      lastError_ = "probe SD read failed";
      return false;
    }
    InflateReader rd;
    rd.init(false);
    rd.setSource(compressed, g0.compressedSize);
    const bool ok = rd.read(probe, g0.uncompressedSize);
    free(compressed);
    free(probe);
    if (!ok) {
      free(buf);
      f.close();
      lastError_ = "group 0 inflate failed (likely zlib-wrapped, want raw deflate)";
      return false;
    }
  }

  // Commit: take ownership of the file handle for the rest of the
  // loader's life so readBitmapBytesCallback can pread without a
  // re-open per request.
  tablesBuf_ = buf;
  tablesBytes_ = tablesBytes;
  file_ = std::move(f);
  bitmapBlobFileOffset_ = tablesBytes;
  header_ = hdr;

  const uint8_t* glyphsPtr = buf + sizeof(Header);
  const uint8_t* intervalsPtr = glyphsPtr + hdr.glyphCount * sizeof(EpdGlyph);
  const uint8_t* groupsBytes = intervalsPtr + hdr.intervalCount * sizeof(EpdUnicodeInterval);

  fontData_ = {};
  // bitmap stays null — readBitmapBytes is the source of truth.
  fontData_.glyph = reinterpret_cast<const EpdGlyph*>(glyphsPtr);
  fontData_.intervals = reinterpret_cast<const EpdUnicodeInterval*>(intervalsPtr);
  fontData_.intervalCount = hdr.intervalCount;
  fontData_.advanceY = static_cast<uint8_t>(hdr.advanceY > 255 ? 255 : hdr.advanceY);
  fontData_.ascender = hdr.ascent;
  fontData_.descender = hdr.descent;
  fontData_.is2Bit = (hdr.bitsPerPixel == 2);
  fontData_.groups = reinterpret_cast<const EpdFontGroup*>(groupsBytes);
  fontData_.groupCount = static_cast<uint16_t>(hdr.groupCount);
  fontData_.bitmapCtx = this;
  fontData_.readBitmapBytes = &EpdBinFontLoader::readBitmapBytesCallback;

  LOG_DBG(kModule, "streamed open %s: %u glyphs / %u groups / tables %u B (blob stays on SD)", path.c_str(),
          static_cast<unsigned>(hdr.glyphCount), static_cast<unsigned>(hdr.groupCount),
          static_cast<unsigned>(tablesBytes));
  return true;
}

// static
int EpdBinFontLoader::readBitmapBytesCallback(void* ctx, uint32_t offset, uint8_t* dst, size_t len) {
  auto* self = static_cast<EpdBinFontLoader*>(ctx);
  if (!self || !self->file_.isOpen()) return -1;
  if (!self->file_.seekSet(self->bitmapBlobFileOffset_ + offset)) return -1;
  size_t got = 0;
  while (got < len) {
    const int n = self->file_.read(dst + got, len - got);
    if (n <= 0) return -1;
    got += static_cast<size_t>(n);
  }
  return static_cast<int>(got);
}

// static
bool EpdBinFontLoader::validateFile(const std::string& path, std::string* error) {
  HalFile f;
  if (!Storage.openFileForRead(kModule, path, f)) {
    if (error) *error = "open failed";
    return false;
  }
  const size_t fileSize = f.size();
  if (fileSize < sizeof(Header) || fileSize > kMaxFileBytes) {
    if (error) *error = "file too small or too large";
    f.close();
    return false;
  }
  Header hdr;
  if (!readExact(f, &hdr, sizeof(hdr))) {
    if (error) *error = "header read failed";
    f.close();
    return false;
  }
  f.close();
  return validateHeader(hdr, static_cast<uint32_t>(fileSize), error);
}

}  // namespace binfont
}  // namespace crosspoint
