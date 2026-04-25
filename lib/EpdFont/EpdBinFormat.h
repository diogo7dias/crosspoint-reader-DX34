#pragma once

#include <cstdint>

// On-disk layout for one user-installed .bin (CPBN v1) custom font.
// Mirrors EpdFontData's in-memory shape so the built-in renderer can
// consume it unchanged — glyph bitmaps go through the same
// DEFLATE-group FontDecompressor path as built-ins. v1 has no kerning
// or ligature tables (those pointers stay null in the view).

namespace crosspoint {
namespace binfont {

constexpr uint32_t kMagic = 0x4E425043u;  // "CPBN" LE
constexpr uint8_t kVersion = 1;

enum Variant : uint8_t {
  kVariantRegular = 0,
  kVariantBold = 1,
  kVariantItalic = 2,
  kVariantBoldItalic = 3,
};

// 32-byte packed header. Body layout that follows:
//   EpdGlyph[glyphCount]             (10 B each, packed)
//   EpdUnicodeInterval[intervalCount] (12 B each)
//   EpdFontGroup[groupCount]          (20 B each, natural alignment)
//   uint8_t bitmapBlob[bitmapBlobSize] (concatenated DEFLATE streams)
struct __attribute__((packed)) Header {
  uint32_t magic;
  uint8_t version;
  uint8_t bitsPerPixel;  // 2
  uint8_t sizePt;        // 9..16
  uint8_t variant;       // Variant enum
  int16_t ascent;
  int16_t descent;
  uint16_t advanceY;   // line height in px
  uint16_t reserved0;  // MBZ
  uint32_t glyphCount;
  uint32_t intervalCount;
  uint32_t groupCount;
  uint32_t bitmapBlobSize;
};
static_assert(sizeof(Header) == 32, "CPBN header must be exactly 32 bytes");

constexpr size_t kMaxFileBytes = 256 * 1024;
constexpr uint32_t kMaxGlyphs = 65535;
constexpr uint32_t kMaxGroups = 2048;

// Per-variant cap on the heap-resident table footprint. The CPBN bitmap
// blob stays on SD; only the small fixed tables (header + glyph metadata
// + intervals + groups) live in heap during render. Caps the activation
// cost so a four-variant family stays well under the EPUB-layout heap
// budget. Computed as: glyphCount*10 + intervalCount*12 + groupCount*20
// + sizeof(Header). Empirically a 40 pt Latin-1 face fits inside ~6 KB;
// EBgaramond and similar wider-coverage fonts run 18-22 KB. The 24 KB
// cap covers those without committing too much BSS up-front (the static
// regular-variant buffer in CustomBinFontManager.cpp follows this
// constant). PR #96 moved that from 16 KB after hardware capture
// rejected EBgaramond at the prior cap.
constexpr uint32_t kMaxTablesBytes = 24 * 1024;

}  // namespace binfont
}  // namespace crosspoint
