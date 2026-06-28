// Host-side tests for the CPBN (CrossPoint BiNary font) blob format validator.
//
// This is the daily-driver safety gate for SD-backed fonts: before the renderer
// trusts a `.bin` blob on the SD card, validateBlob() must confirm the header is
// well-formed AND that the blob matches the firmware that was compiled against it
// (glyph/interval/group counts) and is bit-intact (CRC32). Any mismatch must be
// rejected so the caller can fall back to a flash-resident font rather than render
// garbage or read out of bounds.
//
// Tier 1 layout (tables stay in flash, only the bitmap blob lives on SD):
//   [BlobHeader (packed)] [bitmapBlob (bitmapBlobSize bytes)]
//
// Run via: pio test -e test_bin_font_format
#include <unity.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "EpdBinExport.h"
#include "EpdBinFontLoader.h"
#include "EpdBinFormat.h"
#include "EpdFont.h"
#include "EpdFontData.h"
#include "SdFontManager.h"

using namespace crosspoint::binfont;

namespace {
// In-memory BlobSource standing in for the device HalFile adapter, so the loader
// logic (header validation, streamed CRC, the readBitmapBytes trampoline) is
// exercised on the host with no HAL.
class MemoryBlobSource : public BlobSource {
 public:
  explicit MemoryBlobSource(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}
  int read(uint32_t offset, uint8_t* dst, size_t len) override {
    if (static_cast<uint64_t>(offset) + len > bytes_.size()) return -1;
    std::memcpy(dst, bytes_.data() + offset, len);
    return static_cast<int>(len);
  }
  uint32_t size() const override { return static_cast<uint32_t>(bytes_.size()); }

 private:
  std::vector<uint8_t> bytes_;
};
}  // namespace

namespace {

// Builds a self-consistent CPBN blob buffer: a valid header for `blob`, with the
// CRC32 filled in, followed by the blob bytes. Counts default to a fixed triple
// the tests also use as the matching expectation.
std::vector<uint8_t> makeBlobBuffer(const std::vector<uint8_t>& blob, uint32_t glyphCount = 12,
                                    uint32_t intervalCount = 3, uint32_t groupCount = 2) {
  BlobHeader h{};
  h.magic = kMagic;
  h.version = kBlobFormatVersion;
  h.bitsPerPixel = kBitsPerPixel;
  h.flags = 0;  // Tier 1: tables in flash, not in file
  h.variant = 0;
  h.sizePt = 14;
  h.advanceY = 18;
  h.ascent = 14;
  h.descent = -4;
  h.reserved0 = 0;
  h.glyphCount = glyphCount;
  h.intervalCount = intervalCount;
  h.groupCount = groupCount;
  h.bitmapBlobSize = static_cast<uint32_t>(blob.size());
  h.blobCrc32 = crc32(blob.data(), blob.size());

  std::vector<uint8_t> buf(sizeof(BlobHeader) + blob.size());
  std::memcpy(buf.data(), &h, sizeof(BlobHeader));
  std::memcpy(buf.data() + sizeof(BlobHeader), blob.data(), blob.size());
  return buf;
}

const FontBlobExpectation kExpect{12, 3, 2};

}  // namespace

void setUp() {}
void tearDown() {}

void test_accepts_well_formed_blob() {
  const std::vector<uint8_t> blob{0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03};
  const std::vector<uint8_t> buf = makeBlobBuffer(blob);

  ParsedBlob out{};
  const BlobReject r = validateBlob(buf.data(), buf.size(), kExpect, &out);

  TEST_ASSERT_EQUAL_INT(kOk, r);
  TEST_ASSERT_EQUAL_UINT32(sizeof(BlobHeader), out.blobOffset);
  TEST_ASSERT_EQUAL_UINT32(blob.size(), out.blobSize);
}

void test_accepts_empty_blob() {
  // A zero-length blob (degenerate but valid) must still pass: CRC of nothing is
  // defined, and blobSize 0 fits any buffer >= header.
  const std::vector<uint8_t> buf = makeBlobBuffer({});
  ParsedBlob out{0xFFFFFFFF, 0xFFFFFFFF};
  TEST_ASSERT_EQUAL_INT(kOk, validateBlob(buf.data(), buf.size(), kExpect, &out));
  TEST_ASSERT_EQUAL_UINT32(0u, out.blobSize);
}

