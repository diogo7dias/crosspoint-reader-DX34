#pragma once
/**
 * @file StyleResolver.h
 * @brief Pure resolution of effective inline text style for the chapter parser.
 *
 * Extracted verbatim from ChapterHtmlSlimParser, which carried three overlapping
 * style systems inline:
 *   1. currentCssStyle      — block-level CSS base (set on header/block enter).
 *   2. inlineStyleStack     — per-inline-element overrides, last-writer-wins.
 *   3. *UntilDepth flags     — tag-driven bold/italic/underline-from-depth.
 *
 * The effective Style for a word flushed at parser `depth` is, per axis:
 *
 *     isBold = (boldUntilDepth < depth) OR (cssBase-bold THEN overwritten by
 *              each inlineStyleStack entry that hasBold, in push order)
 *
 * Note this is a logical OR between the depth-flag result and the
 * (cssBase ∘ stack) result, NOT an override. That means an inline
 * `font-weight:normal` (a stack entry with bold=false) cannot turn off bold
 * while a shallower depth flag (`<b>`/`<strong>`/header) is active. This is a
 * pre-existing behaviour preserved exactly here (fidelity-first extraction);
 * any change to it is a separate, test-guarded follow-up (RFC #170).
 *
 * Pure: no expat, no GfxRenderer, no Page. Host-testable in isolation.
 */
#include <EpdFontFamily.h>

#include <climits>
#include <vector>

#include "../css/CssStyle.h"

class StyleResolver {
 public:
  // Matches the historic inlineStyleStack cap. A push beyond this is dropped
  // (returns false); the matching pop then also no-ops because back().depth
  // won't equal the closing depth, preserving push/pop symmetry.
  static constexpr size_t MAX_INLINE_STYLE_DEPTH = 64;

  // One inline override layer. hasX distinguishes "this element sets X" from
  // "inherit X"; x is the value when hasX.
  struct InlineStyle {
    bool hasBold = false, bold = false;
    bool hasItalic = false, italic = false;
    bool hasUnderline = false, underline = false;
  };

  // --- block CSS base (was currentCssStyle) ---
  // Extracts bold/italic/underline intent from the resolved block CSS.
  void setCssBase(const CssStyle& cssStyle);
  // Was currentCssStyle.reset(): clears the block base to "no intent".
  void clearCssBase();

  // --- inline override stack (was inlineStyleStack) ---
  // Returns false (and does not push) once MAX_INLINE_STYLE_DEPTH is reached,
  // mirroring the historic size-guarded push.
  bool pushInline(int depth, const InlineStyle& style);
  // Pops the top entry iff it was pushed at exactly `depth`. Returns true if a
  // pop happened.
  bool popInlineAtDepth(int depth);

  // --- depth flags (was boldUntilDepth / italicUntilDepth / underlineUntilDepth) ---
  void setBoldFrom(int depth);  // flag = min(flag, depth)
  void setItalicFrom(int depth);
  void setUnderlineFrom(int depth);
  void clearDepthFlagsAt(int depth);  // clears each flag whose value == depth

  // --- queries ---
  // endElement's pre-flush guard: would closing at `closingDepth` (== depth-1)
  // change any style? (pop a stack entry, or clear a depth flag)
  [[nodiscard]] bool wouldChangeAt(int closingDepth) const;

  // The one resolution. Returns the merged Style for a word flushed at `depth`.
  [[nodiscard]] EpdFontFamily::Style effectiveStyle(int depth) const;

  void reset();

 private:
  struct Entry {
    int depth;
    InlineStyle style;
  };
  bool cssBaseBold_ = false;
  bool cssBaseItalic_ = false;
  bool cssBaseUnderline_ = false;
  std::vector<Entry> stack_;
  int boldUntilDepth_ = INT_MAX;
  int italicUntilDepth_ = INT_MAX;
  int underlineUntilDepth_ = INT_MAX;
};
