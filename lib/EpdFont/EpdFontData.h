// From
// https://github.com/vroland/epdiy/blob/c61e9e923ce2418150d54f88cea5d196cdc40c54/src/epd_internals.h

#pragma once
#include <cstddef>
#include <cstdint>

/// Font metrics use "fixed-point 4" (4 fractional bits, i.e. 1/16-pixel
/// resolution).  Both the 12.4 glyph advances (uint16_t) and the 4.4 kern
/// values (int8_t) share the same 4 fractional bits, so they can be freely
/// added into a single int32_t accumulator during text layout.  The
/// accumulator is snapped to the nearest whole pixel only at render time,
/// which avoids the per-character rounding errors that plagued integer-only
/// layout.
///
/// The helpers below eliminate the raw bit-shifts that would otherwise be
/// scattered across every layout / measurement call site.
namespace fp4 {
constexpr int FRAC_BITS = 4;
constexpr int32_t HALF = 1 << (FRAC_BITS - 1);  // 8, added before shift for round-to-nearest

/// Convert an integer pixel value to 12.4 fixed-point.
constexpr int32_t fromPixel(int px) { return static_cast<int32_t>(px) << FRAC_BITS; }

/// Snap a fixed-point value to the nearest integer pixel.
constexpr int toPixel(int32_t fp) { return static_cast<int>((fp + HALF) >> FRAC_BITS); }

/// Convert a fixed-point value to float (mainly useful for debug logging).
constexpr float toFloat(int32_t fp) { return fp / static_cast<float>(1 << FRAC_BITS); }
}  // namespace fp4

/// Fixed-point conventions used by EpdGlyph and EpdFontData:
///   advanceX:    12.4 unsigned fixed-point in uint16_t (use fp4::toPixel)
///   kernValues:  4.4 signed fixed-point in int8_t      (use fp4::toPixel)
/// Both share 4 fractional bits so they combine directly in an accumulator.

/// Font data stored PER GLYPH.
///
/// Layout is hand-packed to 10 bytes (down from 16 with natural alignment).  This saves
/// ~6 bytes per glyph × thousands of glyphs per font × dozens of fonts = hundreds of KB of
/// flash in the OTA app partition.  Field widths were chosen after auditing the entire
/// built-in font collection:
///
///   width/height  ≤ 51 / 44 in the shipping 12–17 pt reader fonts        → uint8_t
///   left          in [-26, 11]                                           → int8_t
///   top           in [-9, 43]                                            → int8_t
///   dataLength    ≤ 420 bytes (largest packed 2-bit glyph)               → uint16_t
///   dataOffset    ≤ 46,435 bytes (largest uncompressed-font bitmap, and
///                 groups are ≤ 36 KB uncompressed for compressed fonts)  → uint16_t
///
/// fontconvert.py asserts each of these ranges at emit time so a future font addition that
/// would overflow fails the build loudly rather than producing a silently corrupted header.
///
/// The struct is marked `packed` because the mix of 1/2-byte fields would otherwise get
/// padded to 12 bytes on ESP32-C3 (RV32).  RV32IMC handles unaligned `lh`/`lw` transparently
/// in hardware, so the per-access cost is negligible and glyph lookup is not on any hot
/// inner loop.
typedef struct {
  uint8_t width;        ///< Bitmap width in pixels
  uint8_t height;       ///< Bitmap height in pixels
  uint16_t advanceX;    ///< Distance to advance cursor (x axis), 12.4 fixed-point in pixels
  int8_t left;          ///< X dist from cursor pos to UL corner
  int8_t top;           ///< Y dist from cursor pos to UL corner
  uint16_t dataLength;  ///< Size of the font data.
  uint16_t dataOffset;  ///< Pointer into EpdFont->bitmap (or within-group offset for compressed fonts)
} __attribute__((packed)) EpdGlyph;

/// Compressed font group: a DEFLATE-compressed block of glyph bitmaps
typedef struct {
  uint32_t compressedOffset;  ///< Byte offset into compressed data array
  uint32_t compressedSize;    ///< Compressed DEFLATE stream size
  uint32_t uncompressedSize;  ///< Decompressed size
  uint16_t glyphCount;        ///< Number of glyphs in this group
  uint32_t firstGlyphIndex;   ///< First glyph index in the global glyph array
} EpdFontGroup;

/// Glyph interval structure
typedef struct {
  uint32_t first;   ///< The first unicode code point of the interval
  uint32_t last;    ///< The last unicode code point of the interval
  uint32_t offset;  ///< Index of the first code point into the glyph array
} EpdUnicodeInterval;

