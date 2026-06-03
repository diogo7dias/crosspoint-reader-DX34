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
  // Resolve as a CSS-style cascade, outermost to innermost:
  //   1) block CSS base,
  //   2) ancestor depth flags (<b>/<em>/<u>/header) turn the axis ON,
  //   3) inline stack in push order (last/deepest writer wins per axis).
  // Applying the depth flags BEFORE the stack lets a deeper explicit inline
  // setting (e.g. font-weight:normal) override an ancestor's bold — fixing the
  // former OR-merge quirk where inline-normal could not un-bold (RFC #170 step 4).
  bool effBold = cssBaseBold_;
  bool effItalic = cssBaseItalic_;
  bool effUnderline = cssBaseUnderline_;

  if (boldUntilDepth_ < depth) effBold = true;
  if (italicUntilDepth_ < depth) effItalic = true;
  if (underlineUntilDepth_ < depth) effUnderline = true;

  for (const auto& e : stack_) {
    if (e.style.hasBold) effBold = e.style.bold;
    if (e.style.hasItalic) effItalic = e.style.italic;
    if (e.style.hasUnderline) effUnderline = e.style.underline;
  }

  auto style = EpdFontFamily::REGULAR;
  if (effBold) style = static_cast<EpdFontFamily::Style>(style | EpdFontFamily::BOLD);
  if (effItalic) style = static_cast<EpdFontFamily::Style>(style | EpdFontFamily::ITALIC);
  if (effUnderline) style = static_cast<EpdFontFamily::Style>(style | EpdFontFamily::UNDERLINE);
  return style;
}

void StyleResolver::reset() {
  cssBaseBold_ = cssBaseItalic_ = cssBaseUnderline_ = false;
  stack_.clear();
  boldUntilDepth_ = italicUntilDepth_ = underlineUntilDepth_ = INT_MAX;
}
