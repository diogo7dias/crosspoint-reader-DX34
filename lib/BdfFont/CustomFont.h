#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "CustomFontGlyphSource.h"
#include "CustomFontSharedCache.h"

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
//
// All variant slots are opened eagerly in openVariant() — we used to defer
// file opens until the first lookup to "save FDs for fonts the user may not
// type into," but we only register ONE active family in the renderer at a
// time (SETTINGS.customFontName), so the FD saving never materialised and
// the lazy path forced every render call to re-check pending flags.
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

  // Sets the font size in points.
  void setSizePt(uint16_t sizePt) { sizePt_ = sizePt; }

  // Opens an additional variant. `slot` in {SLOT_REGULAR, SLOT_BOLD, SLOT_ITALIC,
  // SLOT_BOLD_ITALIC}. Safe to skip any subset — unopened slots fall
  // back via variant resolution + synthetic passes. Returns false if the
  // underlying CustomFontGlyphSource open fails (missing / malformed .idx).
  // The shared cache's byte budget is applied on the first successful open.
  bool openVariant(size_t slot, const char* bdfPath, const char* idxPath, size_t cacheBudgetBytes);

  bool isOpen() const { return variants_[SLOT_REGULAR] && variants_[SLOT_REGULAR]->isOpen(); }
  // True iff this variant was opened AND its files are readable.
  bool hasVariant(size_t slot) const {
    if (slot >= 4) return false;
    return variants_[slot] && variants_[slot]->isOpen();
  }
  uint16_t sizePt() const { return sizePt_; }

  // Shrink every variant's glyph cache to `slots` (default 1). Frees the
  // existing slab so a large contiguous allocation elsewhere can succeed.
  // Cache rebuilds organically on next lookup().
  void trimCache(size_t slots = 1);

  // Fully free the glyph slab (and all metadata tables). Unlike trimCache(1),
  // which keeps one slot's worth of slab + the LRU bookkeeping vectors alive,
  // this drops EVERYTHING so the region can coalesce into the 32 KB+ block
  // the epub section ZIP dictionary needs. restoreCache() brings the cache
  // back to its prior shape.
  void releaseCache();

  // Inverse of releaseCache(). Cheap no-op when nothing was released.
  // Returns true if a slab was re-allocated.
  bool restoreCache();

  // Diagnostic stats for the shared LRU cache (hits/misses/evictions plus
  // cumulative decode time). Resetting before a page render and logging
  // after isolates that frame's cache behaviour.
  CustomFontSharedCache::Stats cacheStats() const { return sharedCache_.getStats(); }
  void resetCacheStats() { sharedCache_.resetStats(); }
  size_t cacheCap() const { return sharedCache_.cacheCap(); }

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

  // Walks `text` and forces a cache lookup for every codepoint so the LRU is
  // warm before the real draw pass runs. No framebuffer side effects. Used by
  // the GfxRenderer scan pass (the one FontCacheManager drives for built-in
  // fonts) so first-page-on-a-new-script no longer pays SD I/O during the
  // visible draw. If the text has more unique codepoints than the cache
  // can hold, the tail codepoints cycle through the LRU — the caller-side
  // warning in the renderer flags that case.
  void prewarmGlyphs(const char* text, StyleBits style);

  // Longest prefix (UTF-8) that fits in maxWidth, or the full text if it
  // already fits. Appends "..." if truncated.
  std::string truncatedText(const char* text, int maxWidth, StyleBits style = STYLE_REGULAR);

 private:
  // Variant resolution mirrors EpdFontFamily::getFont. Returns the
  // regular slot when the requested style's slot is absent.
  CustomFontGlyphSource* getVariant(StyleBits style) const;

  const CustomFontGlyphSource::Glyph* resolveGlyph(CustomFontGlyphSource& src, uint32_t cp);

  // sharedCache_ must outlive variants_ — glyph sources hold a raw
  // pointer to it (set via setSharedCache). C++ destroys members in
  // reverse declaration order, so declare it FIRST to ensure it is
  // destroyed LAST.
  mutable CustomFontSharedCache sharedCache_;

  // Eager-owned variant sources. Empty slot == no variant for that style.
  std::array<std::unique_ptr<CustomFontGlyphSource>, 4> variants_{};

  // Cached variant-availability flags. Updated only inside openVariant().
  // Letting getSyntheticBoldPasses() / shouldSynthesizeItalic() read these
  // in O(1) per call avoids the old per-codepoint resolveSlot_ chain.
  bool hasRealBold_ = false;
  bool hasRealItalic_ = false;
  bool hasRealBoldItalic_ = false;

  uint16_t sizePt_ = 0;
  uint8_t syntheticRegularBoldPasses_ = 0;
  uint8_t syntheticBoldExtraPasses_ = 1;  // 1 = visible-but-subtle bold
  // Guard: setCacheBudget is applied once at the first successful open so a
  // later variant open does not flush the cache we just warmed.
  bool cacheBudgetApplied_ = false;
};

}  // namespace bdf
}  // namespace crosspoint
