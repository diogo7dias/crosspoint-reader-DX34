#include "StyleResolver.h"

#include <algorithm>

void StyleResolver::setCssBase(const CssStyle& cssStyle) {
  // Mirrors updateEffectiveInlineStyle's base: derive the three axes from the
  // resolved block CSS. Only the presence+value of these properties matters.
  cssBaseBold_ = cssStyle.hasFontWeight() && cssStyle.fontWeight == CssFontWeight::Bold;
  cssBaseItalic_ = cssStyle.hasFontStyle() && cssStyle.fontStyle == CssFontStyle::Italic;
  cssBaseUnderline_ = cssStyle.hasTextDecoration() && cssStyle.textDecoration == CssTextDecoration::Underline;
}

void StyleResolver::clearCssBase() {
  // == currentCssStyle.reset(): hasX() all become false -> base has no intent.
  cssBaseBold_ = false;
  cssBaseItalic_ = false;
  cssBaseUnderline_ = false;
}

bool StyleResolver::pushInline(int depth, const InlineStyle& style) {
  if (stack_.size() >= MAX_INLINE_STYLE_DEPTH) {
    return false;
  }
  stack_.push_back({depth, style});
  return true;
}

bool StyleResolver::popInlineAtDepth(int depth) {
  if (!stack_.empty() && stack_.back().depth == depth) {
    stack_.pop_back();
    return true;
  }
  return false;
}

void StyleResolver::setBoldFrom(int depth) { boldUntilDepth_ = std::min(boldUntilDepth_, depth); }
void StyleResolver::setItalicFrom(int depth) { italicUntilDepth_ = std::min(italicUntilDepth_, depth); }
void StyleResolver::setUnderlineFrom(int depth) { underlineUntilDepth_ = std::min(underlineUntilDepth_, depth); }

void StyleResolver::clearDepthFlagsAt(int depth) {
  if (boldUntilDepth_ == depth) boldUntilDepth_ = INT_MAX;
  if (italicUntilDepth_ == depth) italicUntilDepth_ = INT_MAX;
  if (underlineUntilDepth_ == depth) underlineUntilDepth_ = INT_MAX;
}

bool StyleResolver::wouldChangeAt(int closingDepth) const {
  const bool willPopStyleStack = !stack_.empty() && stack_.back().depth == closingDepth;
  const bool willClearBold = boldUntilDepth_ == closingDepth;
  const bool willClearItalic = italicUntilDepth_ == closingDepth;
  const bool willClearUnderline = underlineUntilDepth_ == closingDepth;
  return willPopStyleStack || willClearBold || willClearItalic || willClearUnderline;
}

EpdFontFamily::Style StyleResolver::effectiveStyle(int depth) const {
  // 1) block CSS base, 2) inline stack in push order (last-writer-wins per axis).
  bool effBold = cssBaseBold_;
  bool effItalic = cssBaseItalic_;
  bool effUnderline = cssBaseUnderline_;
  for (const auto& e : stack_) {
    if (e.style.hasBold) effBold = e.style.bold;
    if (e.style.hasItalic) effItalic = e.style.italic;
    if (e.style.hasUnderline) effUnderline = e.style.underline;
  }

  // 3) OR with the depth flags (preserves the historic flushPartWordBuffer
  // merge, including the font-weight:normal-cannot-un-bold quirk).
  const bool isBold = boldUntilDepth_ < depth || effBold;
  const bool isItalic = italicUntilDepth_ < depth || effItalic;
  const bool isUnderline = underlineUntilDepth_ < depth || effUnderline;

  auto style = EpdFontFamily::REGULAR;
  if (isBold) style = static_cast<EpdFontFamily::Style>(style | EpdFontFamily::BOLD);
  if (isItalic) style = static_cast<EpdFontFamily::Style>(style | EpdFontFamily::ITALIC);
  if (isUnderline) style = static_cast<EpdFontFamily::Style>(style | EpdFontFamily::UNDERLINE);
  return style;
}

void StyleResolver::reset() {
  cssBaseBold_ = cssBaseItalic_ = cssBaseUnderline_ = false;
  stack_.clear();
  boldUntilDepth_ = italicUntilDepth_ = underlineUntilDepth_ = INT_MAX;
}
