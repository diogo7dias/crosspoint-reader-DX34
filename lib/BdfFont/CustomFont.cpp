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
  pendingVariants_[slot].bdfPath = bdfPath ? bdfPath : "";
  pendingVariants_[slot].idxPath = idxPath ? idxPath : "";
  pendingVariants_[slot].cacheBudgetBytes = cacheBudgetBytes == 0 ? 1 : cacheBudgetBytes;
  pendingVariants_[slot].registered = true;
  return true;
}

void CustomFont::trimCache(size_t slots) {
  sharedCache_.setCacheCap(slots == 0 ? 1 : slots);
  for (auto& p : pendingVariants_) {
    if (p.registered) {
      p.cacheBudgetBytes = (slots == 0 ? 1 : slots);
    }
  }
}

CustomFontGlyphSource* CustomFont::resolveSlot_(size_t slot) const {
  if (slot >= 4) return nullptr;
  if (variants_[slot]) return variants_[slot].get();
  if (!pendingVariants_[slot].registered) return nullptr;

  auto src = std::make_unique<CustomFontGlyphSource>();
  const bool ok = src->open(pendingVariants_[slot].bdfPath.c_str(),
                            pendingVariants_[slot].idxPath.c_str());
  // Clear pending flag regardless — a failed open should not retry every
  // render tick (would stall the render task on bad SD / missing file).
  pendingVariants_[slot].registered = false;
  if (!ok) return nullptr;

  // Grow the shared slab if this variant's glyphs are larger than any
  // previously-opened variant. Only the first grow actually reallocates;
  // subsequent same-or-smaller opens are no-ops (keeps existing cache).
  sharedCache_.ensureMaxBitmapBytes(src->maxBitmapBytes());
  src->setSharedCache(&sharedCache_, slot);

  // Apply the byte-budget once per CustomFont. Without this guard, each
  // variant's open would re-run setCacheBudget → flush the cache we just
  // warmed for the prior variant.
  if (!cacheBudgetApplied_) {
    sharedCache_.setCacheBudget(pendingVariants_[slot].cacheBudgetBytes);
    cacheBudgetApplied_ = true;
  }

  variants_[slot] = std::move(src);
  return variants_[slot].get();
}

CustomFontGlyphSource* CustomFont::getVariant(StyleBits style) const {
  const size_t wanted = kSlotForStyle(style);

  if (auto* v = resolveSlot_(wanted)) return v;

  // Fallthroughs match EpdFontFamily::getFont priority. bold+italic →
  // italic → bold → regular.
  if (wanted == SLOT_BOLD_ITALIC) {
    if (auto* v = resolveSlot_(SLOT_ITALIC)) return v;
    if (auto* v = resolveSlot_(SLOT_BOLD)) return v;
  } else if (wanted == SLOT_ITALIC) {
    if (auto* v = resolveSlot_(SLOT_ITALIC)) return v;
  } else if (wanted == SLOT_BOLD) {
    if (auto* v = resolveSlot_(SLOT_BOLD)) return v;
  }
  return resolveSlot_(SLOT_REGULAR);
}

uint8_t CustomFont::getSyntheticBoldPasses(StyleBits style) const {
  const bool bold = (style & STYLE_BOLD) != 0;
  // When a real bold (or bolditalic) variant is installed and resolves for
  // the requested style, no synthetic passes are needed — the bold glyph
  // bitmap IS the weight. Only inflate when we fell back to regular.
  // Resolve through resolveSlot_ so a failed-open pending variant does not
  // report as "available" — otherwise we'd skip synthetic bold and render
  // regular weight instead.
  bool boldHandledByVariant = false;
  if (bold) {
    const bool italic = (style & STYLE_ITALIC) != 0;
    if (italic) {
      boldHandledByVariant = resolveSlot_(SLOT_BOLD_ITALIC) != nullptr ||
                             resolveSlot_(SLOT_BOLD) != nullptr;
    } else {
      boldHandledByVariant = resolveSlot_(SLOT_BOLD) != nullptr;
    }
  }
  return syntheticRegularBoldPasses_ + ((bold && !boldHandledByVariant) ? syntheticBoldExtraPasses_ : 0);
}

bool CustomFont::shouldSynthesizeItalic(StyleBits style) const {
  const bool italic = (style & STYLE_ITALIC) != 0;
  if (!italic) return false;
  const bool bold = (style & STYLE_BOLD) != 0;
  // Match getVariant fallback order for italic resolution.
  if (bold) {
    return resolveSlot_(SLOT_BOLD_ITALIC) == nullptr &&
           resolveSlot_(SLOT_ITALIC) == nullptr;
  }
  return resolveSlot_(SLOT_ITALIC) == nullptr;
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
