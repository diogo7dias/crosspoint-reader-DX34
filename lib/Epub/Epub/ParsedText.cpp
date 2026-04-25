#include "ParsedText.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <vector>

#include "hyphenation/Hyphenator.h"

constexpr int MAX_COST = std::numeric_limits<int>::max();

namespace {
// Soft hyphen byte pattern used throughout EPUBs (UTF-8 for U+00AD).
constexpr char SOFT_HYPHEN_UTF8[] = "\xC2\xAD";
constexpr size_t SOFT_HYPHEN_BYTES = 2;

int wordSpacingSettingToPixelDelta(const uint8_t mode, const int baseSpaceWidth) {
  switch (mode) {
    case 0:  // Tight (-30%)
      return -(baseSpaceWidth * 3 / 10);
    case 2:  // Wide (+80%)
      return (baseSpaceWidth * 4 / 5);
    case 3:  // Very Wide (+150%)
      return (baseSpaceWidth * 3 / 2);
    case 4:  // Extra Wide (+240%)
      return (baseSpaceWidth * 12 / 5);
    case 1:  // Normal (0%)
    default:
      return 0;
  }
}

bool containsSoftHyphen(const std::string& word) { return word.find(SOFT_HYPHEN_UTF8) != std::string::npos; }

// Removes every soft hyphen in-place so rendered glyphs match measured widths.
void stripSoftHyphensInPlace(std::string& word) {
  size_t pos = 0;
  while ((pos = word.find(SOFT_HYPHEN_UTF8, pos)) != std::string::npos) {
    word.erase(pos, SOFT_HYPHEN_BYTES);
  }
}

// Returns the rendered width for a word while ignoring soft hyphen glyphs and
// optionally appending a visible hyphen.
uint16_t measureWordWidth(const GfxRenderer& renderer, const int fontId, const std::string& word,
                          const EpdFontFamily::Style style, const int letterSpacing, const bool appendHyphen = false) {
  if (word.size() == 1 && word[0] == ' ' && !appendHyphen) {
    return renderer.getSpaceWidth(fontId);
  }
  const bool hasSoftHyphen = containsSoftHyphen(word);
  if (!hasSoftHyphen && !appendHyphen) {
    return renderer.getTextWidthSpaced(fontId, word.c_str(), letterSpacing, style);
  }

  std::string sanitized = word;
  if (hasSoftHyphen) {
    stripSoftHyphensInPlace(sanitized);
  }
  if (appendHyphen) {
    sanitized.push_back('-');
  }
  return renderer.getTextWidthSpaced(fontId, sanitized.c_str(), letterSpacing, style);
}

size_t nextUtf8Offset(const std::string& text, const size_t offset) {
  if (offset >= text.size()) {
    return text.size();
  }
  const uint8_t b = static_cast<uint8_t>(text[offset]);
  if ((b & 0x80) == 0) return offset + 1;
  if ((b & 0xE0) == 0xC0) return std::min(text.size(), offset + 2);
  if ((b & 0xF0) == 0xE0) return std::min(text.size(), offset + 3);
  if ((b & 0xF8) == 0xF0) return std::min(text.size(), offset + 4);
  // Invalid leading byte: move forward defensively by one byte.
  return offset + 1;
}

float indentMultiplierForMode(const uint8_t indentMode) {
  switch (indentMode) {
    case 2:
      return 0.6f;
    case 3:
      return 1.0f;
    case 4:
      return 1.4f;
    default:
      return 0.0f;
  }
}

}  // namespace

void ParsedText::addWord(std::string word, const EpdFontFamily::Style fontStyle, const bool underline,
                         const bool attachToPrevious) {
  if (word.empty()) return;

  words.push_back(std::move(word));
  EpdFontFamily::Style combinedStyle = fontStyle;
  if (underline) {
    combinedStyle = static_cast<EpdFontFamily::Style>(combinedStyle | EpdFontFamily::UNDERLINE);
  }
  wordStyles.push_back(combinedStyle);
  wordContinues.push_back(attachToPrevious);
}

