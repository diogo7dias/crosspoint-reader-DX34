// Metrics-only GfxRenderer shadow for the parse sim. Declares the layout
// metric queries AND the render-path methods that TextBlock/ImageBlock::render
// reference (those render() bodies are pulled into the link by the polymorphic
// vtables but are never CALLED in the sim, so their impls are no-ops). Enums +
// framebuffer/orientation accessors mirror the real class so DirectPixelWriter
// (included by ImageBlock) compiles.
#pragma once

#include <EpdFontFamily.h>

#include <cstdint>

class GfxRenderer {
 public:
  enum RenderMode { BW, GRAYSCALE_LSB, GRAYSCALE_MSB, GRAY2_LSB, GRAY2_MSB };
  enum Orientation { Portrait, LandscapeClockwise, PortraitInverted, LandscapeCounterClockwise };

  // --- layout metrics (real impls in StubRenderer.cpp) ---
  int getTextWidth(int fontId, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getTextWidthSpaced(int fontId, const char* text, int letterSpacing,
                         EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getSpaceWidth(int fontId, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getTextAdvanceX(int fontId, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getTextAdvanceXSpaced(int fontId, const char* text, int letterSpacing,
                            EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getLineHeight(int fontId) const;
  int getFontAscenderSize(int fontId) const;
  bool hasGlyph(int fontId, uint32_t cp, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getScreenWidth() const;
  int getScreenHeight() const;
  uint8_t getTextRenderStyle() const;

  // --- render path (no-op in the sim; linked via vtables, never called) ---
  void drawLine(int x1, int y1, int x2, int y2, bool state = true) const;
  void drawLine(int x1, int y1, int x2, int y2, int lineWidth, bool state) const;
  void drawTextSpaced(int fontId, int x, int y, const char* text, int letterSpacing, bool black = true,
                      EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  uint8_t* getFrameBuffer() const;
  RenderMode getRenderMode() const;
  Orientation getOrientation() const;
};
