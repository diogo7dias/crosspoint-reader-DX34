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

// Reads exactly `count` bytes into `dst`. Short reads return false.
// Resets the task watchdog every ~8 KB so a slow SD on a big (~256 KB)
// font file can't trip the 10-second WDT when activate() opens four
// variants back-to-back at reader-open time.
bool readExact(HalFile& f, void* dst, size_t count) {
  constexpr size_t kWdtEveryBytes = 8192;
  size_t got = 0;
  size_t sinceWdt = 0;
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

// Validate a Header filled from disk. Leaves `error` untouched on
// success. Used by both openFromFile and validateFile so the
// criteria match in both call sites.
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
  if (h.sizePt < 9 || h.sizePt > 16) {
    if (error) *error = "size outside 9..16";
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

  // Cross-check that header + tables + blob all fit in the file.
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
  if (buffer_) {
    free(buffer_);
    buffer_ = nullptr;
  }
  fileBytes_ = 0;
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

  // Read the header first so we can reject garbage before allocating
  // the full buffer. 32 bytes is trivial.
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

  // One contiguous allocation for the whole file. The EpdFontData
  // view below just pokes pointers into it.
  uint8_t* buf = static_cast<uint8_t*>(malloc(fileSize));
  if (!buf) {
    lastError_ = "malloc failed";
    f.close();
    return false;
  }
  std::memcpy(buf, &hdr, sizeof(hdr));
  if (!readExact(f, buf + sizeof(hdr), fileSize - sizeof(hdr))) {
    lastError_ = "body read failed";
    free(buf);
    f.close();
    return false;
  }
  f.close();

  // Wire up the in-memory view. Layout is: header, glyphs, intervals,
  // groups, bitmap blob. The header is already copied into header_ so
  // we keep a stable copy for the accessors.
  buffer_ = buf;
  fileBytes_ = static_cast<uint32_t>(fileSize);
  header_ = hdr;

  const uint8_t* glyphsPtr = buf + sizeof(Header);
  const uint8_t* intervalsPtr = glyphsPtr + hdr.glyphCount * sizeof(EpdGlyph);
  const uint8_t* groupsPtr = intervalsPtr + hdr.intervalCount * sizeof(EpdUnicodeInterval);
  const uint8_t* bitmapPtr = groupsPtr + hdr.groupCount * sizeof(EpdFontGroup);

  fontData_ = {};
  fontData_.bitmap = bitmapPtr;
  fontData_.glyph = reinterpret_cast<const EpdGlyph*>(glyphsPtr);
  fontData_.intervals = reinterpret_cast<const EpdUnicodeInterval*>(intervalsPtr);
  fontData_.intervalCount = hdr.intervalCount;
  fontData_.advanceY = static_cast<uint8_t>(hdr.advanceY > 255 ? 255 : hdr.advanceY);
  fontData_.ascender = hdr.ascent;
  fontData_.descender = hdr.descent;
  fontData_.is2Bit = (hdr.bitsPerPixel == 2);
  fontData_.groups = reinterpret_cast<const EpdFontGroup*>(groupsPtr);
  fontData_.groupCount = static_cast<uint16_t>(hdr.groupCount);
  // No kerning / ligature tables in v1 — leave nullptr (zero-initialised).

  // Smoke-test group 0: confirm the DEFLATE stream actually inflates.
  // Catches files produced by a baker that wrote zlib-wrapped output
  // instead of raw deflate — without this, a broken font activates
  // "successfully" and then floods the heap with failed group-decode
  // mallocs every time the renderer touches a glyph, which fragments
  // the heap until layout itself can't allocate (observed with the
  // pre-deflateRaw browser baker).
  const auto* g0 = reinterpret_cast<const EpdFontGroup*>(groupsPtr);
  if (g0->compressedSize > 0 && g0->uncompressedSize > 0 &&
      g0->compressedSize <= hdr.bitmapBlobSize) {
    auto* probe = static_cast<uint8_t*>(malloc(g0->uncompressedSize));
    if (!probe) {
      lastError_ = "probe alloc failed";
      release();
      return false;
    }
    InflateReader rd;
    rd.init(false);
    rd.setSource(bitmapPtr + g0->compressedOffset, g0->compressedSize);
    const bool ok = rd.read(probe, g0->uncompressedSize);
    free(probe);
    if (!ok) {
      lastError_ = "group 0 inflate failed (likely zlib-wrapped, want raw deflate)";
      release();
      return false;
    }
  }

  LOG_DBG(kModule, "loaded %s: %u glyphs / %u groups / %u B bitmap (%u B total)", path.c_str(),
          static_cast<unsigned>(hdr.glyphCount), static_cast<unsigned>(hdr.groupCount),
          static_cast<unsigned>(hdr.bitmapBlobSize), static_cast<unsigned>(fileSize));
  return true;
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