// Consumes data to minimize memory usage
void ParsedText::layoutAndExtractLines(const GfxRenderer& renderer, const int fontId, const uint16_t viewportWidth,
                                       const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                                       const bool includeLastLine) {
  if (words.empty()) {
    return;
  }

  // Apply fixed transforms before any per-line layout work.
  applyParagraphIndent(renderer, fontId);

  const int pageWidth = viewportWidth;
  const int baseSpaceWidth = renderer.getSpaceWidth(fontId);
  const int userSpaceWidth =
      std::max(1, baseSpaceWidth + wordSpacingSettingToPixelDelta(wordSpacingPercent, baseSpaceWidth));
  const int spaceWidth = std::max(0, userSpaceWidth + blockStyle.wordSpacing);
  auto wordWidths = calculateWordWidths(renderer, fontId);

  // Build canBreakBefore: by default, can break before any non-continuation word.
  std::vector<bool> canBreakBefore(words.size());
  for (size_t i = 0; i < words.size(); ++i) {
    canBreakBefore[i] = !wordContinues[i];
  }

  // Track which sub-tokens need a visible '-' appended when at end of line.
  std::vector<bool> wordNeedsHyphenAtBreak(words.size(), false);

  if (hyphenationEnabled) {
    expandHyphenationBreaks(renderer, fontId, wordWidths, canBreakBefore, wordNeedsHyphenAtBreak);
  }
  const int firstLineIndent = blockStyle.textIndentDefined && (blockStyle.alignment == CssTextAlign::Justify ||
                                                               blockStyle.alignment == CssTextAlign::Left)
                                  ? blockStyle.textIndent
                                  : 0;
  splitOversizedTokens(renderer, fontId, pageWidth, pageWidth - firstLineIndent, wordWidths, canBreakBefore,
                       wordNeedsHyphenAtBreak);

  std::vector<size_t> lineBreakIndices = computeLineBreaks(renderer, fontId, pageWidth, spaceWidth, wordWidths,
                                                           wordContinues, canBreakBefore, wordNeedsHyphenAtBreak);
  const size_t lineCount = includeLastLine ? lineBreakIndices.size() : lineBreakIndices.size() - 1;

  for (size_t i = 0; i < lineCount; ++i) {
    extractLine(i, pageWidth, spaceWidth, wordWidths, wordContinues, lineBreakIndices, wordNeedsHyphenAtBreak,
                processLine);
  }

  // Remove consumed words so size() reflects only remaining words
  if (lineCount > 0) {
    const size_t consumed = lineBreakIndices[lineCount - 1];
    words.erase(words.begin(), words.begin() + consumed);
    wordStyles.erase(wordStyles.begin(), wordStyles.begin() + consumed);
    wordContinues.erase(wordContinues.begin(), wordContinues.begin() + consumed);
  }
}

