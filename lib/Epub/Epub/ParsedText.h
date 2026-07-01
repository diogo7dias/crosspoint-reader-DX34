#pragma once

#include <EpdFontFamily.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "blocks/BlockStyle.h"
#include "blocks/TextBlock.h"
#include "layout/LayoutArena.h"

class GfxRenderer;

class ParsedText {
  // RFC #164 step 4: the accumulation buffer (words/wordStyles/wordContinues)
  // is the unbounded per-paragraph peak. When the LayoutEngine hands ParsedText
  // a viable section LayoutArena at construction, addWord interns each word's
  // bytes into the arena and records a compact handle in arenaWords_ instead of
  // growing these three std::vectors — bounding the buffer to a slice of the
  // pre-allocated arena. The std::vector members below remain the FALLBACK: any
  // case the arena path cannot bound byte-identically (hyphenation / oversized
  // token expansion, which insert mid-buffer; or an arena overflow) migrates
  // the interned words back into these vectors and runs the original layout
  // verbatim. So these vectors are also the golden the arena path is asserted
  // against (test_reader_sim_parse).
  std::vector<std::string> words;
  std::vector<EpdFontFamily::Style> wordStyles;
  std::vector<bool> wordContinues;  // true = word attaches to previous (no space before it)

  // Arena-backed accumulation (active only when useArenaWords_). A handle is 8
  // bytes vs a std::string's 32 + heap payload; bytes are interned in the
  // arena's packed string region.
  struct ArenaWord {
    crosspoint::layout::LayoutArena::Str text;  // interned bytes (off + len)
    EpdFontFamily::Style style;
    bool continues;  // attaches to previous (no space before)
  };
  crosspoint::layout::LayoutArena* arena_ = nullptr;  // section scratch (nullable), owned by the parser
  ArenaWord* arenaWords_ = nullptr;                   // fixed-cap handle array, bump-allocated from arena_
  size_t arenaCount_ = 0;
  size_t arenaCap_ = 0;
  bool useArenaWords_ = false;
  bool arenaRegionActive_ = false;                   // true once wordsMark_ is captured and the word region is held
  crosspoint::layout::LayoutArena::Mark wordsMark_;  // checkpoint to rewind the whole word region
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

  void applyParagraphIndent(const GfxRenderer& renderer, int fontId, int viewportWidth);
  void expandHyphenationBreaks(const GfxRenderer& renderer, int fontId, std::vector<uint16_t>& wordWidths,
                               std::vector<bool>& canBreakBefore, std::vector<bool>& wordNeedsHyphenAtBreak);
  void splitOversizedTokens(const GfxRenderer& renderer, int fontId, int maxTokenWidth, int firstLineMaxTokenWidth,
                            std::vector<uint16_t>& wordWidths, std::vector<bool>& canBreakBefore,
                            std::vector<bool>& wordNeedsHyphenAtBreak);
  // `totalWordCount` is passed explicitly (rather than read from words.size())
  // so the arena-backed path, whose words live in arenaWords_ and not in the
  // `words` vector, can reuse this storage-agnostic DP unchanged.
  std::vector<size_t> computeLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth, int spaceWidth,
                                        size_t totalWordCount, std::vector<uint16_t>& wordWidths,
                                        const std::vector<bool>& continuesVec, const std::vector<bool>& canBreakBefore,
                                        const std::vector<bool>& wordNeedsHyphenAtBreak,
                                        crosspoint::layout::LayoutArena* arena);
  // `getWord`/`getStyle` source the line's bytes/styles. When null, extractLine
  // moves them out of the `words`/`wordStyles` vectors as before; the arena path
  // supplies accessors over arenaWords_ instead. The x-position/justification
  // math is identical for both.
  void extractLine(size_t breakIndex, int pageWidth, int spaceWidth, const std::vector<uint16_t>& wordWidths,
                   const std::vector<bool>& continuesVec, const std::vector<size_t>& lineBreakIndices,
                   const std::vector<bool>& wordNeedsHyphenAtBreak,
                   const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                   const std::function<std::string(size_t)>& getWord = nullptr,
                   const std::function<EpdFontFamily::Style(size_t)>& getStyle = nullptr);
  std::vector<uint16_t> calculateWordWidths(const GfxRenderer& renderer, int fontId);

  // — arena word-store helpers (RFC #164 step 4) —
  bool ensureArenaWords();            // lazily reserve arenaWords_ from arena_; false if it won't fit
  void migrateArenaWordsToVectors();  // copy arenaWords_ -> the std::vectors, drop the arena path
  // Drop the first `consumed` arena words after a partial flush and recompact
  // the interned bytes (re-intern the survivors), mirroring the std::vector
  // path's words.erase(begin, begin+consumed).
  void consumeArenaPrefix(size_t consumed);
  // The arena fast path: lay out arenaWords_ directly when no expansion is
  // needed (no hyphenation, no oversized token). Returns false (no work done)
  // when expansion IS needed or anything overflows, so the caller migrates and
  // runs the std::vector path. Mirrors layoutAndExtractLines' contract.
  bool layoutArenaWords(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                        const std::function<void(std::shared_ptr<TextBlock>)>& processLine, bool includeLastLine);

 public:
  explicit ParsedText(const bool extraParagraphSpacing, const bool hyphenationEnabled = false,
                      const BlockStyle& blockStyle = BlockStyle(), const uint8_t wordSpacingPercent = 1,
                      const uint8_t firstLineIndentMode = 0, const bool usePublisherStyles = true,
                      crosspoint::layout::LayoutArena* arena = nullptr)
      : blockStyle(blockStyle),
        extraParagraphSpacing(extraParagraphSpacing),
        hyphenationEnabled(hyphenationEnabled),
        wordSpacingPercent(wordSpacingPercent),
        firstLineIndentMode(firstLineIndentMode),
        usePublisherStyles(usePublisherStyles),
        arena_(arena) {
    // Defer the rewind-checkpoint capture to the first addWord (ensureArenaWords):
    // the parser constructs this block's engine via reset(new ...) BEFORE the
    // previous block's ParsedText is destroyed, so the arena cursor only sits at
    // the correct reclaimed position once we actually begin feeding words.
    useArenaWords_ = (arena_ != nullptr && arena_->ok());
  }
  // Return the arena word region (LIFO: the next block's ParsedText reuses the
  // bytes). No-op for the vector path or an unused arena.
  ~ParsedText() {
    if (arenaRegionActive_) {
      arena_->rewind(wordsMark_);
    }
  }

  void addWord(std::string word, EpdFontFamily::Style fontStyle, bool underline = false, bool attachToPrevious = false);
  // True if any addWord call skipped its push_back because the heap probe
  // failed. Parser checks this after each addWord to short-circuit the
  // chapter into the recovery path instead of crashing on the next vector
  // growth.
  bool hadOom() const { return oom_; }
  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  BlockStyle& getBlockStyle() { return blockStyle; }
  // Stored word count / emptiness — reflects whichever buffer is live (arena
  // handles or the std::vector). The parser leans on these for the 750-word
  // drain and footnote word-index bookkeeping.
  size_t size() const { return useArenaWords_ ? arenaCount_ : words.size(); }
  bool isEmpty() const { return useArenaWords_ ? arenaCount_ == 0 : words.empty(); }
  // Lays out the buffered words and emits each line via processLine. The
  // section LayoutArena (supplied at construction) backs the word buffer and
  // the line-break DP scratch when viable; otherwise this runs the std::vector
  // path with byte-identical output (RFC #164 steps 3-4).
  void layoutAndExtractLines(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                             bool includeLastLine = true);
};
