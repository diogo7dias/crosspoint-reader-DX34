#include "HighlightController.h"

#include <algorithm>
#include <climits>
#include <cstdlib>

namespace crosspoint {
namespace reader {

void HighlightController::clearIndices_() {
  cursorIndex_ = 0;
  startSpine_ = -1;
  startPage_ = -1;
  startWordIndex_ = -1;
  endPage_ = -1;
  endWordIndex_ = -1;
  underlineStartMs_ = 0;
}

void HighlightController::enter() {
  state_ = State::SELECT_START;
  clearIndices_();
  wordCachePage_ = -1;
  words_.clear();
  cursorIndex_ = 0;
}

void HighlightController::exit() {
  state_ = State::NONE;
  clearIndices_();
  words_.clear();
  words_.shrink_to_fit();  // mirrors legacy free-on-exit behaviour
  wordCachePage_ = -1;
}

void HighlightController::setWordsForPage(int pageIndex, std::vector<WordPos> words) {
  words_ = std::move(words);
  wordCachePage_ = pageIndex;
}

void HighlightController::invalidateWordCache() {
  wordCachePage_ = -1;
  // NB: keep words_ populated — render will overwrite via setWordsForPage.
  // Callers that need the storage released should call exit().
}

bool HighlightController::wordCacheValidFor(int pageIndex) const { return wordCachePage_ == pageIndex; }

void HighlightController::onPageChanged(int newPage) {
  if (wordCachePage_ != newPage) {
    wordCachePage_ = -1;
  }
}

bool HighlightController::underlineTimedOut(uint32_t nowMs) const {
  if (state_ != State::SHOW_UNDERLINE) return false;
  return (nowMs - underlineStartMs_) >= kUnderlineTimeoutMs;
}

MoveOutcome HighlightController::moveCursor(int direction, PageContext ctx) {
  MoveOutcome out{};

  if (state_ == State::SELECT_START) {
    if (ctx.wordCount == 0) return out;
    int newIndex = cursorIndex_ + direction;
    if (newIndex < 0) newIndex = 0;
    if (newIndex >= ctx.wordCount) newIndex = ctx.wordCount - 1;
    if (newIndex != cursorIndex_) {
      cursorIndex_ = newIndex;
      out.stateChanged = true;
    }
    return out;
  }

  if (state_ != State::SELECT_END) return out;

  // Clamp stale end index against current page's word count.
  if (endWordIndex_ >= ctx.wordCount) {
    endWordIndex_ = ctx.wordCount > 0 ? ctx.wordCount - 1 : 0;
  }

  int newIndex = endWordIndex_ + direction;

  // Forward past last word on page → next page if available.
  if (newIndex >= ctx.wordCount) {
    if (ctx.currentPage < ctx.pageCount - 1) {
      out.pageDelta = +1;
      endPage_ = ctx.currentPage + 1;
      wordCachePage_ = -1;  // force rebuild on next render
      endWordIndex_ = 0;
      out.stateChanged = true;
      return out;
    }
    newIndex = ctx.wordCount > 0 ? ctx.wordCount - 1 : 0;
  }

  // Backward past first word → previous page if we're past the start page.
  if (newIndex < 0) {
    if (ctx.currentPage > startPage_) {
      out.pageDelta = -1;
      endPage_ = ctx.currentPage - 1;
      wordCachePage_ = -1;
      // We don't know the new page's word count yet; set to INT_MAX and let
      // the next render / clamp pull it back to wordCount-1.
      endWordIndex_ = INT_MAX;
      out.stateChanged = true;
      return out;
    }
    if (ctx.currentPage == startPage_) {
      newIndex = startWordIndex_;
    } else {
      newIndex = 0;
    }
  }

  // On the start page, end cursor can't go before start word.
  if (ctx.currentPage == startPage_ && newIndex < startWordIndex_) {
    newIndex = startWordIndex_;
  }

  if (newIndex != endWordIndex_) {
    endWordIndex_ = newIndex;
    out.stateChanged = true;
  }
  return out;
}

MoveOutcome HighlightController::moveCursorLine(int direction, PageContext ctx) {
  MoveOutcome out{};
  if (words_.empty()) return out;
  if (state_ != State::SELECT_START && state_ != State::SELECT_END) return out;

  const bool isEnd = (state_ == State::SELECT_END);
  const int curIdx = isEnd ? endWordIndex_ : cursorIndex_;
  if (curIdx < 0 || curIdx >= static_cast<int>(words_.size())) return out;

  const int curY = words_[curIdx].y;
  const int curX = words_[curIdx].x;

  int targetY = -1;
  if (direction < 0) {
    for (const auto& w : words_) {
      if (w.y < curY && (targetY < 0 || w.y > targetY)) targetY = w.y;
    }
  } else {
    for (const auto& w : words_) {
      if (w.y > curY && (targetY < 0 || w.y < targetY)) targetY = w.y;
    }
  }

  if (targetY < 0) {
    // No other line exists in that direction — fall through to word-level move,
    // which handles cross-page transitions.
    return moveCursor(direction, ctx);
  }

  int bestIdx = -1;
  int bestDist = INT_MAX;
  for (int i = 0; i < static_cast<int>(words_.size()); i++) {
    if (words_[i].y == targetY) {
      int dist = std::abs(words_[i].x - curX);
      if (dist < bestDist) {
        bestDist = dist;
        bestIdx = i;
      }
    }
  }
  if (bestIdx < 0) return out;

  if (isEnd && ctx.currentPage == startPage_ && bestIdx < startWordIndex_) {
    bestIdx = startWordIndex_;
  }

  if (isEnd) {
    if (bestIdx != endWordIndex_) {
      endWordIndex_ = bestIdx;
      out.stateChanged = true;
    }
  } else {
    if (bestIdx != cursorIndex_) {
      cursorIndex_ = bestIdx;
      out.stateChanged = true;
    }
  }
  return out;
}

MoveOutcome HighlightController::confirm(int currentSpine, int currentPage, uint32_t nowMs) {
  MoveOutcome out{};

  if (state_ == State::SELECT_START) {
    startSpine_ = currentSpine;
    startPage_ = currentPage;
    startWordIndex_ = cursorIndex_;
    endPage_ = currentPage;
    endWordIndex_ = wordCount() - 1;
    if (endWordIndex_ < 0) endWordIndex_ = 0;
    state_ = State::SELECT_END;
    out.stateChanged = true;
    return out;
  }

  if (state_ == State::SELECT_END) {
    out.pageDelta = startPage_ - currentPage;
    wordCachePage_ = -1;
    state_ = State::SHOW_UNDERLINE;
    underlineStartMs_ = nowMs;
    out.stateChanged = true;
    return out;
  }

  return out;
}

}  // namespace reader
}  // namespace crosspoint
