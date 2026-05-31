// Shadow GfxRenderer.h for the host reader-sim — METRICS ONLY.
//
// The real lib/GfxRenderer/GfxRenderer.h pulls <HalDisplay.h> + the framebuffer
// and is far too coupled to compile on host. Layout (ParsedText) only needs
// text-measurement queries, so this shadow declares exactly the methods
// ParsedText.cpp calls. The implementation (StubRenderer.cpp) answers them from
// a real built-in EpdFontFamily, so widths are realistic enough to drive line
// breaking. Pixel output is irrelevant to the sim's goals (allocation
// behaviour + timing).
//
// Signatures MUST match the real GfxRenderer for the call sites to compile —
// a mismatch is a compile error, which is the desired safety net.
#pragma once

#include <EpdFontFamily.h>

#include <cstdint>

class GfxRenderer {
 public:
  int getTextWidth(int fontId, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getTextWidthSpaced(int fontId, const char* text, int letterSpacing,
                         EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getSpaceWidth(int fontId, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getTextAdvanceX(int fontId, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getLineHeight(int fontId) const;
  bool hasGlyph(int fontId, uint32_t cp, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
};