void test_rejects_null_or_truncated_buffer() {
  const std::vector<uint8_t> buf = makeBlobBuffer({0x11, 0x22});
  ParsedBlob out{};
  TEST_ASSERT_EQUAL_INT(kTooSmall, validateBlob(nullptr, 100, kExpect, &out));
  // One byte short of a full header.
  TEST_ASSERT_EQUAL_INT(kTooSmall, validateBlob(buf.data(), sizeof(BlobHeader) - 1, kExpect, &out));
}

void test_rejects_bad_magic() {
  std::vector<uint8_t> buf = makeBlobBuffer({0x11, 0x22, 0x33});
  buf[0] ^= 0xFF;  // corrupt the magic
  ParsedBlob out{};
  TEST_ASSERT_EQUAL_INT(kBadMagic, validateBlob(buf.data(), buf.size(), kExpect, &out));
}

void test_rejects_unsupported_version() {
  std::vector<uint8_t> buf = makeBlobBuffer({0x11, 0x22, 0x33});
  buf[offsetof(BlobHeader, version)] = kBlobFormatVersion + 1;
  ParsedBlob out{};
  TEST_ASSERT_EQUAL_INT(kBadVersion, validateBlob(buf.data(), buf.size(), kExpect, &out));
}

void test_rejects_wrong_bits_per_pixel() {
  std::vector<uint8_t> buf = makeBlobBuffer({0x11, 0x22, 0x33});
  buf[offsetof(BlobHeader, bitsPerPixel)] = 1;  // 1-bit not supported by this path
  ParsedBlob out{};
  TEST_ASSERT_EQUAL_INT(kBadBitsPerPix, validateBlob(buf.data(), buf.size(), kExpect, &out));
}

void test_rejects_blob_size_overrun() {
  // Header claims a bigger blob than the buffer actually contains -> must not
  // hand back an out-of-bounds region.
  std::vector<uint8_t> buf = makeBlobBuffer({0x11, 0x22, 0x33});
  const uint32_t lie = 1000000u;
  std::memcpy(buf.data() + offsetof(BlobHeader, bitmapBlobSize), &lie, sizeof(lie));
  ParsedBlob out{};
  TEST_ASSERT_EQUAL_INT(kSizeOverrun, validateBlob(buf.data(), buf.size(), kExpect, &out));
}

void test_rejects_count_mismatch_stale_card() {
  // SD blob baked for a different firmware (glyph count moved) -> reject so we
  // never index flash glyph tables with offsets from a mismatched blob.
  std::vector<uint8_t> buf = makeBlobBuffer({0x11, 0x22, 0x33}, /*glyphCount=*/13);
  ParsedBlob out{};
  TEST_ASSERT_EQUAL_INT(kCountMismatch, validateBlob(buf.data(), buf.size(), kExpect, &out));
}

void test_rejects_corrupt_blob_bytes() {
  std::vector<uint8_t> buf = makeBlobBuffer({0x11, 0x22, 0x33, 0x44});
  buf[sizeof(BlobHeader) + 1] ^= 0x80;  // flip a bit in the blob, CRC no longer matches
  ParsedBlob out{};
  TEST_ASSERT_EQUAL_INT(kBadCrc, validateBlob(buf.data(), buf.size(), kExpect, &out));
}

void test_crc32_matches_zlib_check_vector() {
  // "123456789" -> 0xCBF43926 is the canonical CRC-32 check value, identical to
  // Python zlib.crc32. Pins the serializer<->validator integrity contract.
  const char* s = "123456789";
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, crc32(reinterpret_cast<const uint8_t*>(s), 9));
}

// ---- EpdBinFontLoader (SD blob loader, Tier 1) ----

