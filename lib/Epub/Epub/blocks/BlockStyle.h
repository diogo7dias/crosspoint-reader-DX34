#pragma once

#include <algorithm>
#include <cstdint>

#include "Epub/css/CssStyle.h"

/**
 * BlockStyle - Block-level styling properties
 */
struct BlockStyle {
  CssTextAlign alignment = CssTextAlign::Justify;

  // Spacing (in pixels)
  int16_t marginTop = 0;
  int16_t marginBottom = 0;
  int16_t marginLeft = 0;
  int16_t marginRight = 0;
  int16_t paddingTop = 0;     // treated same as margin for rendering
  int16_t paddingBottom = 0;  // treated same as margin for rendering
  int16_t paddingLeft = 0;    // treated same as margin for rendering
  int16_t paddingRight = 0;   // treated same as margin for rendering
  int16_t textIndent = 0;
  int16_t letterSpacing = 0;
  int16_t wordSpacing = 0;
  uint16_t lineHeightPercent = 100;
  bool textIndentDefined = false;  // true if text-indent was explicitly set in CSS
  bool textAlignDefined = false;   // true if text-align was explicitly set in CSS
  bool letterSpacingDefined = false;
  bool wordSpacingDefined = false;
  bool lineHeightDefined = false;

  // Combined horizontal insets (margin + padding)
  [[nodiscard]] int16_t leftInset() const { return marginLeft + paddingLeft; }
  [[nodiscard]] int16_t rightInset() const { return marginRight + paddingRight; }
  [[nodiscard]] int16_t totalHorizontalInset() const { return leftInset() + rightInset(); }
  [[nodiscard]] int resolveLineHeight(const int baseLineHeight) const {
    if (!lineHeightDefined || lineHeightPercent == 100) {
      return baseLineHeight;
    }
    return std::max(1, (baseLineHeight * static_cast<int>(lineHeightPercent)) / 100);
  }

  // Combine with another block style. Useful for parent -> child styles, where the child style should be
  // applied on top of the parent's style to get the combined style.
  BlockStyle getCombinedBlockStyle(const BlockStyle& child) const {
    BlockStyle combinedBlockStyle;

    combinedBlockStyle.marginTop = static_cast<int16_t>(child.marginTop + marginTop);
    combinedBlockStyle.marginBottom = static_cast<int16_t>(child.marginBottom + marginBottom);
    combinedBlockStyle.marginLeft = static_cast<int16_t>(child.marginLeft + marginLeft);
    combinedBlockStyle.marginRight = static_cast<int16_t>(child.marginRight + marginRight);

    combinedBlockStyle.paddingTop = static_cast<int16_t>(child.paddingTop + paddingTop);
    combinedBlockStyle.paddingBottom = static_cast<int16_t>(child.paddingBottom + paddingBottom);
    combinedBlockStyle.paddingLeft = static_cast<int16_t>(child.paddingLeft + paddingLeft);
    combinedBlockStyle.paddingRight = static_cast<int16_t>(child.paddingRight + paddingRight);
    // Text indent: use child's if defined
    if (child.textIndentDefined) {
      combinedBlockStyle.textIndent = child.textIndent;
      combinedBlockStyle.textIndentDefined = true;
    } else {
      combinedBlockStyle.textIndent = textIndent;
      combinedBlockStyle.textIndentDefined = textIndentDefined;
    }
    // Text align: use child's if defined
    if (child.textAlignDefined) {
      combinedBlockStyle.alignment = child.alignment;
      combinedBlockStyle.textAlignDefined = true;
    } else {
      combinedBlockStyle.alignment = alignment;
      combinedBlockStyle.textAlignDefined = textAlignDefined;
    }
    if (child.letterSpacingDefined) {
      combinedBlockStyle.letterSpacing = child.letterSpacing;
      combinedBlockStyle.letterSpacingDefined = true;
    } else {
      combinedBlockStyle.letterSpacing = letterSpacing;
      combinedBlockStyle.letterSpacingDefined = letterSpacingDefined;
    }
    if (child.wordSpacingDefined) {
      combinedBlockStyle.wordSpacing = child.wordSpacing;
      combinedBlockStyle.wordSpacingDefined = true;
    } else {
      combinedBlockStyle.wordSpacing = wordSpacing;
      combinedBlockStyle.wordSpacingDefined = wordSpacingDefined;
    }
    if (child.lineHeightDefined) {
      combinedBlockStyle.lineHeightPercent = child.lineHeightPercent;
      combinedBlockStyle.lineHeightDefined = true;
    } else {
      combinedBlockStyle.lineHeightPercent = lineHeightPercent;
      combinedBlockStyle.lineHeightDefined = lineHeightDefined;
    }
    return combinedBlockStyle;
  }

