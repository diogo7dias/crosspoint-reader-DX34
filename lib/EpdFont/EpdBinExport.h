#pragma once

#include <cstddef>
#include <cstdint>

#include "EpdBinFormat.h"
#include "EpdFontData.h"

// Serialize an in-flash EpdFontData's compressed bitmap blob into a CPBN file.
//
// This is the on-device bootstrap: the firmware already holds every built-in
// font's bitmap in flash, so an "Export font packs to SD" action can write the
// authoritative .bin files straight to the SD card with no card reader or web
// upload. The emitted bytes are byte-identical to the host-baked .bin (same
// header layout, same blob), so a slim (bitmap-dropped) build reads them back
// transparently. Pure + HAL-free so it is host-testable; the caller supplies the
// ByteSink (a HalFile writer on device, an in-memory buffer in tests).

namespace crosspoint {
namespace binfont {

struct ByteSink {
  virtual ~ByteSink() = default;
  // Append `len` bytes; return false on write failure (aborts the export).
  virtual bool write(const uint8_t* data, size_t len) = 0;
};

// Total glyphs covered by the font's unicode intervals.
inline uint32_t glyphCountFromIntervals(const EpdFontData& fd) {
  uint32_t n = 0;
  for (uint32_t i = 0; i < fd.intervalCount; ++i) {
    n += fd.intervals[i].last - fd.intervals[i].first + 1;
  }
  return n;
}

// Bitmap blob length = the furthest byte any group reaches. Groups are not
// required to be ordered, so take the max end rather than the last group's.
inline uint32_t blobSizeFromGroups(const EpdFontData& fd) {
  uint32_t end = 0;
  for (uint16_t i = 0; i < fd.groupCount; ++i) {
    const uint32_t e = fd.groups[i].compressedOffset + fd.groups[i].compressedSize;
    if (e > end) end = e;
  }
  return end;
}

// Writes [BlobHeader][bitmap blob] for a compressed (group-backed) font. Returns
// false for an uncompressed font, a null bitmap, or on a sink write failure.
inline bool exportFontBlob(const EpdFontData& fd, uint8_t variant, uint16_t sizePt, ByteSink& sink) {
  if (fd.bitmap == nullptr || fd.groups == nullptr || fd.groupCount == 0) return false;

  const uint32_t blobSize = blobSizeFromGroups(fd);

  BlobHeader h{};
  h.magic = kMagic;
  h.version = kBlobFormatVersion;
  h.bitsPerPixel = kBitsPerPixel;
  h.flags = 0;
  h.variant = variant;
  h.sizePt = sizePt;
  h.advanceY = fd.advanceY;
  h.ascent = static_cast<int16_t>(fd.ascender);
  h.descent = static_cast<int16_t>(fd.descender);
  h.reserved0 = 0;
  h.glyphCount = glyphCountFromIntervals(fd);
  h.intervalCount = fd.intervalCount;
  h.groupCount = fd.groupCount;
  h.bitmapBlobSize = blobSize;
  h.blobCrc32 = crc32(fd.bitmap, blobSize);

  uint8_t hdr[sizeof(BlobHeader)];
  for (size_t i = 0; i < sizeof(BlobHeader); ++i) hdr[i] = reinterpret_cast<const uint8_t*>(&h)[i];
  if (!sink.write(hdr, sizeof(hdr))) return false;
  if (blobSize > 0 && !sink.write(fd.bitmap, blobSize)) return false;
  return true;
}

}  // namespace binfont
}  // namespace crosspoint
