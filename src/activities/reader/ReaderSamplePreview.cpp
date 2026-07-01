#include "ReaderSamplePreview.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <string>
#include <vector>

#include "CrossPointSettings.h"

namespace reader {
namespace {
// Two short paragraphs so paragraph spacing is visible; caps / descenders /
// punctuation exercise the face.
constexpr const char* kSample =
    "The morning light fell across the quiet room, and for a moment the whole "
    "world seemed to pause.\n"
    "She turned the page slowly, savoring each word, while the old clock ticked "
    "on and the rain traced soft lines down the glass.";
}  // namespace

void drawReaderSamplePreview(GfxRenderer& renderer, const CrossPointSettings& s, const int boxX, const int boxY,
                             const int boxW, const int boxH) {
  const int fontId = s.getReaderFontId();
  const int lineAdv = std::max(1, renderer.getLineHeight(fontId) * s.lineSpacingPercent / 100);

  constexpr int kPad = 8;
  // Apply the theme's horizontal margin as a capped inset so wider-margin themes
  // read visibly narrower inside the box.
  const int usableW = std::max(1, boxW - 2 * kPad);
  const int margin = std::clamp<int>(s.screenMarginHorizontal, 0, usableW / 3);
  const int insetL = boxX + kPad + margin;
  const int colW = std::max(1, boxW - 2 * (kPad + margin));

  int em = renderer.hasGlyph(fontId, 0x2003) ? renderer.getTextAdvanceX(fontId, "\xE2\x80\x83") : 0;
  if (em <= 0) em = renderer.getLineHeight(fontId);
  int indent = em;  // Book (0) / default ~ 1 em
  switch (s.firstLineIndentMode) {
    case 1: indent = 0; break;                            // Off
    case 2: indent = static_cast<int>(em * 0.6f); break;  // Small
    case 3: indent = em; break;                           // Medium
    case 4: indent = static_cast<int>(em * 1.4f); break;  // Large
    case 5: indent = std::min(std::max(colW / 4, em), colW * 3 / 5); break;  // Mega
    default: break;
  }

  const uint8_t align = s.paragraphAlignment;  // 0 Justify, 1 Left, 2 Center, 3 Right, 4 Book(=justify)
  const uint8_t prevStyle = renderer.getTextRenderStyle();
  renderer.setTextRenderStyle(CrossPointSettings::renderStyleForTextMode(s.textRenderMode));

  const int bandBottom = boxY + boxH - kPad;
  const int baseSpaceW = std::max(1, renderer.getTextWidth(fontId, " "));
  int wsDelta = 0;
  switch (s.wordSpacingPercent) {
    case 0: wsDelta = -(baseSpaceW * 3 / 10); break;
    case 2: wsDelta = baseSpaceW * 2 / 5; break;
    case 3: wsDelta = baseSpaceW * 4 / 5; break;
    case 4: wsDelta = baseSpaceW * 23 / 20; break;
    case 5: wsDelta = baseSpaceW * 3 / 2; break;
    case 6: wsDelta = baseSpaceW * 39 / 20; break;
    case 7: wsDelta = baseSpaceW * 12 / 5; break;
    case 8: wsDelta = baseSpaceW * 3; break;
    default: break;  // 1 = Normal
  }
  const int spaceW = std::max(1, baseSpaceW + wsDelta);
  static constexpr float kParaFrac[] = {0.f, 0.20f, 0.30f, 0.42f, 0.55f, 0.68f, 0.80f};
  const int paraGap =
      static_cast<int>(renderer.getLineHeight(fontId) * kParaFrac[std::min<int>(s.extraParagraphSpacingLevel, 6)]);

  int y = boxY + kPad;
  int lineIndent = indent;
  std::vector<std::string> lineWords;
  std::string word;

  auto drawLine = [&](bool isLast) {
    if (lineWords.empty()) return;
    const int avail = colW - lineIndent;
    int wordsW = 0;
    for (const auto& w : lineWords) wordsW += renderer.getTextWidth(fontId, w.c_str());
    const int gaps = static_cast<int>(lineWords.size()) - 1;
    const int naturalW = wordsW + spaceW * gaps;
    const bool justify = (align == 0 || align == 4) && !isLast && gaps > 0;
    int x = insetL + lineIndent;
    if (align == 2) x += std::max(0, (avail - naturalW) / 2);  // center
    else if (align == 3) x += std::max(0, avail - naturalW);   // right
    const int extra = justify ? std::max(0, avail - naturalW) : 0;
    for (size_t i = 0; i < lineWords.size(); ++i) {
      renderer.drawText(fontId, x, y, lineWords[i].c_str(), true, EpdFontFamily::REGULAR);
      x += renderer.getTextWidth(fontId, lineWords[i].c_str());
      if (static_cast<int>(i) < gaps) {
        x += spaceW;
        if (justify) x += extra / gaps + (static_cast<int>(i) < extra % gaps ? 1 : 0);
      }
    }
  };
  auto emit = [&](const std::string& w) {
    std::string cand;
    for (const auto& lw : lineWords) {
      cand += lw;
      cand += ' ';
    }
    cand += w;
    if (lineWords.empty() || renderer.getTextWidth(fontId, cand.c_str()) <= colW - lineIndent) {
      lineWords.push_back(w);
    } else {
      drawLine(false);
      y += lineAdv;
      lineIndent = 0;
      lineWords.clear();
      lineWords.push_back(w);
    }
  };
  for (const char* p = kSample; *p && y <= bandBottom; ++p) {
    if (*p == '\n') {  // paragraph break
      if (!word.empty()) {
        emit(word);
        word.clear();
      }
      if (y <= bandBottom) drawLine(true);
      y += lineAdv + paraGap;
      lineIndent = indent;
      lineWords.clear();
    } else if (*p == ' ') {
      if (!word.empty()) {
        emit(word);
        word.clear();
      }
    } else {
      word += *p;
    }
  }
  if (!word.empty() && y <= bandBottom) emit(word);
  if (y <= bandBottom) drawLine(true);

  renderer.setTextRenderStyle(prevStyle);
}

}  // namespace reader
