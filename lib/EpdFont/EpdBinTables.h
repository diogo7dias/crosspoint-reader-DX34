#pragma once

#include <cstddef>
#include <cstdint>

#include "EpdBinFormat.h"
#include "EpdFontData.h"

// Tier 2 of the CPBN SD-font format: the glyph / interval / group / kerning /
// ligature tables travel in the .bin file alongside the bitmap blob, instead of
// staying compiled into flash (Tier 1). This lets a brand-new reader size cost
// ~0 flash — only the bitmap was offloaded in Tier 1; here the metadata tables
// go too. The tables are materialised into heap only while the font is the
// active reader font (the active-set swap bounds the RAM cost), which is why the
// removed-in-2026 v1 format's "pin every table forever" mistake is not repeated.
//
//   Tier-2 file:  [BlobHeader (flags|=kFlagTablesInFile)]
//                 [TablesHeader] [serialized tables ...]
//                 [bitmap blob]
//
// This header owns the on-disk table layout, the serialization byte order
// (emitTableBytes — the single source of truth shared by the exporter and the
// parser), and parseTablesSection, which reconstructs an EpdFontData whose
// pointers alias the caller's in-memory tables buffer (groups excepted: their
// padded in-memory struct differs from the 18-byte packed on-disk form, so they
// are expanded into a caller-provided aligned array).