void ParsedText::splitOversizedTokens(const GfxRenderer& renderer, const int fontId, const int maxTokenWidth,
                                      const int firstLineMaxTokenWidth, std::vector<uint16_t>& wordWidths,
                                      std::vector<bool>& canBreakBefore, std::vector<bool>& wordNeedsHyphenAtBreak) {
  if (maxTokenWidth <= 0 && firstLineMaxTokenWidth <= 0) {
    return;
  }

  for (size_t i = 0; i < words.size(); ++i) {
    const int tokenMaxWidth = i == 0 ? firstLineMaxTokenWidth : maxTokenWidth;
    if (tokenMaxWidth <= 0 || wordWidths[i] <= tokenMaxWidth || words[i].size() <= 1) {
      continue;
    }

    const std::string original = words[i];
    const auto style = wordStyles[i];
    const bool originalCanBreakBefore = canBreakBefore[i];
    const bool originalNeedsHyphen = wordNeedsHyphenAtBreak[i];

    std::vector<std::string> parts;
    std::vector<uint16_t> partWidths;
    parts.reserve(original.size());
    partWidths.reserve(original.size());

    size_t partStart = 0;
    size_t cursor = 0;
    size_t lastFit = 0;

    while (cursor < original.size()) {
      const size_t next = nextUtf8Offset(original, cursor);
      const std::string candidate = original.substr(partStart, next - partStart);
      const uint16_t candidateWidth = measureWordWidth(renderer, fontId, candidate, style, blockStyle.letterSpacing);

      if (candidateWidth <= tokenMaxWidth) {
        lastFit = next;
        cursor = next;
        continue;
      }

      if (lastFit == partStart) {
        // Single glyph wider than the viewport: force one codepoint so we always progress.
        parts.push_back(original.substr(partStart, next - partStart));
        partWidths.push_back(candidateWidth);
        partStart = next;
        cursor = next;
        lastFit = next;
      } else {
        const std::string fitted = original.substr(partStart, lastFit - partStart);
        parts.push_back(fitted);
        partWidths.push_back(measureWordWidth(renderer, fontId, fitted, style, blockStyle.letterSpacing));
        partStart = lastFit;
        cursor = lastFit;
      }
    }

    if (partStart < original.size()) {
      const std::string tail = original.substr(partStart);
      parts.push_back(tail);
      partWidths.push_back(measureWordWidth(renderer, fontId, tail, style, blockStyle.letterSpacing));
    }

    if (parts.size() < 2) {
      continue;
    }

    words[i] = std::move(parts[0]);
    wordWidths[i] = partWidths[0];
    canBreakBefore[i] = originalCanBreakBefore;
    wordNeedsHyphenAtBreak[i] = false;

    for (size_t p = 1; p < parts.size(); ++p) {
      const size_t insertPos = i + p;
      words.insert(words.begin() + insertPos, std::move(parts[p]));
      wordStyles.insert(wordStyles.begin() + insertPos, style);
      wordContinues.insert(wordContinues.begin() + insertPos, true);
      wordWidths.insert(wordWidths.begin() + insertPos, partWidths[p]);
      canBreakBefore.insert(canBreakBefore.begin() + insertPos, true);
      wordNeedsHyphenAtBreak.insert(wordNeedsHyphenAtBreak.begin() + insertPos, false);
    }

    // Preserve any pre-existing "needs hyphen at break" state on the last piece.
    wordNeedsHyphenAtBreak[i + parts.size() - 1] = originalNeedsHyphen;

    i += parts.size() - 1;
  }
}

std::vector<uint16_t> ParsedText::calculateWordWidths(const GfxRenderer& renderer, const int fontId) {
  std::vector<uint16_t> wordWidths;
  wordWidths.reserve(words.size());

  for (size_t i = 0; i < words.size(); ++i) {
    wordWidths.push_back(measureWordWidth(renderer, fontId, words[i], wordStyles[i], blockStyle.letterSpacing));
  }

  return wordWidths;
}

void ParsedText::expandHyphenationBreaks(const GfxRenderer& renderer, const int fontId,
                                         std::vector<uint16_t>& wordWidths, std::vector<bool>& canBreakBefore,
                                         std::vector<bool>& wordNeedsHyphenAtBreak) {
  // Walk the word list and split hyphenable words into sub-tokens.
  // Process in forward order; new sub-tokens are inserted right after the
  // current index, so we advance past them.
  for (size_t i = 0; i < words.size(); ++i) {
    // Skip very short words and continuation tokens (punctuation attached to
    // previous word) — those should not be independently hyphenated.
    if (words[i].size() < 5 || wordContinues[i]) {
      continue;
    }

    const auto breaks = Hyphenator::breakOffsets(words[i], false);
    if (breaks.empty()) {
      continue;
    }

    // Split this word into (breaks.size() + 1) sub-tokens.
    const std::string original = words[i];
    const EpdFontFamily::Style style = wordStyles[i];

    // Build sub-token strings from break offsets.
    std::vector<std::string> parts;
    std::vector<bool> partNeedsHyphen;
    parts.reserve(breaks.size() + 1);
    partNeedsHyphen.reserve(breaks.size() + 1);

    size_t prev = 0;
    for (const auto& b : breaks) {
      std::string prefix = original.substr(prev, b.byteOffset - prev);
      parts.push_back(std::move(prefix));
      partNeedsHyphen.push_back(b.requiresInsertedHyphen);
      prev = b.byteOffset;
    }
    // Last part (suffix after final break)
    parts.push_back(original.substr(prev));
    partNeedsHyphen.push_back(false);

    if (parts.size() < 2) {
      continue;
    }

    // Replace the original word at position i with the first part.
    words[i] = parts[0];
    wordWidths[i] = measureWordWidth(renderer, fontId, parts[0], style, blockStyle.letterSpacing);
    wordNeedsHyphenAtBreak[i] = partNeedsHyphen[0];
    // canBreakBefore[i] stays as it was (inherits from the original word).

    // Insert remaining parts after position i.
    for (size_t p = 1; p < parts.size(); ++p) {
      const size_t insertPos = i + p;
      const uint16_t w = measureWordWidth(renderer, fontId, parts[p], style, blockStyle.letterSpacing);
      words.insert(words.begin() + insertPos, parts[p]);
      wordWidths.insert(wordWidths.begin() + insertPos, w);
      wordStyles.insert(wordStyles.begin() + insertPos, style);
      wordContinues.insert(wordContinues.begin() + insertPos,
                           true);  // no space before sub-token
      canBreakBefore.insert(canBreakBefore.begin() + insertPos,
                            true);  // but CAN break here (hyphenation point)
      wordNeedsHyphenAtBreak.insert(wordNeedsHyphenAtBreak.begin() + insertPos, partNeedsHyphen[p]);
    }

    // Advance i past all the new sub-tokens so we don't re-process them.
    i += parts.size() - 1;
  }
}

