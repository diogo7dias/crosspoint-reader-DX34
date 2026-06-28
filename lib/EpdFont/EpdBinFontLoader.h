#pragma once

#include <cstddef>
#include <cstdint>

#include "EpdBinFormat.h"

// SD-backed CPBN font loader (Tier 1: bitmap-only on SD).
//
// The glyph / interval / group / kerning tables for an offloaded built-in font
// stay compiled into flash exactly as before; only the DEFLATE bitmap blob lives
// in the .bin file. This loader opens that file through an injected BlobSource,
// validates the header against the firmware's expectation, verifies the blob
// CRC by STREAMING it in small chunks (never pinning the whole blob in RAM — the
// device is OOM-prone and the old full-file-in-heap loader was removed for
// exactly that reason), and then exposes a readBitmapBytes-compatible trampoline
// that FontDecompressor calls to pull one compressed group off disk on demand.
//
// BlobSource is the seam: the device wires a HalFile-backed implementation; host
// tests wire an in-memory one. The loader itself has no HAL dependency.

namespace crosspoint {
namespace binfont {

// Random-access byte source for the .bin file (offsets are absolute file bytes).
struct BlobSource {
  virtual ~BlobSource() = default;
  // Read exactly `len` bytes starting at absolute file `offset` into `dst`.
  // Returns `len` on success, anything else on failure.
  virtual int read(uint32_t offset, uint8_t* dst, size_t len) = 0;
  // Total number of bytes available in the source.
  virtual uint32_t size() const = 0;
};

class EpdBinFontLoader {
 public:
  // Validates the CPBN header (structure + table counts) and the blob CRC
  // (streamed) from `src` against `expect`. On kOk the loader is "open" and its
  // trampoline may be wired into an EpdFontData; any other result leaves the
  // loader closed so the caller falls back to a flash-resident font.
  BlobReject open(BlobSource* src, const FontBlobExpectation& expect) {
    src_ = nullptr;
    blobOffset_ = 0;
    blobSize_ = 0;
    if (src == nullptr) return kTooSmall;

    uint8_t hdr[sizeof(BlobHeader)];
    if (src->read(0, hdr, sizeof(hdr)) != static_cast<int>(sizeof(hdr))) return kTooSmall;

    BlobHeader h{};
    const BlobReject hr = parseHeader(hdr, sizeof(hdr), expect, &h);
    if (hr != kOk) return hr;

    const uint64_t need = static_cast<uint64_t>(sizeof(BlobHeader)) + h.bitmapBlobSize;
    if (need > src->size()) return kSizeOverrun;

    // Stream the blob through the CRC so integrity is checked at open without a
    // large contiguous allocation.
    uint32_t crc = kCrc32Init;
    uint8_t chunk[256];
    uint32_t pos = sizeof(BlobHeader);
    uint32_t remaining = h.bitmapBlobSize;
    while (remaining > 0) {
      const uint32_t n = remaining < sizeof(chunk) ? remaining : static_cast<uint32_t>(sizeof(chunk));
      if (src->read(pos, chunk, n) != static_cast<int>(n)) return kTooSmall;
      crc = crc32Update(crc, chunk, n);
      pos += n;
      remaining -= n;
    }
    if (crc32Finish(crc) != h.blobCrc32) return kBadCrc;

    src_ = src;
    blobOffset_ = sizeof(BlobHeader);
    blobSize_ = h.bitmapBlobSize;
    return kOk;
  }

  bool isOpen() const { return src_ != nullptr; }
  uint32_t blobOffset() const { return blobOffset_; }
  uint32_t blobSize() const { return blobSize_; }

  // Pass this as EpdFontData::bitmapCtx alongside &readBitmapTrampoline.
  void* bitmapCtx() { return this; }

  // Matches EpdFontData::readBitmapBytes. `offset` is relative to the blob start
  // (the compressed-group offset the FontDecompressor holds). Bounds-checked so a
  // bad group offset can never read past the blob into neighbouring file data.
  static int readBitmapTrampoline(void* ctx, uint32_t offset, uint8_t* dst, size_t len) {
    auto* self = static_cast<EpdBinFontLoader*>(ctx);
    if (self == nullptr || self->src_ == nullptr) return -1;
    if (static_cast<uint64_t>(offset) + len > self->blobSize_) return -1;
    return self->src_->read(self->blobOffset_ + static_cast<uint32_t>(offset), dst, len);
  }

 private:
  BlobSource* src_ = nullptr;
  uint32_t blobOffset_ = 0;
  uint32_t blobSize_ = 0;
};

}  // namespace binfont
}  // namespace crosspoint
