#pragma once

#include <EpdFontFamily.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "blocks/BlockStyle.h"
#include "blocks/TextBlock.h"

class GfxRenderer;

class ParsedText {
  std::vector<std::string> words;
  std::vector<EpdFontFamily::Style> wordStyles;
  std::vector<bool> wordContinues;  // true = word attaches to previous (no space before it)
  BlockStyle blockStyle;
  bool extraParagraphSpacing;
  bool hyphenationEnabled;
  uint8_t wordSpacingPercent;
  uint8_t firstLineIndentMode;
  bool usePublisherStyles;
  // Set when addWord's pre-allocation heap probe failed and the push_back
  // was skipped. The parser checks this after every addWord call so a
  // fragmentation-driven OOM during the words[] vector growth aborts the
  // chapter cleanly (parseFailed -> recovery screen) instead of
  // bad_alloc -> terminate -> abort -> reset.
  bool oom_ = false;

  void applyParagraphIndent(const GfxRenderer& renderer, int fontId);
  void expandHyphenationBreaks(const GfxRenderer& renderer, int fontId, std::vector<uint16_t>& wordWidths,
                               std::vector<bool>& canBreakBefore, std::vector<bool>& wordNeedsHyphenAtBreak);
  void splitOversizedTokens(const GfxRenderer& renderer, int fontId, int maxTokenWidth, int firstLineMaxTokenWidth,
                            std::vector<uint16_t>& wordWidths, std::vector<bool>& canBreakBefore,
                            std::vector<bool>& wordNeedsHyphenAtBreak);
  std::vector<size_t> computeLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth, int spaceWidth,
                                        std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                        const std::vector<bool>& canBreakBefore,
                                        const std::vector<bool>& wordNeedsHyphenAtBreak);
  void extractLine(size_t breakIndex, int pageWidth, int spaceWidth, const std::vector<uint16_t>& wordWidths,
                   const std::vector<bool>& continuesVec, const std::vector<size_t>& lineBreakIndices,
                   const std::vector<bool>& wordNeedsHyphenAtBreak,
                   const std::function<void(std::shared_ptr<TextBlock>)>& processLine);
  std::vector<uint16_t> calculateWordWidths(const GfxRenderer& renderer, int fontId);

 public:
  explicit ParsedText(const bool extraParagraphSpacing, const bool hyphenationEnabled = false,
                      const BlockStyle& blockStyle = BlockStyle(), const uint8_t wordSpacingPercent = 1,
                      const uint8_t firstLineIndentMode = 0, const bool usePublisherStyles = true)
      : blockStyle(blockStyle),
        extraParagraphSpacing(extraParagraphSpacing),
        hyphenationEnabled(hyphenationEnabled),
        wordSpacingPercent(wordSpacingPercent),
        firstLineIndentMode(firstLineIndentMode),
        usePublisherStyles(usePublisherStyles) {}
  ~ParsedText() = default;

  void addWord(std::string word, EpdFontFamily::Style fontStyle, bool underline = false, bool attachToPrevious = false);
  // True if any addWord call skipped its push_back because the heap probe
  // failed. Parser checks this after each addWord to short-circuit the
  // chapter into the recovery path instead of crashing on the next vector
  // growth.
  bool hadOom() const { return oom_; }
  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  BlockStyle& getBlockStyle() { return blockStyle; }
  size_t size() const { return words.size(); }
  bool isEmpty() const { return words.empty(); }
  void layoutAndExtractLines(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                             bool includeLastLine = true);
};