  // Create a BlockStyle from CSS style properties, resolving CssLength values to pixels
  // emSize is the current font line height, used for em/rem unit conversion
  // paragraphAlignment is the user's paragraphAlignment setting preference
  static BlockStyle fromCssStyle(const CssStyle& cssStyle, const float emSize, const CssTextAlign paragraphAlignment,
                                 const uint16_t viewportWidth = 0) {
    BlockStyle blockStyle;
    const float vw = viewportWidth;
    // Resolve all CssLength values to pixels using the current font's em size and viewport width
    blockStyle.marginTop = cssStyle.marginTop.toPixelsInt16(emSize, vw);
    blockStyle.marginBottom = cssStyle.marginBottom.toPixelsInt16(emSize, vw);
    blockStyle.marginLeft = cssStyle.marginLeft.toPixelsInt16(emSize, vw);
    blockStyle.marginRight = cssStyle.marginRight.toPixelsInt16(emSize, vw);

    blockStyle.paddingTop = cssStyle.paddingTop.toPixelsInt16(emSize, vw);
    blockStyle.paddingBottom = cssStyle.paddingBottom.toPixelsInt16(emSize, vw);
    blockStyle.paddingLeft = cssStyle.paddingLeft.toPixelsInt16(emSize, vw);
    blockStyle.paddingRight = cssStyle.paddingRight.toPixelsInt16(emSize, vw);

    // For textIndent: if it's a percentage we can't resolve (no viewport width),
    // leave textIndentDefined=false so the EmSpace fallback in applyParagraphIndent() is used
    if (cssStyle.hasTextIndent() && cssStyle.textIndent.isResolvable(vw)) {
      blockStyle.textIndent = cssStyle.textIndent.toPixelsInt16(emSize, vw);
      blockStyle.textIndentDefined = true;
    }
    if (cssStyle.hasLetterSpacing()) {
      blockStyle.letterSpacing = cssStyle.letterSpacing.toPixelsInt16(emSize);
      blockStyle.letterSpacingDefined = true;
    }
    if (cssStyle.hasWordSpacing()) {
      blockStyle.wordSpacing = cssStyle.wordSpacing.toPixelsInt16(emSize);
      blockStyle.wordSpacingDefined = true;
    }
    if (cssStyle.hasLineHeight()) {
      float lineHeightPx = 0.0f;
      if (cssStyle.lineHeight.unit == CssUnit::Percent) {
        lineHeightPx = (cssStyle.lineHeight.value * emSize) / 100.0f;
      } else {
        lineHeightPx = cssStyle.lineHeight.toPixels(emSize, vw);
      }
      if (lineHeightPx > 0.0f && emSize > 0.0f) {
        blockStyle.lineHeightPercent =
            static_cast<uint16_t>(std::max(40.0f, std::min(300.0f, (lineHeightPx * 100.0f) / emSize)));
        blockStyle.lineHeightDefined = true;
      }
    }
    blockStyle.textAlignDefined = cssStyle.hasTextAlign();
    // User setting overrides CSS, unless "Book's Style" alignment setting is selected
    if (paragraphAlignment == CssTextAlign::None) {
      blockStyle.alignment = blockStyle.textAlignDefined ? cssStyle.textAlign : CssTextAlign::Justify;
    } else {
      blockStyle.alignment = paragraphAlignment;
    }
    return blockStyle;
  }
};