void test_loader_opens_valid_blob_and_streams_bytes() {
  // A blob long enough to cross the loader's internal streaming-CRC chunk so the
  // chunk loop is genuinely exercised, with a recognisable pattern to read back.
  std::vector<uint8_t> blob(1000);
  for (size_t i = 0; i < blob.size(); ++i) blob[i] = static_cast<uint8_t>((i * 7 + 3) & 0xFF);
  MemoryBlobSource src(makeBlobBuffer(blob));

  EpdBinFontLoader loader;
  TEST_ASSERT_EQUAL_INT(kOk, loader.open(&src, kExpect));
  TEST_ASSERT_TRUE(loader.isOpen());
  TEST_ASSERT_EQUAL_UINT32(sizeof(BlobHeader), loader.blobOffset());
  TEST_ASSERT_EQUAL_UINT32(blob.size(), loader.blobSize());

  // The trampoline reads bitmap bytes by offset RELATIVE to the blob start,
  // exactly as FontDecompressor invokes EpdFontData::readBitmapBytes.
  uint8_t got[16] = {};
  const int n = EpdBinFontLoader::readBitmapTrampoline(loader.bitmapCtx(), 500, got, sizeof(got));
  TEST_ASSERT_EQUAL_INT(sizeof(got), n);
  for (size_t i = 0; i < sizeof(got); ++i) {
    TEST_ASSERT_EQUAL_UINT8(blob[500 + i], got[i]);
  }
}

void test_loader_trampoline_rejects_out_of_range_read() {
  std::vector<uint8_t> blob{1, 2, 3, 4, 5};
  MemoryBlobSource src(makeBlobBuffer(blob));
  EpdBinFontLoader loader;
  TEST_ASSERT_EQUAL_INT(kOk, loader.open(&src, kExpect));
  uint8_t got[8] = {};
  // Reading past the end of the blob must fail, not read into neighbouring data.
  TEST_ASSERT_NOT_EQUAL(static_cast<int>(sizeof(got)),
                        EpdBinFontLoader::readBitmapTrampoline(loader.bitmapCtx(), 2, got, sizeof(got)));
}

void test_loader_rejects_bad_magic() {
  std::vector<uint8_t> buf = makeBlobBuffer({9, 8, 7});
  buf[0] ^= 0xFF;
  MemoryBlobSource src(buf);
  EpdBinFontLoader loader;
  TEST_ASSERT_EQUAL_INT(kBadMagic, loader.open(&src, kExpect));
  TEST_ASSERT_FALSE(loader.isOpen());
}

void test_loader_rejects_count_mismatch() {
  MemoryBlobSource src(makeBlobBuffer({9, 8, 7}, /*glyphCount=*/99));
  EpdBinFontLoader loader;
  TEST_ASSERT_EQUAL_INT(kCountMismatch, loader.open(&src, kExpect));
}

void test_loader_rejects_corrupt_blob_via_streamed_crc() {
  std::vector<uint8_t> buf = makeBlobBuffer({9, 8, 7, 6});
  buf[sizeof(BlobHeader) + 2] ^= 0x40;  // flip a blob bit; header CRC field unchanged
  MemoryBlobSource src(buf);
  EpdBinFontLoader loader;
  TEST_ASSERT_EQUAL_INT(kBadCrc, loader.open(&src, kExpect));
  TEST_ASSERT_FALSE(loader.isOpen());
}

void test_loader_rejects_truncated_file() {
  std::vector<uint8_t> buf = makeBlobBuffer({9, 8, 7, 6, 5});
  buf.resize(sizeof(BlobHeader) + 2);  // header says 5 blob bytes, only 2 present
  MemoryBlobSource src(buf);
  EpdBinFontLoader loader;
  TEST_ASSERT_EQUAL_INT(kSizeOverrun, loader.open(&src, kExpect));
}

// ---- EpdBinExport (serialize an in-flash EpdFontData blob to CPBN, for the
//      on-device "Export font packs to SD" bootstrap) ----

namespace {
// Collects exported bytes in memory so the test can both validate the CPBN file
// and feed it straight back through the loader (export -> consume roundtrip).
class MemorySink : public ByteSink {
 public:
  bool write(const uint8_t* data, size_t len) override {
    bytes_.insert(bytes_.end(), data, data + len);
    return true;
  }
  const std::vector<uint8_t>& bytes() const { return bytes_; }

