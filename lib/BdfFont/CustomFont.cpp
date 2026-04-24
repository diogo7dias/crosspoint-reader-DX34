#include "CustomFont.h"

#include <Utf8.h>

namespace crosspoint {
namespace bdf {

namespace {

constexpr size_t kSlotForStyle(CustomFont::StyleBits style) {
  const bool bold = (style & CustomFont::STYLE_BOLD) != 0;
  const bool italic = (style & CustomFont::STYLE_ITALIC) != 0;
  if (bold && italic) return CustomFont::SLOT_BOLD_ITALIC;
  if (bold) return CustomFont::SLOT_BOLD;
  if (italic) return CustomFont::SLOT_ITALIC;
  return CustomFont::SLOT_REGULAR;
}

}  // namespace

bool CustomFont::openVariant(size_t slot, const char* bdfPath, const char* idxPath, size_t cacheBudgetBytes) {
  if (slot >= 4) return false;
  if (!bdfPath || !idxPath) return false;

  auto src = std::make_unique<CustomFontGlyphSource>();
  if (!src->open(bdfPath, idxPath)) {
    // Failed open. Do not retain the (broken) source so hasVariant reports
    // "no" rather than "pending broken" — this matters for synthetic bold /
    // italic fallback decisions.
    return false;
  }

  // Expand the shared slab to cover this variant's glyph bounding box. The
  // first variant sets the ceiling; subsequent same-or-smaller opens are
  // no-ops (keeps the slab warm for variants opened earlier).
  sharedCache_.ensureMaxBitmapBytes(src->maxBitmapBytes());
  src->setSharedCache(&sharedCache_, static_cast<uint8_t>(slot));

  // Apply the byte-budget exactly once. Later opens must not re-run
  // setCacheBudget — it would reallocate the slab and flush already-cached
  // glyphs of the earlier variants.
  if (!cacheBudgetApplied_) {
    sharedCache_.setCacheBudget(cacheBudgetBytes == 0 ? 1 : cacheBudgetBytes);
    cacheBudgetApplied_ = true;
  }

  variants_[slot] = std::move(src);

  // Update the availability booleans the per-codepoint style resolver reads.
  hasRealBold_ = variants_[SLOT_BOLD] && variants_[SLOT_BOLD]->isOpen();
  hasRealItalic_ = variants_[SLOT_ITALIC] && variants_[SLOT_ITALIC]->isOpen();
  hasRealBoldItalic_ = variants_[SLOT_BOLD_ITALIC] && variants_[SLOT_BOLD_ITALIC]->isOpen();
  return true;
}

void CustomFont::trimCache(size_t slots) {
  sharedCache_.setCacheCap(slots == 0 ? 1 : slots);
}

void CustomFont::releaseCache() { sharedCache_.releaseSlab(); }

bool CustomFont::restoreCache() { return sharedCache_.restoreSlab(); }

CustomFontGlyphSource* CustomFont::getVariant(StyleBits style) const {
  const size_t wanted = kSlotForStyle(style);

  // Exact match.
  if (variants_[wanted] && variants_[wanted]->isOpen()) return variants_[wanted].get();

  // Fallthroughs match EpdFontFamily::getFont priority:
  //   bold+italic → italic → bold → regular
  //   bold        → regular
  //   italic      → regular
  if (wanted == SLOT_BOLD_ITALIC) {
    if (variants_[SLOT_ITALIC] && variants_[SLOT_ITALIC]->isOpen()) return variants_[SLOT_ITALIC].get();
    if (variants_[SLOT_BOLD] && variants_[SLOT_BOLD]->isOpen()) return variants_[SLOT_BOLD].get();
  }
  // regular is the universal fallback.
  if (variants_[SLOT_REGULAR] && variants_[SLOT_REGULAR]->isOpen()) return variants_[SLOT_REGULAR].get();
  return nullptr;
}

uint8_t CustomFont::getSyntheticBoldPasses(StyleBits style) const {
  const bool bold = (style & STYLE_BOLD) != 0;
  if (!bold) return syntheticRegularBoldPasses_;
  const bool italic = (style & STYLE_ITALIC) != 0;
  const bool handledByVariant = italic ? (hasRealBoldItalic_ || hasRealBold_) : hasRealBold_;
  return syntheticRegularBoldPasses_ + (handledByVariant ? 0 : syntheticBoldExtraPasses_);
}

bool CustomFont::shouldSynthesizeItalic(StyleBits style) const {
  const bool italic = (style & STYLE_ITALIC) != 0;
  if (!italic) return false;
  const bool bold = (style & STYLE_BOLD) != 0;
  if (bold) return !(hasRealBoldItalic_ || hasRealItalic_);
  return !hasRealItalic_;
}

const CustomFontGlyphSource::Glyph* CustomFont::resolveGlyph(CustomFontGlyphSource& src, uint32_t cp) {
  if (const auto* g = src.lookup(cp)) return g;
  if (cp != REPLACEMENT_GLYPH) {
    if (const auto* g = src.lookup(REPLACEMENT_GLYPH)) return g;
  }
  return nullptr;
}

int CustomFont::ascender(StyleBits /*style*/) const {
  // Always use the regular variant's ascent. If we returned per-variant
  // metrics the reader would jitter the baseline between runs on the
  // same line whenever a variant's BDF has a different FONT_ASCENT
  // (happens often with third-party bold variants). Built-in
  // EpdFontFamily enforces the same convention by computing metrics
  // from the regular slot.
  auto* v = getVariant(STYLE_REGULAR);
  if (!v) return 0;
  const int a = v->fontAscent();
  if (a > 0) return a;
  return v->fontBbxH();
}

int CustomFont::descender(StyleBits /*style*/) const {
  auto* v = getVariant(STYLE_REGULAR);
  if (!v) return 0;
  const int d = v->fontDescent();
  return d > 0 ? -d : 0;
}

int CustomFont::lineHeight(StyleBits /*style*/) const {
  auto* v = getVariant(STYLE_REGULAR);
  if (!v) return 0;
  const int a = v->fontAscent();
  const int d = v->fontDescent();
  const int sum = (a > 0 ? a : 0) + (d > 0 ? d : 0);
  if (sum > 0) return sum;
  return v->fontBbxH();
}

int CustomFont::getTextWidth(const char* text, StyleBits style) {
  return getTextAdvanceX(text, 0, style);
}

int CustomFont::getTextAdvanceX(const char* text, int letterSpacing, StyleBits style) {
  auto* src = getVariant(style);
  if (!src || text == nullptr || *text == '\0') return 0;
  const uint8_t boldPasses = getSyntheticBoldPasses(style);
  int width = 0;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(text);
  uint32_t cp;
  bool first = true;
  while ((cp = utf8NextCodepoint(&p))) {
    const auto* g = resolveGlyph(*src, cp);
    if (!g) continue;
    if (!first && letterSpacing != 0) width += letterSpacing;
    first = false;
    width += g->advance + boldPasses;
  }
  return width;
}

int CustomFont::getSpaceWidth(StyleBits style) {
  auto* src = getVariant(style);
  if (!src) return 0;
  const uint8_t boldPasses = getSyntheticBoldPasses(style);
  const auto* g = src->lookup(' ');
  if (g) return g->advance + boldPasses;
  const int w = src->fontBbxW();
  return (w > 0 ? w / 2 : 0) + boldPasses;
}

bool CustomFont::hasGlyph(uint32_t cp, StyleBits style) {
  auto* src = getVariant(style);
  return src != nullptr && src->lookup(cp) != nullptr;
}

void CustomFont::prewarmGlyphs(const char* text, StyleBits style) {
  auto* src = getVariant(style);
  if (!src || text == nullptr || *text == '\0') return;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(text);
  uint32_t cp;
  while ((cp = utf8NextCodepoint(&p))) {
    resolveGlyph(*src, cp);
  }
}

void CustomFont::visitGlyphs(const char* text, int letterSpacing, StyleBits style, const GlyphVisitor& visit) {
  auto* src = getVariant(style);
  if (!src || text == nullptr || *text == '\0') return;
  const uint8_t boldPasses = getSyntheticBoldPasses(style);
  const uint8_t* p = reinterpret_cast<const uint8_t*>(text);
  uint32_t cp;
  int cursor = 0;
  bool first = true;
  while ((cp = utf8NextCodepoint(&p))) {
    const auto* g = resolveGlyph(*src, cp);
    if (!g) continue;
    if (!first && letterSpacing != 0) cursor += letterSpacing;
    first = false;
    if (!visit(cursor, *g)) return;
    cursor += g->advance + boldPasses;
  }
}

std::string CustomFont::truncatedText(const char* text, int maxWidth, StyleBits style) {
  if (!text || maxWidth <= 0) return "";
  std::string item = text;
  if (getTextWidth(item.c_str(), style) <= maxWidth) return item;
  const char* ellipsis = "...";
  while (!item.empty() && getTextWidth((item + ellipsis).c_str(), style) >= maxWidth) {
    utf8RemoveLastChar(item);
  }
  return item.empty() ? ellipsis : item + ellipsis;
}

}  // namespace bdf
}  // namespace crosspoint
