/**
 * @file HighlightController.h
 * @brief Text highlight / quote selection state machine + word-position cache.
 *
 * Design (RFC #21 Stage 2 — EPUB reader decomposition, code-only):
 *   - Swallows the 5 highlight* indices + cursor + start/end page + word cache
 *     currently scattered across EpubReaderActivity.
 *   - Single chokepoint for cache invalidation: onPageChanged(). Fixes the
 *     fragility flagged in MEMORY.md where exit paths + cursor cross-page
 *     moves each manually reset highlightWordCachePage = -1 in 4 separate
 *     places (lines 1791, 1804, 1901, 1954).
 *   - Pure state machine: no GfxRenderer, no Page, no Section. Caller owns
 *     rendering, page mutation, quote extraction (which need those). Caller
 *     passes current page / pageCount / word count via PageContext.
 *   - Word-position cache holds WordPos (6 bytes/word) so render overlay
 *     can draw dashed border + underline without recomputing geometry.
 *     Caller populates via setWordsForPage after layout finishes.
 *
 * Not wired into EpubReaderActivity in this stage. Stage 4 (final)
 * integrates behind READER_V2 gate.
 */
#pragma once

#include <cstdint>
#include <vector>

namespace crosspoint {
namespace reader {

// Word geometry for cursor / underline rendering (matches the legacy
// EpubReaderActivity::WordPos layout: 6 bytes/word).
struct WordPos {
  int16_t x = 0;
  int16_t y = 0;
  int16_t width = 0;
};

struct PageContext {
  int currentPage = 0;
  int pageCount = 0;
  int wordCount = 0;  // words on currentPage per the cache
};

struct MoveOutcome {
  int pageDelta = 0;          // caller adds this to section->currentPage
  bool stateChanged = false;  // caller requestUpdate()
};

class HighlightController {
 public:
  enum class State {
    NONE,
    SELECT_START,
    SELECT_END,
    SHOW_UNDERLINE,
  };

  static constexpr uint32_t kUnderlineTimeoutMs = 3000;

  // --- Lifecycle ---
  // Move state from NONE → SELECT_START, reset cursor/indices, clear cache.
  void enter();

  // Force back to NONE. Post-condition: state()==NONE, cursor/indices zeroed,
  // word cache empty (and shrunk to free memory).
  void exit();

  State state() const { return state_; }

  // --- Word-position cache ---
  // Replace the cache with the geometry for `pageIndex`. Controller treats
  // this as the authoritative word list for cursor math.
  void setWordsForPage(int pageIndex, std::vector<WordPos> words);

  // Force-invalidate the cache (e.g. page turned without render yet). Caller
  // must refresh via setWordsForPage before next moveCursor.
  void invalidateWordCache();

  bool wordCacheValidFor(int pageIndex) const;
  int wordCount() const { return static_cast<int>(words_.size()); }
  const std::vector<WordPos>& words() const { return words_; }
  int cachedPage() const { return wordCachePage_; }

  // --- Cursor / selection indices ---
  int cursorIndex() const { return cursorIndex_; }
  int startSpine() const { return startSpine_; }
  int startPage() const { return startPage_; }
  int startWordIndex() const { return startWordIndex_; }
  int endPage() const { return endPage_; }
  int endWordIndex() const { return endWordIndex_; }

  // --- Input: cursor nav ---
  // Word-by-word cursor move. Direction = ±1.
  // In SELECT_START: clamps to [0, wordCount-1] on current page.
  // In SELECT_END: crosses page boundaries when reaching edge, returning
  // pageDelta=±1. On forward cross-page, endWordIndex = 0; on backward,
  // endWordIndex = INT_MAX (caller re-renders, refreshes cache, clamps).
  MoveOutcome moveCursor(int direction, PageContext ctx);

  // Line-based cursor move using cached word y-positions. If no other line
  // exists in that direction, falls through to moveCursor.
  MoveOutcome moveCursorLine(int direction, PageContext ctx);

  // Confirms selection anchor or end-cursor, advancing state machine.
  // In SELECT_START: capture start position (spine/page/word), jump end
  // cursor to last word on page, advance to SELECT_END.
  // In SELECT_END: request jump back to startPage (pageDelta = startPage-
  // currentPage), invalidate cache, advance to SHOW_UNDERLINE, store
  // nowMs for timeout.
  MoveOutcome confirm(int currentSpine, int currentPage, uint32_t nowMs);

  // --- Render-phase helpers ---
  // Caller calls this after layout produces the page. If the page changed
  // since last render, cache is invalidated so caller knows to rebuild.
  void onPageChanged(int newPage);

  // True iff state is SHOW_UNDERLINE AND now - startMs >= kUnderlineTimeoutMs.
  bool underlineTimedOut(uint32_t nowMs) const;

  uint32_t underlineStartMs() const { return underlineStartMs_; }

 private:
  void clearIndices_();

  State state_ = State::NONE;
  int cursorIndex_ = 0;
  int startSpine_ = -1;
  int startPage_ = -1;
  int startWordIndex_ = -1;
  int endPage_ = -1;
  int endWordIndex_ = -1;
  uint32_t underlineStartMs_ = 0;

  std::vector<WordPos> words_;
  int wordCachePage_ = -1;
};

}  // namespace reader
}  // namespace crosspoint
