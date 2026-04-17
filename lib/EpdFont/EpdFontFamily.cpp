#include "EpdFontFamily.h"

bool EpdFontFamily::readerBoldSwapEnabled = false;

void EpdFontFamily::setReaderBoldSwapEnabled(const bool enabled) { readerBoldSwapEnabled = enabled; }

bool EpdFontFamily::isReaderBoldSwapEnabled() { return readerBoldSwapEnabled; }

EpdFontFamily::Style EpdFontFamily::remapStyleForReaderBoldSwap(const Style style) {
  if (!readerBoldSwapEnabled) {
    return style;
  }

  // Keep all italic styles unchanged.
  if ((style & ITALIC) != 0) {
    return style;
  }

  const bool hasBold = (style & BOLD) != 0;
  if (hasBold) {
    return static_cast<Style>(style & ~BOLD);
  }
  return static_cast<Style>(style | BOLD);
}

const EpdFont* EpdFontFamily::getFont(const Style style) const {
  const Style remappedStyle = remapStyleForReaderBoldSwap(style);
  // Extract font style bits (ignore UNDERLINE bit for font selection)
  const bool hasBold = (remappedStyle & BOLD) != 0;
  const bool hasItalic = (remappedStyle & ITALIC) != 0;

  if (hasBold && hasItalic) {
    if (boldItalic) return boldItalic;
    if (italic) return italic;
    if (bold) return bold;
  } else if (hasBold && bold) {
    return bold;
  } else if (hasItalic && italic) {
    return italic;
  }

  return regular;
}

void EpdFontFamily::getTextDimensions(const char* string, int* w, int* h, const Style style) const {
  getFont(style)->getTextDimensions(string, w, h);
}

bool EpdFontFamily::hasPrintableChars(const char* string, const Style style) const {
  return getFont(style)->hasPrintableChars(string);
}

const EpdFontData* EpdFontFamily::getData(const Style style) const { return getFont(style)->data; }

const EpdGlyph* EpdFontFamily::getGlyph(const uint32_t cp, const Style style) const {
  return getFont(style)->getGlyph(cp);
};

bool EpdFontFamily::hasGlyph(const uint32_t cp, const Style style) const { return getFont(style)->hasGlyph(cp); }

uint8_t EpdFontFamily::getSyntheticBoldPasses(const Style style) const {
  const Style remappedStyle = remapStyleForReaderBoldSwap(style);
  const uint8_t boldExtra = ((remappedStyle & BOLD) != 0) ? syntheticBoldExtraPasses : 0;
  return syntheticRegularBoldPasses + boldExtra;
}

bool EpdFontFamily::shouldSynthesizeItalic(const Style style) const {
  const Style remappedStyle = remapStyleForReaderBoldSwap(style);
  return syntheticItalic && ((remappedStyle & ITALIC) != 0);
}