 private:
  std::vector<uint8_t> bytes_;
};

// A minimal compressed-font EpdFontData: a 20-byte "bitmap" reachable as a single
// group, covering one 5-codepoint interval. Only the fields the exporter reads
// are populated.
EpdFontData makeSyntheticFont(const uint8_t* bitmap, const EpdFontGroup* groups, const EpdUnicodeInterval* intervals) {
  EpdFontData fd{};
  fd.bitmap = bitmap;
  fd.groups = groups;
  fd.groupCount = 1;
  fd.intervals = intervals;
  fd.intervalCount = 1;
  fd.advanceY = 18;
  fd.ascender = 14;
  fd.descender = -4;
  fd.is2Bit = true;
  return fd;
}
}  // namespace

void test_export_then_validate_and_consume_roundtrips() {
  uint8_t bitmap[20];
  for (int i = 0; i < 20; ++i) bitmap[i] = static_cast<uint8_t>((i * 11 + 5) & 0xFF);
  const EpdFontGroup groups[1] = {{/*compressedOffset=*/0, /*compressedSize=*/20, /*uncompressedSize=*/40,
                                   /*glyphCount=*/5, /*firstGlyphIndex=*/0}};
  const EpdUnicodeInterval intervals[1] = {{/*first=*/32, /*last=*/36, /*offset=*/0}};  // 5 glyphs
  const EpdFontData fd = makeSyntheticFont(bitmap, groups, intervals);

  MemorySink sink;
  TEST_ASSERT_TRUE(exportFontBlob(fd, /*variant=*/0, /*sizePt=*/14, sink));

  // The exported bytes form a valid CPBN file matching this font's tables...
  const FontBlobExpectation expect{5, 1, 1};
  ParsedBlob parsed{};
  TEST_ASSERT_EQUAL_INT(kOk, validateBlob(sink.bytes().data(), sink.bytes().size(), expect, &parsed));
  TEST_ASSERT_EQUAL_UINT32(20u, parsed.blobSize);
  // ...and the blob payload is the font's own bitmap bytes, verbatim.
  TEST_ASSERT_EQUAL_UINT8_ARRAY(bitmap, sink.bytes().data() + parsed.blobOffset, 20);

  // Full loop: feed the exported file back through the SD loader.
  MemoryBlobSource src(sink.bytes());
  EpdBinFontLoader loader;
  TEST_ASSERT_EQUAL_INT(kOk, loader.open(&src, expect));
  uint8_t got[20] = {};
  TEST_ASSERT_EQUAL_INT(20, EpdBinFontLoader::readBitmapTrampoline(loader.bitmapCtx(), 0, got, 20));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(bitmap, got, 20);
}

void test_export_blob_size_spans_all_groups() {
  // Blob length must be the max group end, not just group 0, so multi-group fonts
  // export their whole bitmap.
  uint8_t bitmap[50];
  for (int i = 0; i < 50; ++i) bitmap[i] = static_cast<uint8_t>(i);
  const EpdFontGroup groups[2] = {{0, 18, 30, 3, 0}, {18, 32, 60, 4, 3}};  // ends at 18+32 = 50
  const EpdUnicodeInterval intervals[1] = {{32, 38, 0}};  // 7 glyphs
  EpdFontData fd = makeSyntheticFont(bitmap, groups, intervals);
  fd.groupCount = 2;

  MemorySink sink;
  TEST_ASSERT_TRUE(exportFontBlob(fd, 1, 12, sink));
  const FontBlobExpectation expect{7, 1, 2};
  ParsedBlob parsed{};
  TEST_ASSERT_EQUAL_INT(kOk, validateBlob(sink.bytes().data(), sink.bytes().size(), expect, &parsed));
  TEST_ASSERT_EQUAL_UINT32(50u, parsed.blobSize);
}

// ---- SdFontManager (registry + active-set swap + export-all, HAL-free) ----
//
// The manager is the generalization of the single-font pilot to all reader-font
// families. It owns no HAL: an injected SdFontIo provides exists / export / open
// / release. Here a std::map-backed MemorySdFontIo lets the whole state machine
// (export-all-missing, activate-on-selection, restore-on-switch, fallback) run on
// the host. The manager repoints the live EpdFont objects' `data` pointer exactly
// as the renderer would see it.

