#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "CustomFontGlyphSource.h"

namespace crosspoint {
namespace bdf {

// Render-time wrapper for a user-dropped BDF family. Parallel to
// EpdFontFamily for built-in fonts. GfxRenderer's text methods dispatch to
// this class when a fontId resolves to a CustomFont entry.
//
// Holds up to four variant slots (regular / bold / italic / bolditalic).
// The regular slot is required. Missing bold → rendered via synthetic
// bold passes. Missing italic → rendered via synthetic shear (see
// shouldSynthesizeItalic). The variant resolution mirrors
// EpdFontFamily::getFont: bold+italic falls through to italic → bold →
// regular when the exact combo is absent.
class CustomFont {
 public:
  // Style bits match EpdFontFamily::Style (bit 0 = BOLD, bit 1 = ITALIC).
  // Kept as a plain uint8_t so lib/BdfFont does not need to depend on
  // lib/EpdFont. Callers cast the EpdFontFamily::Style value directly.
  using StyleBits = uint8_t;
  static constexpr StyleBits STYLE_REGULAR = 0;
  static constexpr StyleBits STYLE_BOLD = 1;
  static constexpr StyleBits STYLE_ITALIC = 2;
  static constexpr StyleBits STYLE_BOLD_ITALIC = 3;

  static constexpr size_t SLOT_REGULAR = 0;
  static constexpr size_t SLOT_BOLD = 1;
  static constexpr size_t SLOT_ITALIC = 2;
  static constexpr size_t SLOT_BOLD_ITALIC = 3;

  CustomFont() = default;
  ~CustomFont() = default;

  CustomFont(const CustomFont&) = delete;
  CustomFont& operator=(const CustomFont&) = delete;

  // Opens the regular variant (required). Allocates + opens the glyph
  // source into the SLOT_REGULAR slot. Must be called before openVariant()
  // for any other slot.
  bool open(const char* bdfPath, const char* idxPath, uint16_t sizePt, size_t cacheSlots);

  // Opens an additional variant. `slot` in {SLOT_BOLD, SLOT_ITALIC,
  // SLOT_BOLD_ITALIC}. Safe to skip any subset — unopened slots fall
  // back via variant resolution + synthetic passes.
  bool openVariant(size_t slot, const char* bdfPath, const char* idxPath, size_t cacheSlots);

  bool isOpen() const { return variants_[SLOT_REGULAR] && variants_[SLOT_REGULAR]->isOpen(); }
  bool hasVariant(size_t slot) const { return slot < 4 && variants_[slot] && variants_[slot]->isOpen(); }
  uint16_t sizePt() const { return sizePt_; }

  // Synthetic-bold tuning. Mirrors EpdFontFamily:
  // totalPasses(style) = syntheticRegularBoldPasses_
  //                    + (no real bold variant AND style has BOLD ? syntheticBoldExtraPasses_ : 0)
  // Each pass stamps the set bitmap pixel offset by +1 px on x. Advance
  // grows by the same amount so glyphs don't overlap. When a real bold
  // variant is installed, the bold style uses its bitmap + 0 extra passes.
  void setSyntheticBold(uint8_t regularPasses, uint8_t boldExtraPasses) {
    syntheticRegularBoldPasses_ = regularPasses;
    syntheticBoldExtraPasses_ = boldExtraPasses;
  }
  uint8_t getSyntheticBoldPasses(StyleBits style) const;

  // True when the requested style needs italic shear — i.e. the style
  // has the ITALIC bit AND the family has no real italic variant to
  // fall back on. Built-in fonts left this as a vapor flag; custom
  // fonts actually implement shear in the GfxRenderer dispatch.
  bool shouldSynthesizeItalic(StyleBits style) const;

  // Font-wide metrics derived from the .idx header of the active variant
  // for the given style. Fall back to BBX height when ascent/descent are
  // zero (some BDFs omit them).
  int ascender(StyleBits style = STYLE_REGULAR) const;
  int descender(StyleBits style = STYLE_REGULAR) const;
  int lineHeight(StyleBits style = STYLE_REGULAR) const;

  // Measurement — style drives both the variant pick and the
  // synthetic-bold inflation so a BOLD run lays out wider than the
  // REGULAR version of the same text (even when a real bold variant is
  // installed, if that variant's glyph advances are wider).
  int getTextWidth(const char* text, StyleBits style = STYLE_REGULAR);
  int getTextAdvanceX(const char* text, int letterSpacing, StyleBits style = STYLE_REGULAR);
  int getSpaceWidth(StyleBits style = STYLE_REGULAR);
  bool hasGlyph(uint32_t cp, StyleBits style = STYLE_REGULAR);

  // Walks `text` in UTF-8 order, invoking `visit(cursorX, glyph)` for each
  // successfully-resolved glyph FROM THE VARIANT CHOSEN FOR `style`.
  // cursorX already has synthetic-bold inflation applied.
  using GlyphVisitor = std::function<bool(int cursorX, const CustomFontGlyphSource::Glyph& g)>;
  void visitGlyphs(const char* text, int letterSpacing, StyleBits style, const GlyphVisitor& visit);

  // Longest prefix (UTF-8) that fits in maxWidth, or the full text if it
  // already fits. Appends "..." if truncated.
  std::string truncatedText(const char* text, int maxWidth, StyleBits style = STYLE_REGULAR);

 private:
  // Variant resolution mirrors EpdFontFamily::getFont. Returns the
  // regular slot when the requested style's slot is absent.
  CustomFontGlyphSource* getVariant(StyleBits style) const;

  const CustomFontGlyphSource::Glyph* resolveGlyph(CustomFontGlyphSource& src, uint32_t cp);

  // unique_ptr so visitGlyph state (LRU) stays tied to one glyph source
  // and the struct remains non-copyable without deleting the move ops
  // on CustomFont itself.
  std::array<std::unique_ptr<CustomFontGlyphSource>, 4> variants_{};
  uint16_t sizePt_ = 0;
  uint8_t syntheticRegularBoldPasses_ = 0;
  uint8_t syntheticBoldExtraPasses_ = 1;  // 1 = visible-but-subtle bold
};

}  // namespace bdf
}  // namespace crosspoint