std::vector<size_t> ParsedText::computeLineBreaks(const GfxRenderer& renderer, const int fontId, const int pageWidth,
                                                  const int spaceWidth, std::vector<uint16_t>& wordWidths,
                                                  std::vector<bool>& continuesVec,
                                                  const std::vector<bool>& canBreakBefore,
                                                  const std::vector<bool>& wordNeedsHyphenAtBreak) {
  if (words.empty()) {
    return {};
  }

  // Calculate first line indent (only for left/justified text).
  // Negative text-indent (hanging indent, e.g. margin-left:3em; text-indent:-1em) always applies —
  // it is structural (positions the bullet/marker), not decorative.
  const int firstLineIndent = blockStyle.textIndentDefined && (blockStyle.alignment == CssTextAlign::Justify ||
                                                               blockStyle.alignment == CssTextAlign::Left)
                                  ? blockStyle.textIndent
                                  : 0;

  const size_t totalWordCount = words.size();

  // Pre-compute width of a visible '-' for hyphenation line-end accounting.
  const int hyphenWidth = renderer.getTextWidthSpaced(fontId, "-", blockStyle.letterSpacing, EpdFontFamily::REGULAR);

  // DP table to store the minimum badness (cost) of lines starting at index i
  std::vector<int> dp(totalWordCount);
  // 'ans[i]' stores the index 'j' of the *last word* in the optimal line
  // starting at 'i'
  std::vector<size_t> ans(totalWordCount);

  // Base Case
  dp[totalWordCount - 1] = 0;
  ans[totalWordCount - 1] = totalWordCount - 1;

  for (int i = totalWordCount - 2; i >= 0; --i) {
    int currlen = 0;
    dp[i] = MAX_COST;

    // First line has reduced width due to text-indent
    const int effectivePageWidth = i == 0 ? pageWidth - firstLineIndent : pageWidth;

    for (size_t j = i; j < totalWordCount; ++j) {
      // Add space before word j, unless it's the first word on the line or a
      // continuation
      const int gap = j > static_cast<size_t>(i) && !continuesVec[j] ? spaceWidth : 0;
      currlen += wordWidths[j] + gap;

      if (currlen > effectivePageWidth) {
        break;
      }

      // Cannot break after word j if the next token cannot start a new line
      // (true continuation like punctuation). Hyphenation sub-tokens have
      // canBreakBefore=true so the DP may still break before them.
      if (j + 1 < totalWordCount && !canBreakBefore[j + 1]) {
        continue;
      }

      // Account for visible hyphen width when this word ends the line.
      const int lineEndWidth = currlen + (wordNeedsHyphenAtBreak[j] ? hyphenWidth : 0);

      int cost;
      if (j == totalWordCount - 1) {
        cost = 0;  // Last line
      } else {
        if (lineEndWidth > effectivePageWidth && j > static_cast<size_t>(i)) {
          // Trailing hyphen pushes line over — skip this break point
          // (unless it's the only word, handled by oversized-word fallback).
          continue;
        }
        const int remainingSpace = effectivePageWidth - lineEndWidth;
        // Use long long for the square to prevent overflow
        const long long cost_ll = static_cast<long long>(remainingSpace) * remainingSpace + dp[j + 1];

        if (cost_ll > MAX_COST) {
          cost = MAX_COST;
        } else {
          cost = static_cast<int>(cost_ll);
        }
      }

      if (cost < dp[i]) {
        dp[i] = cost;
        ans[i] = j;  // j is the index of the last word in this optimal line
      }
    }

    // Handle oversized word: if no valid configuration found, force single-word
    // line This prevents cascade failure where one oversized word breaks all
    // preceding words
    if (dp[i] == MAX_COST) {
      ans[i] = i;  // Just this word on its own line
      // Inherit cost from next word to allow subsequent words to find valid
      // configurations
      if (i + 1 < static_cast<int>(totalWordCount)) {
        dp[i] = dp[i + 1];
      } else {
        dp[i] = 0;
      }
    }
  }

  // Stores the index of the word that starts the next line (last_word_index +
  // 1)
  std::vector<size_t> lineBreakIndices;
  size_t currentWordIndex = 0;

  while (currentWordIndex < totalWordCount) {
    size_t nextBreakIndex = ans[currentWordIndex] + 1;

    // Safety check: prevent infinite loop if nextBreakIndex doesn't advance
    if (nextBreakIndex <= currentWordIndex) {
      // Force advance by at least one word to avoid infinite loop
      nextBreakIndex = currentWordIndex + 1;
    }

    lineBreakIndices.push_back(nextBreakIndex);
    currentWordIndex = nextBreakIndex;
  }

  return lineBreakIndices;
}

