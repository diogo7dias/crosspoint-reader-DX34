#pragma once
/**
 * @file FootnotePlacer.h
 * @brief Pure word-index -> page assignment for footnotes (chapter parser).
 *
 * Extracted from ChapterHtmlSlimParser, which tracked footnote placement with a
 * cumulative `wordsExtractedInBlock` counter plus a `pendingFootnotes`
 * <wordIndex, entry> queue, drained inside addLineToPage and makePages.
 *
 * A footnote anchor is registered at its <a> close with the word index at which
 * it occurs. As lines are laid onto pages the cumulative word count advances;
 * each pending footnote whose index has been reached is emitted (the caller
 * binds it to the current page). A fallback drains any stragglers at block end
 * (the historic "word index equals exact block size" edge).
 *
 * Pure: no Page, no expat, no renderer. Host-testable with synthetic word/
 * footnote/line sequences across page breaks.
 */
#include <functional>
#include <utility>
#include <vector>

#include "../FootnoteEntry.h"

class FootnotePlacer {
 public:
  // Caller binds (number, href) to the current page.
  using EmitFn = std::function<void(const char* number, const char* href)>;

  // Register a footnote anchor at `wordIndex` (cumulative block word count at
  // the <a> close). Was pendingFootnotes.push_back.
  void registerFootnote(int wordIndex, const FootnoteEntry& entry);

  // One laid-out line of `lineWordCount` words. Advances the cumulative counter,
  // then emits every pending footnote whose index has been reached, in
  // registration order, removing them. Was the addLineToPage drain.
  void placeForLine(int lineWordCount, const EmitFn& emit);

  // End-of-block fallback: emit all remaining pending footnotes and clear.
  // Was the makePages tail drain (covers wordIndex == exact block size).
  void drainRemaining(const EmitFn& emit);

  // New text block: reset the cumulative counter (was wordsExtractedInBlock = 0).
  // Pending footnotes are intentionally left untouched, preserving historic
  // behaviour.
  void onNewBlock();

  // Cumulative words laid out so far in the current block (was
  // wordsExtractedInBlock). The caller adds the in-progress block size to form
  // a registration index.
  [[nodiscard]] int extractedWordCount() const { return cumulativeWords_; }
  [[nodiscard]] bool empty() const { return pending_.empty(); }

  void reset();

 private:
  std::vector<std::pair<int, FootnoteEntry>> pending_;
  int cumulativeWords_ = 0;
};
