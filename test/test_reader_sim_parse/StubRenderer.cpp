// Metrics-only GfxRenderer for the parse sim (layout widths + line/ascender
// heights) from a real built-in font. No pixels.
#include <EpdFont.h>
#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <ui_10_regular.h>

#include <cstdint>

namespace {
const EpdFont g_regular(&ui_10_regular);
const EpdFontFamily g_family(&g_regular);
size_t cpCount(const char* s) {
  size_t n = 0;
  for (; *s; ++s)
    if ((static_cast<unsigned char>(*s) & 0xC0) != 0x80) ++n;
  return n;
}
}  // namespace

int GfxRenderer::getTextWidth(int, const char* text, EpdFontFamily::Style style) const {
  int w = 0, h = 0;
  g_family.getTextDimensions(text, &w, &h, style);
  return w;
}
int GfxRenderer::getTextWidthSpaced(int fontId, const char* text, int ls, EpdFontFamily::Style style) const {
  int base = getTextWidth(fontId, text, style);
  const size_t n = cpCount(text);
  if (n > 0 && ls != 0) base += ls * static_cast<int>(n);
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
int GfxRenderer::getFontAscenderSize(int) const {
  const EpdFontData* d = g_family.getData(EpdFontFamily::REGULAR);
  return d ? d->ascender : 12;
}
bool GfxRenderer::hasGlyph(int, uint32_t cp, EpdFontFamily::Style style) const { return g_family.hasGlyph(cp, style); }

// --- render-path no-ops (linked via TextBlock/ImageBlock vtables, never called) ---
int GfxRenderer::getScreenWidth() const { return 600; }
int GfxRenderer::getScreenHeight() const { return 800; }
uint8_t GfxRenderer::getTextRenderStyle() const { return 0; }
int GfxRenderer::getTextAdvanceXSpaced(int fontId, const char* text, int ls, EpdFontFamily::Style style) const {
  return getTextWidthSpaced(fontId, text, ls, style);
}
void GfxRenderer::drawLine(int, int, int, int, bool) const {}
void GfxRenderer::drawLine(int, int, int, int, int, bool) const {}
void GfxRenderer::drawTextSpaced(int, int, int, const char*, int, bool, EpdFontFamily::Style) const {}
uint8_t* GfxRenderer::getFrameBuffer() const {
  static uint8_t fb[600 / 8 * 800] = {};
  return fb;
}
GfxRenderer::RenderMode GfxRenderer::getRenderMode() const { return BW; }
GfxRenderer::Orientation GfxRenderer::getOrientation() const { return Portrait; }