/// Maps a codepoint to a kerning class ID, sorted by codepoint for binary search.
/// Class IDs are 1-based; codepoints not in the table have implicit class 0 (no kerning).
typedef struct {
  uint16_t codepoint;  ///< Unicode codepoint
  uint8_t classId;     ///< 1-based kerning class ID
} __attribute__((packed)) EpdKernClassEntry;

/// Ligature substitution for a specific glyph pair, sorted by `pair` for binary search.
/// `pair` encodes (leftCodepoint << 16 | rightCodepoint) for single-key lookup.
typedef struct {
  uint32_t pair;        ///< Packed codepoint pair (left << 16 | right)
  uint32_t ligatureCp;  ///< Codepoint of the replacement ligature glyph
} __attribute__((packed)) EpdLigaturePair;

/// Data stored for FONT AS A WHOLE
/// New fields have defaults so existing font headers (without kerning/ligature data) compile unchanged.
typedef struct {
  const uint8_t* bitmap;                ///< Glyph bitmaps, concatenated
  const EpdGlyph* glyph;                ///< Glyph array
  const EpdUnicodeInterval* intervals;  ///< Valid unicode intervals for this font
  uint32_t intervalCount;               ///< Number of unicode intervals.
  uint8_t advanceY;                     ///< Newline distance (y axis)
  int ascender;                         ///< Maximal height of a glyph above the base line
  int descender;                        ///< Maximal height of a glyph below the base line
  bool is2Bit;
  const EpdFontGroup* groups = nullptr;                 ///< NULL for uncompressed fonts
  uint16_t groupCount = 0;                              ///< 0 for uncompressed fonts
  const uint16_t* glyphToGroup = nullptr;               ///< Per-glyph group ID (nullptr for contiguous-group fonts)
  const EpdKernClassEntry* kernLeftClasses = nullptr;   ///< Sorted left-side class map (nullptr if none)
  const EpdKernClassEntry* kernRightClasses = nullptr;  ///< Sorted right-side class map (nullptr if none)
  /// CSR-style sparse kerning matrix.  A dense leftClassCount x rightClassCount grid of
  /// int8_t 4.4 values is typically ~90 % zeros for Latin class-based kerning, so we store
  /// only the non-zero cells:
  ///
  ///   kernRowStart[leftClassId - 1]       points to the first entry for that row
  ///   kernRowStart[leftClassId]           points one past the last entry
  ///   kernCols[i]   is the (rightClassId - 1) of the i-th non-zero
  ///   kernValues[i] is the 4.4 fixed-point adjustment of the i-th non-zero
  ///
  /// kernRowStart has length (leftClassCount + 1) and is non-decreasing, with
  /// kernRowStart[0] = 0 and kernRowStart[leftClassCount] = total non-zeros.  Within each
  /// row, kernCols is sorted ascending so getKerning() can linear-scan (rows are short
  /// enough that binary search costs more than it saves).
  const uint16_t* kernRowStart = nullptr;          ///< Row offsets into kernCols/kernValues (nullptr if no kerning)
  const uint8_t* kernCols = nullptr;               ///< Right-class indices of non-zero cells (0-based)
  const int8_t* kernValues = nullptr;              ///< 4.4 fixed-point kerning values, one per non-zero
  uint16_t kernLeftEntryCount = 0;                 ///< Entries in kernLeftClasses
  uint16_t kernRightEntryCount = 0;                ///< Entries in kernRightClasses
  uint8_t kernLeftClassCount = 0;                  ///< Number of distinct left classes (CSR rows)
  uint8_t kernRightClassCount = 0;                 ///< Number of distinct right classes (CSR columns)
  const EpdLigaturePair* ligaturePairs = nullptr;  ///< Sorted ligature pair table (nullptr if none)
  uint32_t ligaturePairCount = 0;                  ///< Number of entries in ligaturePairs

  /// Optional read-bitmap callback. When non-null, FontDecompressor invokes
  /// this instead of reading `bitmap[offset]` directly — used by SD-backed
  /// custom fonts to keep the compressed glyph blob on disk rather than
  /// pinning ~30 KB per variant in heap. Implementations must read exactly
  /// `len` bytes starting at `offset` (relative to the start of the blob)
  /// into `dst` and return `len` on success, anything else on failure.
  /// Built-in fonts leave this null and the decompressor uses the embedded
  /// PROGMEM pointer as before.
  void* bitmapCtx = nullptr;
  int (*readBitmapBytes)(void* ctx, uint32_t offset, uint8_t* dst, size_t len) = nullptr;
} EpdFontData;