namespace crosspoint {
namespace binfont {

constexpr uint32_t kTablesMagic = 0x42545043u;  // "CPTB" little-endian
constexpr uint16_t kTableSectionVersion = 1u;

// Packed on-disk mirror of EpdFontGroup. The in-memory struct pads to 20 bytes
// (uint16 glyphCount before a uint32), so it cannot be memcpy'd to disk; this
// 18-byte form is the wire layout.
struct __attribute__((packed)) PackedGroup {
  uint32_t compressedOffset;
  uint32_t compressedSize;
  uint32_t uncompressedSize;
  uint16_t glyphCount;
  uint32_t firstGlyphIndex;
};
static_assert(sizeof(PackedGroup) == 18, "PackedGroup must be exactly 18 bytes");

// Fixed header preceding the serialized tables (when kFlagTablesInFile is set).
// Counts are cross-checked against the BlobHeader; the CRC and byte-length guard
// the table bytes the same way BlobHeader guards the bitmap blob.
struct __attribute__((packed)) TablesHeader {
  uint32_t magic;               // kTablesMagic
  uint16_t version;             // kTableSectionVersion
  uint8_t is2Bit;               // EpdFontData.is2Bit
  uint8_t hasGlyphToGroup;      // 1 if a glyphToGroup[] array follows
  uint32_t glyphCount;          // == BlobHeader.glyphCount
  uint32_t intervalCount;       // == BlobHeader.intervalCount
  uint16_t groupCount;          // == BlobHeader.groupCount
  uint16_t kernLeftEntryCount;  // EpdKernClassEntry rows in kernLeftClasses
  uint16_t kernRightEntryCount; // EpdKernClassEntry rows in kernRightClasses
  uint8_t kernLeftClassCount;   // CSR rows (kernRowStart has +1 entries)
  uint8_t kernRightClassCount;  // CSR columns
  uint32_t kernCellCount;       // length of kernCols / kernValues
  uint32_t ligaturePairCount;   // entries in ligaturePairs
  uint32_t tablesByteLength;    // bytes of serialized tables following this header
  uint32_t tablesCrc32;         // CRC32 over those table bytes
};
static_assert(sizeof(TablesHeader) == 40, "CPBN TablesHeader must be exactly 40 bytes");

// Count of CSR kerning cells for a font (length of kernCols / kernValues). Zero
// when the font has no class-based kerning.
inline uint32_t kernCellCountOf(const EpdFontData& fd) {
  return fd.kernLeftClassCount ? fd.kernRowStart[fd.kernLeftClassCount] : 0u;
}

// Total serialized-tables byte length implied by a font's counts. Mirrors the
// exact array set (and order) emitTableBytes writes.
inline uint32_t tablesByteLengthOf(const EpdFontData& fd, uint32_t glyphCount, uint32_t kernCellCount) {
  uint32_t n = 0;
  n += glyphCount * sizeof(EpdGlyph);
  n += fd.intervalCount * sizeof(EpdUnicodeInterval);
  n += fd.groupCount * sizeof(PackedGroup);
  if (fd.glyphToGroup != nullptr) n += glyphCount * 2u;
  n += fd.kernLeftEntryCount * sizeof(EpdKernClassEntry);
  n += fd.kernRightEntryCount * sizeof(EpdKernClassEntry);
  if (fd.kernLeftClassCount) n += (fd.kernLeftClassCount + 1u) * 2u;
  n += kernCellCount;  // kernCols
  n += kernCellCount;  // kernValues
  n += fd.ligaturePairCount * sizeof(EpdLigaturePair);
  return n;
}

// Emits the serialized tables, in the canonical order, to `out` — a functor
// `void(const uint8_t*, size_t)`. The single source of truth for the wire order:
// the exporter feeds a sink, a pre-pass feeds a CRC accumulator, and
// parseTablesSection walks the very same layout. Groups are packed to 18 bytes
// on the way out; everything else is already a fixed-width packed form.
template <typename Emit>
inline void emitTableBytes(const EpdFontData& fd, uint32_t glyphCount, uint32_t kernCellCount, Emit&& out) {
  out(reinterpret_cast<const uint8_t*>(fd.glyph), glyphCount * sizeof(EpdGlyph));
  out(reinterpret_cast<const uint8_t*>(fd.intervals), fd.intervalCount * sizeof(EpdUnicodeInterval));
  for (uint16_t i = 0; i < fd.groupCount; ++i) {
    const PackedGroup pg{fd.groups[i].compressedOffset, fd.groups[i].compressedSize,
                         fd.groups[i].uncompressedSize, fd.groups[i].glyphCount,
                         fd.groups[i].firstGlyphIndex};
    out(reinterpret_cast<const uint8_t*>(&pg), sizeof(PackedGroup));
  }
  if (fd.glyphToGroup != nullptr) {
    out(reinterpret_cast<const uint8_t*>(fd.glyphToGroup), glyphCount * 2u);
  }
  if (fd.kernLeftEntryCount) {
    out(reinterpret_cast<const uint8_t*>(fd.kernLeftClasses), fd.kernLeftEntryCount * sizeof(EpdKernClassEntry));
  }
  if (fd.kernRightEntryCount) {
    out(reinterpret_cast<const uint8_t*>(fd.kernRightClasses), fd.kernRightEntryCount * sizeof(EpdKernClassEntry));
  }
  if (fd.kernLeftClassCount) {
    out(reinterpret_cast<const uint8_t*>(fd.kernRowStart), (fd.kernLeftClassCount + 1u) * 2u);
  }
  if (kernCellCount) {
    out(reinterpret_cast<const uint8_t*>(fd.kernCols), kernCellCount);
    out(reinterpret_cast<const uint8_t*>(fd.kernValues), kernCellCount);
  }
  if (fd.ligaturePairCount) {
    out(reinterpret_cast<const uint8_t*>(fd.ligaturePairs), fd.ligaturePairCount * sizeof(EpdLigaturePair));
  }
}

// Reconstructs an EpdFontData from a serialized tables section (the bytes
// immediately after the BlobHeader). On kOk:
//   - `*fd` points its arrays into `tables` (so `tables` must outlive `fd`),
//     except `groups`, which is expanded into `groupsOut` (capacity groupsCap),
//   - `fd->bitmap` is left null: the SD loader wires the streaming bitmap
//     callback afterwards.
// `bh` supplies advanceY/ascent/descent and the counts the tables are checked
// against. Any structural / CRC / capacity failure returns a BlobReject and
// leaves `*fd` untouched, so the caller falls back to a flash-resident font.
inline BlobReject parseTablesSection(const uint8_t* tables, uint32_t len, const BlobHeader& bh,
                                     EpdFontData* fd, EpdFontGroup* groupsOut, uint16_t groupsCap) {
  if (tables == nullptr || len < sizeof(TablesHeader)) return kTooSmall;

  TablesHeader th{};
  for (size_t i = 0; i < sizeof(TablesHeader); ++i) {
    reinterpret_cast<uint8_t*>(&th)[i] = tables[i];
  }
  if (th.magic != kTablesMagic) return kBadMagic;
  if (th.version != kTableSectionVersion) return kBadVersion;
  if (th.glyphCount != bh.glyphCount || th.intervalCount != bh.intervalCount ||
      th.groupCount != bh.groupCount) {
    return kCountMismatch;
  }
  if (th.groupCount > groupsCap) return kCountMismatch;

  const uint64_t need = static_cast<uint64_t>(sizeof(TablesHeader)) + th.tablesByteLength;
  if (need > len) return kSizeOverrun;
  if (crc32(tables + sizeof(TablesHeader), th.tablesByteLength) != th.tablesCrc32) return kBadCrc;

  // Walk the canonical array order, slicing the section into typed pointers and
  // verifying the running total equals the declared length so a malformed count
  // can never point a pointer past the validated bytes.
  const uint8_t* const base = tables + sizeof(TablesHeader);
  uint64_t off = 0;
  auto take = [&](uint64_t bytes) -> const uint8_t* {
    const uint8_t* r = base + off;
    off += bytes;
    return r;
  };
  const uint8_t* glyphP = take(static_cast<uint64_t>(th.glyphCount) * sizeof(EpdGlyph));
  const uint8_t* intP = take(static_cast<uint64_t>(th.intervalCount) * sizeof(EpdUnicodeInterval));
  const uint8_t* grpP = take(static_cast<uint64_t>(th.groupCount) * sizeof(PackedGroup));
  const uint8_t* g2gP = th.hasGlyphToGroup ? take(static_cast<uint64_t>(th.glyphCount) * 2u) : nullptr;
  const uint8_t* klP = take(static_cast<uint64_t>(th.kernLeftEntryCount) * sizeof(EpdKernClassEntry));
  const uint8_t* krP = take(static_cast<uint64_t>(th.kernRightEntryCount) * sizeof(EpdKernClassEntry));
  const uint8_t* rowP = th.kernLeftClassCount ? take((static_cast<uint64_t>(th.kernLeftClassCount) + 1u) * 2u) : nullptr;
  const uint8_t* colP = take(static_cast<uint64_t>(th.kernCellCount));
  const uint8_t* valP = take(static_cast<uint64_t>(th.kernCellCount));
  const uint8_t* ligP = take(static_cast<uint64_t>(th.ligaturePairCount) * sizeof(EpdLigaturePair));
  if (off != th.tablesByteLength) return kCountMismatch;

  // Expand the packed 18-byte groups into the caller's aligned array.
  for (uint16_t i = 0; i < th.groupCount; ++i) {
    PackedGroup pg{};
    for (size_t b = 0; b < sizeof(PackedGroup); ++b) {
      reinterpret_cast<uint8_t*>(&pg)[b] = grpP[i * sizeof(PackedGroup) + b];
    }
    groupsOut[i].compressedOffset = pg.compressedOffset;
    groupsOut[i].compressedSize = pg.compressedSize;
    groupsOut[i].uncompressedSize = pg.uncompressedSize;
    groupsOut[i].glyphCount = pg.glyphCount;
    groupsOut[i].firstGlyphIndex = pg.firstGlyphIndex;
  }

  *fd = EpdFontData{};
  fd->bitmap = nullptr;  // SD loader wires readBitmapBytes
  fd->glyph = reinterpret_cast<const EpdGlyph*>(glyphP);
  fd->intervals = reinterpret_cast<const EpdUnicodeInterval*>(intP);
  fd->intervalCount = th.intervalCount;
  fd->advanceY = static_cast<uint8_t>(bh.advanceY);
  fd->ascender = bh.ascent;
  fd->descender = bh.descent;
  fd->is2Bit = th.is2Bit != 0;
  fd->groups = groupsOut;
  fd->groupCount = th.groupCount;
  fd->glyphToGroup = reinterpret_cast<const uint16_t*>(g2gP);
  fd->kernLeftClasses = th.kernLeftEntryCount ? reinterpret_cast<const EpdKernClassEntry*>(klP) : nullptr;
  fd->kernRightClasses = th.kernRightEntryCount ? reinterpret_cast<const EpdKernClassEntry*>(krP) : nullptr;
  fd->kernRowStart = reinterpret_cast<const uint16_t*>(rowP);
  fd->kernCols = th.kernCellCount ? colP : nullptr;
  fd->kernValues = th.kernCellCount ? reinterpret_cast<const int8_t*>(valP) : nullptr;
  fd->kernLeftEntryCount = th.kernLeftEntryCount;
  fd->kernRightEntryCount = th.kernRightEntryCount;
  fd->kernLeftClassCount = th.kernLeftClassCount;
  fd->kernRightClassCount = th.kernRightClassCount;
  fd->ligaturePairs = th.ligaturePairCount ? reinterpret_cast<const EpdLigaturePair*>(ligP) : nullptr;
  fd->ligaturePairCount = th.ligaturePairCount;
  return kOk;
}

}  // namespace binfont
}  // namespace crosspoint