void ParsedText::applyParagraphIndent(const GfxRenderer& renderer, const int fontId) {
  if (words.empty()) {
    return;
  }

  if (firstLineIndentMode == 1) {
    blockStyle.textIndent = 0;
    blockStyle.textIndentDefined = true;
    return;
  }

  // Measure em-width from the em-space glyph; fall back to line height
  // (≈ font size) when the font lacks U+2003 (e.g. Vollkorn).
  // Note: getTextAdvanceX falls back to the replacement glyph (U+FFFD) when
  // the requested glyph is missing, returning a wrong positive value.
  // We must check hasGlyph first to detect missing em-space.
  int emWidth = 0;
  if (renderer.hasGlyph(fontId, 0x2003)) {
    emWidth = renderer.getTextAdvanceX(fontId, "\xe2\x80\x83");
  }
  if (emWidth <= 0) {
    emWidth = renderer.getLineHeight(fontId);
  }

  const float forcedIndentMultiplier = indentMultiplierForMode(firstLineIndentMode);
  if (forcedIndentMultiplier > 0.0f) {
    blockStyle.textIndent = static_cast<int16_t>(std::lround(static_cast<float>(emWidth) * forcedIndentMultiplier));
    blockStyle.textIndentDefined = true;
    return;
  }

  if (blockStyle.textIndentDefined && usePublisherStyles) {
    // CSS text-indent is explicitly set (even if 0) - don't use fallback
    // The actual indent positioning is handled in extractLine()
  } else if (blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::Left) {
    // No CSS text-indent defined - use textIndent fallback for visual indent
    // (previously prepended em-space char, but that breaks for fonts missing
    // the U+2003 glyph)
    blockStyle.textIndent = static_cast<int16_t>(emWidth);
    blockStyle.textIndentDefined = true;
  }
}