namespace {

using crosspoint::fonts::SdFontIo;
using crosspoint::fonts::SdFontManager;

// In-memory SdFontIo: files live in a map keyed by the path the manager builds.
class MemorySdFontIo : public SdFontIo {
 public:
  std::map<std::string, std::vector<uint8_t>> files;
  int openCalls = 0;
  int exportCalls = 0;

  bool exists(const char* path) override { return files.count(path) != 0; }
  bool exportBlob(const char* path, const EpdFontData& flashData, uint16_t sizePt) override {
    ++exportCalls;
    MemorySink sink;
    if (!exportFontBlob(flashData, /*variant=*/0, sizePt, sink)) return false;
    files[path] = sink.bytes();
    return true;
  }
  BlobSource* openSource(const char* path) override {
    auto it = files.find(path);
    if (it == files.end()) return nullptr;
    ++openCalls;
    return new MemoryBlobSource(it->second);  // owned by the IO, freed in releaseSource
  }
  void releaseSource(BlobSource* src) override { delete src; }
};

// One offloadable variant the manager can drive: a flash EpdFontData (single
// group / 5-glyph interval) plus the live EpdFont the renderer dereferences.
struct TestVariant {
  uint8_t bitmap[20];
  EpdFontGroup groups[1];
  EpdUnicodeInterval intervals[1];
  EpdFontData flash{};
  EpdFont font{nullptr};

  // `seed` makes each variant's bitmap bytes distinct so a test can prove which
  // pack is actually being streamed. `hasFlashBitmap=false` models a slim build.
  explicit TestVariant(uint8_t seed, bool hasFlashBitmap = true) : font(nullptr) {
    for (int i = 0; i < 20; ++i) bitmap[i] = static_cast<uint8_t>(seed + i);
    groups[0] = EpdFontGroup{/*compressedOffset=*/0, /*compressedSize=*/20, /*uncompressedSize=*/40,
                             /*glyphCount=*/5, /*firstGlyphIndex=*/0};
    intervals[0] = EpdUnicodeInterval{/*first=*/32, /*last=*/36, /*offset=*/0};
    flash.bitmap = hasFlashBitmap ? bitmap : nullptr;
    flash.groups = groups;
    flash.groupCount = 1;
    flash.intervals = intervals;
    flash.intervalCount = 1;
    flash.advanceY = 18;
    flash.ascender = 14;
    flash.descender = -4;
    flash.is2Bit = true;
    font.data = &flash;
  }
};

// Reads `len` bytes at blob `offset` through whatever EpdFontData the font now
// points at (its readBitmapBytes trampoline), i.e. what the renderer would get.
int readThroughFont(const EpdFont& f, uint32_t offset, uint8_t* dst, size_t len) {
  return f.data->readBitmapBytes(const_cast<void*>(f.data->bitmapCtx), offset, dst, len);
}

constexpr int kFontA = 100;
constexpr int kFontB = 200;

}  // namespace

void test_manager_export_all_writes_missing_packs() {
  SdFontManager mgr;
  MemorySdFontIo io;
  mgr.setIo(&io);
  TestVariant a(0x10), b(0x40);
  mgr.registerFont(kFontA, 14, "famA", &a.font, nullptr, nullptr, nullptr, nullptr);
  mgr.registerFont(kFontB, 12, "famB", &b.font, nullptr, nullptr, nullptr, nullptr);

  mgr.exportAllMissing();

  // One pack per registered (family,size,weight), at the manager-built path.
  TEST_ASSERT_TRUE(io.exists("/fonts/famA_14_regular.bin"));
  TEST_ASSERT_TRUE(io.exists("/fonts/famB_12_regular.bin"));
  TEST_ASSERT_EQUAL_INT(2, io.exportCalls);
  // And each written pack is a valid CPBN file for its font.
  const FontBlobExpectation expect{5, 1, 1};
  ParsedBlob parsed{};
  const auto& bytes = io.files["/fonts/famA_14_regular.bin"];
  TEST_ASSERT_EQUAL_INT(kOk, validateBlob(bytes.data(), bytes.size(), expect, &parsed));
}

