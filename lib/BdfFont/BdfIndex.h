#pragma once

#include <cstddef>
#include <cstdint>

namespace crosspoint {
namespace bdf {

// On-disk format for the per-BDF glyph index. Lives next to the source BDF
// as <name>_<size>.idx. Built once on Install (Phase 2 Slice 2a) and then
// memory-mapped from SD via lookups during render.
//
// Layout (little-endian throughout):
//   [0..3]   magic "CPBI"
//   [4]      version (currently 1)
//   [5..7]   reserved (zeroed)
//   [8..11]  glyphCount (uint32)
//   [12]     font-wide bbxW   (int8) — informational, from FONTBOUNDINGBOX
//   [13]     font-wide bbxH   (int8)
//   [14]     font-wide ascent (int8)
//   [15]     font-wide descent(int8)
//   [16..]   glyphCount × IndexEntry, sorted by codepoint ascending
//
// Sorted-codepoint invariant lets the runtime do an O(log N) binary search.
// Unsorted BDFs are rejected at build time with an error.

constexpr uint32_t kBdfIndexMagic =
    static_cast<uint32_t>('C') | (static_cast<uint32_t>('P') << 8) | (static_cast<uint32_t>('B') << 16) |
    (static_cast<uint32_t>('I') << 24);
// v2: renamed IndexEntry.bdfOffset → bitmapOffset. Same size, same position —
// only the semantic changed (now points at the first hex row, not at the
// STARTCHAR line), which lets CustomFontGlyphSource::decodeBitmap seek
// directly to the hex data and skip the 64-line "find BITMAP" scan that
// every cold glyph paid under v1. v1 .idx files are rejected at open time
// so the install flow re-runs and produces a v2 file.
constexpr uint8_t kBdfIndexVersion = 2;

struct __attribute__((packed)) IndexHeader {
  uint32_t magic;        // kBdfIndexMagic
  uint8_t version;       // kBdfIndexVersion
  uint8_t reserved0;
  uint8_t reserved1;
  uint8_t reserved2;
  uint32_t glyphCount;
  int8_t fontBbxW;
  int8_t fontBbxH;
  int8_t fontAscent;
  int8_t fontDescent;
};
static_assert(sizeof(IndexHeader) == 16, "IndexHeader must be exactly 16 bytes");

struct __attribute__((packed)) IndexEntry {
  uint32_t codepoint;     // Unicode scalar
  uint32_t bitmapOffset;  // byte offset of the first hex row (first byte AFTER
                          // the "BITMAP\n" line) in the source BDF. Decoder
                          // seeks straight here.
  uint8_t bitmapW;        // BBX width  (BDF "BBX W H X Y" first field)
  uint8_t bitmapH;        // BBX height
  uint8_t advance;        // DWIDTH x (advance width in pixels). 0 if not present.
  int8_t bbxOffX;         // BBX X offset relative to origin
  int8_t bbxOffY;         // BBX Y offset relative to baseline
  uint8_t reserved0;
  uint8_t reserved1;
  uint8_t reserved2;
};
static_assert(sizeof(IndexEntry) == 16, "IndexEntry must be exactly 16 bytes");

}  // namespace bdf
}  // namespace crosspoint
