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
#include <vector>

#include "EpdBinFormat.h"

using namespace crosspoint::binfont;

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
  return UNITY_END();
}