void test_manager_export_all_skips_existing_packs() {
  SdFontManager mgr;
  MemorySdFontIo io;
  mgr.setIo(&io);
  TestVariant a(0x10);
  mgr.registerFont(kFontA, 14, "famA", &a.font, nullptr, nullptr, nullptr, nullptr);
  io.files["/fonts/famA_14_regular.bin"] = {1, 2, 3};  // pretend it's already there

  mgr.exportAllMissing();

  TEST_ASSERT_EQUAL_INT(0, io.exportCalls);  // nothing re-written
}

void test_manager_export_skips_when_no_flash_bitmap_slim() {
  // Slim build: flash bitmap dropped (nullptr) -> nothing to serialize from.
  SdFontManager mgr;
  MemorySdFontIo io;
  mgr.setIo(&io);
  TestVariant a(0x10, /*hasFlashBitmap=*/false);
  mgr.registerFont(kFontA, 14, "famA", &a.font, nullptr, nullptr, nullptr, nullptr);

  mgr.exportAllMissing();

  TEST_ASSERT_EQUAL_INT(0, io.exportCalls);
  TEST_ASSERT_FALSE(io.exists("/fonts/famA_14_regular.bin"));
}

void test_manager_ensure_active_repoints_font_to_sd() {
  SdFontManager mgr;
  MemorySdFontIo io;
  mgr.setIo(&io);
  TestVariant a(0x10);
  mgr.registerFont(kFontA, 14, "famA", &a.font, nullptr, nullptr, nullptr, nullptr);
  mgr.exportAllMissing();

  mgr.ensureActive(kFontA);

  // The live font now streams from SD: bitmap pointer cleared, callback wired.
  TEST_ASSERT_NULL(a.font.data->bitmap);
  TEST_ASSERT_NOT_NULL(a.font.data->readBitmapBytes);
  uint8_t got[20] = {};
  TEST_ASSERT_EQUAL_INT(20, readThroughFont(a.font, 0, got, 20));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(a.bitmap, got, 20);  // the right pack's bytes
}

void test_manager_ensure_active_swaps_sets_and_restores_flash() {
  SdFontManager mgr;
  MemorySdFontIo io;
  mgr.setIo(&io);
  TestVariant a(0x10), b(0x40);
  mgr.registerFont(kFontA, 14, "famA", &a.font, nullptr, nullptr, nullptr, nullptr);
  mgr.registerFont(kFontB, 12, "famB", &b.font, nullptr, nullptr, nullptr, nullptr);
  mgr.exportAllMissing();

  mgr.ensureActive(kFontA);
  TEST_ASSERT_NULL(a.font.data->bitmap);  // A is SD-backed

  mgr.ensureActive(kFontB);
  // Switching away restores A to its flash bitmap (fat build) and SD-backs B.
  TEST_ASSERT_EQUAL_PTR(&a.flash, a.font.data);
  TEST_ASSERT_NOT_NULL(a.font.data->bitmap);
  TEST_ASSERT_NULL(b.font.data->bitmap);
  uint8_t got[20] = {};
  TEST_ASSERT_EQUAL_INT(20, readThroughFont(b.font, 0, got, 20));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(b.bitmap, got, 20);
}

void test_manager_ensure_active_is_idempotent() {
  SdFontManager mgr;
  MemorySdFontIo io;
  mgr.setIo(&io);
  TestVariant a(0x10);
  mgr.registerFont(kFontA, 14, "famA", &a.font, nullptr, nullptr, nullptr, nullptr);
  mgr.exportAllMissing();

  mgr.ensureActive(kFontA);
  mgr.ensureActive(kFontA);
  mgr.ensureActive(kFontA);

  // Re-selecting the active font must not re-open handles (page-turn cheapness).
  TEST_ASSERT_EQUAL_INT(1, io.openCalls);
}

void test_manager_unknown_id_deactivates_active_font() {
  // getReaderFontId() can return an unregistered id (e.g. ChareInk emergency
  // downgrade). That must close the previous SD set and back nothing new.
  SdFontManager mgr;
  MemorySdFontIo io;
  mgr.setIo(&io);
  TestVariant a(0x10);
  mgr.registerFont(kFontA, 14, "famA", &a.font, nullptr, nullptr, nullptr, nullptr);
  mgr.exportAllMissing();

  mgr.ensureActive(kFontA);
  TEST_ASSERT_NULL(a.font.data->bitmap);

  mgr.ensureActive(/*some ChareInk id*/ 999);
  TEST_ASSERT_EQUAL_PTR(&a.flash, a.font.data);  // A restored to flash
  TEST_ASSERT_NOT_NULL(a.font.data->bitmap);
}

