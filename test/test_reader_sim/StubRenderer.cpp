// Metrics-only GfxRenderer implementation for the host reader-sim.
// Answers text-measurement queries from a real built-in EpdFontFamily so
// ParsedText's line breaking sees realistic widths.
#include <EpdFont.h>
#include <EpdFontFamily.h>
#include <GfxRenderer.h>    // the shadow (sim_shadows is first on the include path)
#include <ui_16_regular.h>  // -I lib/EpdFont/builtinFonts; defines static const EpdFontData

#include <cstdint>

namespace {
// Built-in uncompressed font (groups == nullptr) — glyph advances come straight
// from the flash-resident tables, no decompression needed on host. The UI Pixel
// Operator face is uncompressed; reader fonts are --compress so unusable here.
const EpdFont g_regular(&ui_16_regular);
const EpdFontFamily g_family(&g_regular);

// Count UTF-8 codepoints (lead bytes) for letter-spacing accounting.
size_t cpCount(const char* s) {
  size_t n = 0;
  for (; *s; ++s) {
    if ((static_cast<unsigned char>(*s) & 0xC0) != 0x80) ++n;
  }
  return n;
}
}  // namespace

int GfxRenderer::getTextWidth(int, const char* text, EpdFontFamily::Style style) const {
  int w = 0, h = 0;
  g_family.getTextDimensions(text, &w, &h, style);
  return w;
}

int GfxRenderer::getTextWidthSpaced(int fontId, const char* text, int letterSpacing, EpdFontFamily::Style style) const {
  int base = getTextWidth(fontId, text, style);
  const size_t n = cpCount(text);
  if (n > 0 && letterSpacing != 0) base += letterSpacing * static_cast<int>(n);
  return base;
}

int GfxRenderer::getSpaceWidth(int, EpdFontFamily::Style style) const {
  int w = 0, h = 0;
  g_family.getTextDimensions(" ", &w, &h, style);
  return w;
}

int GfxRenderer::getTextAdvanceX(int fontId, const char* text, EpdFontFamily::Style style) const {
  return getTextWidth(fontId, text, style);
}

int GfxRenderer::getLineHeight(int) const {
  const EpdFontData* d = g_family.getData(EpdFontFamily::REGULAR);
  return d ? d->advanceY : 16;
}

bool GfxRenderer::hasGlyph(int, uint32_t cp, EpdFontFamily::Style style) const { return g_family.hasGlyph(cp, style); }