void ParsedText::extractLine(const size_t breakIndex, const int pageWidth, const int spaceWidth,
                             const std::vector<uint16_t>& wordWidths, const std::vector<bool>& continuesVec,
                             const std::vector<size_t>& lineBreakIndices,
                             const std::vector<bool>& wordNeedsHyphenAtBreak,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine) {
  const size_t lineBreak = lineBreakIndices[breakIndex];
  const size_t lastBreakAt = breakIndex > 0 ? lineBreakIndices[breakIndex - 1] : 0;
  const size_t lineWordCount = lineBreak - lastBreakAt;

  // Calculate first line indent (only for left/justified text).
  // Negative text-indent (hanging indent) always applies — it is structural.
  const bool isFirstLine = breakIndex == 0;
  const int firstLineIndent =
      isFirstLine && blockStyle.textIndentDefined &&
              (blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::Left)
          ? blockStyle.textIndent
          : 0;

  // Calculate total word width for this line and count actual word gaps
  // (continuation words attach to previous word with no gap)
  int lineWordWidthSum = 0;
  size_t actualGapCount = 0;

  for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
    lineWordWidthSum += wordWidths[lastBreakAt + wordIdx];
    // Count gaps: each word after the first creates a gap, unless it's a
    // continuation
    if (wordIdx > 0 && !continuesVec[lastBreakAt + wordIdx]) {
      actualGapCount++;
    }
  }

  // Calculate spacing (account for indent reducing effective page width on
  // first line)
  const int effectivePageWidth = pageWidth - firstLineIndent;
  const int spareSpace = effectivePageWidth - lineWordWidthSum;

  int spacing = spaceWidth;
  int spacingRemainder = 0;
  const bool isLastLine = breakIndex == lineBreakIndices.size() - 1;

  // For justified text, distribute spare space across gaps.
  // Use Bresenham-style remainder distribution so no pixels are lost:
  // each gap gets baseSpacing, and the first `remainder` gaps get +1px.
  if (blockStyle.alignment == CssTextAlign::Justify && !isLastLine && actualGapCount >= 1) {
    spacing = spareSpace / static_cast<int>(actualGapCount);
    spacingRemainder = spareSpace - spacing * static_cast<int>(actualGapCount);
  }

  // Calculate initial x position (first line starts at indent for left/justified text;
  // may be negative for hanging indents, e.g. margin-left:3em; text-indent:-1em).
  auto xpos = static_cast<int16_t>(firstLineIndent);
  if (blockStyle.alignment == CssTextAlign::Right) {
    xpos = spareSpace - static_cast<int>(actualGapCount) * spaceWidth;
  } else if (blockStyle.alignment == CssTextAlign::Center) {
    xpos = (spareSpace - static_cast<int>(actualGapCount) * spaceWidth) / 2;
  }

  // Pre-calculate X positions for words
  // Continuation words attach to the previous word with no space before them
  std::vector<int16_t> lineXPos;
  lineXPos.reserve(lineWordCount);
  int gapIndex = 0;

  for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
    const uint16_t currentWordWidth = wordWidths[lastBreakAt + wordIdx];

    lineXPos.push_back(xpos);

    // Add spacing after this word, unless the next word is a continuation
    const bool nextIsContinuation = wordIdx + 1 < lineWordCount && continuesVec[lastBreakAt + wordIdx + 1];

    if (nextIsContinuation) {
      xpos += currentWordWidth;
    } else {
      const int extra = (gapIndex < spacingRemainder) ? 1 : 0;
      xpos += currentWordWidth + spacing + extra;
      gapIndex++;
    }
  }

  // Build line data by moving from the original vectors using index range
  std::vector<std::string> lineWords(std::make_move_iterator(words.begin() + lastBreakAt),
                                     std::make_move_iterator(words.begin() + lineBreak));
  std::vector<EpdFontFamily::Style> lineWordStyles(wordStyles.begin() + lastBreakAt, wordStyles.begin() + lineBreak);

  for (auto& word : lineWords) {
    if (containsSoftHyphen(word)) {
      stripSoftHyphensInPlace(word);
    }
  }

  // Append visible hyphen to the last word on the line if it needs one
  // (the hyphen is only shown at actual line breaks, not mid-line).
  if (!lineWords.empty() && wordNeedsHyphenAtBreak[lineBreak - 1]) {
    lineWords.back().push_back('-');
  }

  // make_shared throws bad_alloc on OOM, which the firmware build can't
  // catch (exceptions disabled) and panics. Use the explicit form so the
  // call returns a null shared_ptr that processLine handles cleanly.
  auto* rawBlock =
      new (std::nothrow) TextBlock(std::move(lineWords), std::move(lineXPos), std::move(lineWordStyles), blockStyle);
  processLine(std::shared_ptr<TextBlock>(rawBlock));
}
