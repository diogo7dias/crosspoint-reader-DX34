#pragma once

#include <cstddef>
#include <cstdint>

// On-disk layout for one CPBN (CrossPoint BiNary) SD-backed font blob.
//
// Lineage: a v1 CPBN format once shipped for user-uploaded custom fonts and was
// removed in 2026-06 to reclaim heap (it pinned the glyph tables in RAM). This
// is the revived, integrity-checked successor used to offload BUILT-IN reader
// families off the OTA flash partition and onto the SD card.
//
// Tier 1 (this version): only the DEFLATE bitmap blob lives on SD. The glyph /
// interval / group / kerning tables stay compiled into flash exactly as before,
// so the EpdFontData struct keeps its flash pointers and merely swaps its
// `bitmap` pointer for the `readBitmapBytes` SD callback. The blob's group
// offsets are identical whether the blob sits in PROGMEM or on disk.
//
//   File layout:  [BlobHeader (packed)] [bitmapBlob (bitmapBlobSize bytes)]
//
// validateBlob() is the daily-driver safety gate: the renderer only trusts a
// blob that (a) is structurally well-formed, (b) matches the firmware compiled
// against it (glyph/interval/group counts — guards a stale SD card after an OTA
// that changed a font), and (c) is bit-intact (CRC32 over the blob bytes). Any
// failure means the caller falls back to a flash-resident font; reading never
// proceeds on an untrusted blob.

namespace crosspoint {
namespace binfont {

constexpr uint32_t kMagic = 0x4E425043u;     // "CPBN" little-endian
constexpr uint8_t kBlobFormatVersion = 2u;   // v2: integrity-checked, tables-in-flash (Tier 1)
constexpr uint8_t kBitsPerPixel = 2u;        // reader fonts are 2-bit greyscale

// Header flag bits.
constexpr uint8_t kFlagTablesInFile = 0x01u;  // reserved for Tier 2 (tables follow header); 0 in Tier 1

// 38-byte packed header preceding the bitmap blob.
struct __attribute__((packed)) BlobHeader {
  uint32_t magic;           // kMagic
  uint8_t version;          // kBlobFormatVersion
  uint8_t bitsPerPixel;     // kBitsPerPixel
  uint8_t flags;            // kFlag* bitfield (0 in Tier 1)
  uint8_t variant;          // 0=regular 1=bold 2=italic 3=bolditalic
  uint16_t sizePt;          // nominal point size
  uint16_t advanceY;        // line height in px
  int16_t ascent;
  int16_t descent;
  uint16_t reserved0;       // MBZ
  uint32_t glyphCount;      // expected glyph count (cross-checked vs flash tables)
  uint32_t intervalCount;   // expected unicode-interval count
  uint32_t groupCount;      // expected DEFLATE-group count
  uint32_t bitmapBlobSize;  // length of the blob that follows the header
  uint32_t blobCrc32;       // CRC32 (zlib polynomial) over the blob bytes
};
static_assert(sizeof(BlobHeader) == 38, "CPBN BlobHeader must be exactly 38 bytes");

// The firmware's compiled-in expectation for the font this blob backs. Pulled
// from the flash EpdFontData tables so a stale SD blob (wrong counts) is caught.
struct FontBlobExpectation {
  uint32_t glyphCount;
  uint32_t intervalCount;
  uint32_t groupCount;
};

// Where the trusted blob lives within the file buffer, on success.
struct ParsedBlob {
  uint32_t blobOffset;  // byte offset of the blob (== sizeof(BlobHeader))
  uint32_t blobSize;    // == header.bitmapBlobSize
};

enum BlobReject : uint8_t {
  kOk = 0,
  kTooSmall,       // buffer shorter than the header
  kBadMagic,       // not a CPBN file
  kBadVersion,     // unsupported format version
  kBadBitsPerPix,  // unexpected bit depth
  kSizeOverrun,    // header.bitmapBlobSize does not fit in the buffer
  kCountMismatch,  // tables do not match this firmware (stale SD card)
  kBadCrc,         // blob bytes are corrupt
};

// CRC32 with the zlib/PNG polynomial (0xEDB88320), matching Python zlib.crc32 so
// the serializer and the validator agree. Exposed in incremental form so the
// device loader can stream the blob through it in chunks rather than pinning the
// whole blob in a contiguous buffer just to check integrity at open time.
constexpr uint32_t kCrc32Init = 0xFFFFFFFFu;

inline uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b) {
      const uint32_t mask = -(crc & 1u);
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return crc;
}

inline uint32_t crc32Finish(uint32_t crc) { return ~crc; }

inline uint32_t crc32(const uint8_t* data, size_t len) {
  return crc32Finish(crc32Update(kCrc32Init, data, len));
}

// Validates the fixed 38-byte header alone: structure (magic/version/bit-depth)
// and that the tables match this firmware (counts). Does NOT verify the blob
// CRC or that the blob fits any particular buffer — callers that stream the blob
// off SD verify the CRC incrementally via crc32Update. Fills `*out` on success.
inline BlobReject parseHeader(const uint8_t* buf, size_t len, const FontBlobExpectation& expect, BlobHeader* out) {
  if (buf == nullptr || len < sizeof(BlobHeader)) return kTooSmall;

  BlobHeader h{};
  // Copy out of the (possibly unaligned) buffer rather than aliasing it.
  for (size_t i = 0; i < sizeof(BlobHeader); ++i) {
    reinterpret_cast<uint8_t*>(&h)[i] = buf[i];
  }

  if (h.magic != kMagic) return kBadMagic;
  if (h.version != kBlobFormatVersion) return kBadVersion;
  if (h.bitsPerPixel != kBitsPerPixel) return kBadBitsPerPix;

  // Tier-2 blobs carry their own tables and have no flash counterpart to match,
  // so the count cross-check (a stale-SD guard meaningful only for Tier-1, where
  // the tables live in flash) is skipped; the TablesHeader self-checks instead.
  if ((h.flags & kFlagTablesInFile) == 0) {
    if (h.glyphCount != expect.glyphCount || h.intervalCount != expect.intervalCount ||
        h.groupCount != expect.groupCount) {
      return kCountMismatch;
    }
  }

  if (out != nullptr) *out = h;
  return kOk;
}

// Validates a fully in-memory CPBN blob buffer against the firmware's
// expectation, including the blob CRC. Returns kOk and fills `out` when the blob
// is trustworthy; otherwise returns the reason and leaves `out` untouched.
inline BlobReject validateBlob(const uint8_t* buf, size_t len, const FontBlobExpectation& expect, ParsedBlob* out) {
  BlobHeader h{};
  const BlobReject hr = parseHeader(buf, len, expect, &h);
  if (hr != kOk) return hr;

  // Blob must fit within the buffer (guard the addition against overflow).
  const uint64_t need = static_cast<uint64_t>(sizeof(BlobHeader)) + h.bitmapBlobSize;
  if (need > len) return kSizeOverrun;

  if (crc32(buf + sizeof(BlobHeader), h.bitmapBlobSize) != h.blobCrc32) return kBadCrc;

  if (out != nullptr) {
    out->blobOffset = sizeof(BlobHeader);
    out->blobSize = h.bitmapBlobSize;
  }
  return kOk;
}

}  // namespace binfont
}  // namespace crosspoint