void test_manager_slim_falls_back_when_pack_missing() {
  // Slim build (no flash bitmap) AND no .bin on SD -> must substitute a guaranteed
  // flash font rather than leave the renderer dereferencing a null bitmap.
  SdFontManager mgr;
  MemorySdFontIo io;
  mgr.setIo(&io);
  TestVariant a(0x10, /*hasFlashBitmap=*/false);
  TestVariant fallback(0x77);  // stands in for ChareInk at the same size
  mgr.registerFont(kFontA, 14, "famA", &a.font, nullptr, nullptr, nullptr, &fallback.flash);
  // NOTE: no exportAllMissing(); slim build can't export, and the pack is absent.

  mgr.ensureActive(kFontA);

  TEST_ASSERT_EQUAL_PTR(&fallback.flash, a.font.data);  // fell back, not null bitmap
  TEST_ASSERT_NOT_NULL(a.font.data->bitmap);
}

void test_manager_fat_keeps_flash_when_open_fails() {
  // Fat build, pack missing and (forced) export failure -> keep the flash bitmap,
  // never break reading.
  SdFontManager mgr;
  MemorySdFontIo io;
  mgr.setIo(&io);
  TestVariant a(0x10);  // has flash bitmap
  mgr.registerFont(kFontA, 14, "famA", &a.font, nullptr, nullptr, nullptr, nullptr);
  // Pre-seed a CORRUPT pack so openSource succeeds but the loader rejects it,
  // exercising the open-failure path without removing the flash bitmap.
  io.files["/fonts/famA_14_regular.bin"] = {0, 1, 2, 3, 4, 5, 6, 7};  // garbage, bad magic

  mgr.ensureActive(kFontA);

  TEST_ASSERT_EQUAL_PTR(&a.flash, a.font.data);  // unchanged flash font
  TEST_ASSERT_NOT_NULL(a.font.data->bitmap);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_accepts_well_formed_blob);
  RUN_TEST(test_accepts_empty_blob);
  RUN_TEST(test_rejects_null_or_truncated_buffer);
  RUN_TEST(test_rejects_bad_magic);
  RUN_TEST(test_rejects_unsupported_version);
  RUN_TEST(test_rejects_wrong_bits_per_pixel);
  RUN_TEST(test_rejects_blob_size_overrun);
  RUN_TEST(test_rejects_count_mismatch_stale_card);
  RUN_TEST(test_rejects_corrupt_blob_bytes);
  RUN_TEST(test_crc32_matches_zlib_check_vector);
  RUN_TEST(test_loader_opens_valid_blob_and_streams_bytes);
  RUN_TEST(test_loader_trampoline_rejects_out_of_range_read);
  RUN_TEST(test_loader_rejects_bad_magic);
  RUN_TEST(test_loader_rejects_count_mismatch);
  RUN_TEST(test_loader_rejects_corrupt_blob_via_streamed_crc);
  RUN_TEST(test_loader_rejects_truncated_file);
  RUN_TEST(test_export_then_validate_and_consume_roundtrips);
  RUN_TEST(test_export_blob_size_spans_all_groups);
  RUN_TEST(test_manager_export_all_writes_missing_packs);
  RUN_TEST(test_manager_export_all_skips_existing_packs);
  RUN_TEST(test_manager_export_skips_when_no_flash_bitmap_slim);
  RUN_TEST(test_manager_ensure_active_repoints_font_to_sd);
  RUN_TEST(test_manager_ensure_active_swaps_sets_and_restores_flash);
  RUN_TEST(test_manager_ensure_active_is_idempotent);
  RUN_TEST(test_manager_unknown_id_deactivates_active_font);
  RUN_TEST(test_manager_slim_falls_back_when_pack_missing);
  RUN_TEST(test_manager_fat_keeps_flash_when_open_fails);
  return UNITY_END();
}
