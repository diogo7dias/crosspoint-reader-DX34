/**
 * @file SectionPageCache.h
 * @brief 3-page sliding window + pending-nav bundle for the reader.
 *
 * Design (RFC #21 Stage 3 — EPUB reader decomposition, code-only):
 *   - Absorbs pageCache[3] + pageCacheSpineIndex + clearPageCache +
 *     getCachedPage + loadAndCachePage + refreshPageCacheWindow from
 *     EpubReaderActivity (lines 115-116, 246-322).
 *   - Also carries PendingNav (pendingPercentJump + pendingSpineProgress +
 *     pendingAnchor, RFC #21 lines 1483-1500) as a passive bundle — the
 *     cache is the natural owner since the block runs exactly when a new
 *     section loads and its cache is empty. Resolution logic stays in the
 *     activity (it needs Section to compute the target page).
 *   - Templated on PageT so host tests can use a stub Page type without
 *     pulling in HalStorage / Epub headers.
 *
 * Not wired into EpubReaderActivity in this stage. Stage 4 integrates
 * behind READER_V2 gate with V1 parallel.
 */
#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace crosspoint {
namespace reader {

struct PendingNav {
  bool hasPercentJump = false;
  float spineProgress = 0.0f;   // 0..1 within the target spine item
  bool hasAnchor = false;
  std::string anchor;           // element id (e.g. "#chap3")

  bool empty() const { return !hasPercentJump && !hasAnchor; }
  void clear() {
    hasPercentJump = false;
    spineProgress = 0.0f;
    hasAnchor = false;
    anchor.clear();
  }
};

template <typename PageT>
class SectionPageCache {
 public:
  using PagePtr = std::shared_ptr<PageT>;
  using PageLoader = std::function<PagePtr(int pageIndex)>;

  static constexpr size_t kWindow = 3;

  // --- Lifecycle ---
  // Attach to a spine. Does not load anything; clears prior entries.
  void attach(int spineIndex) {
    clearEntries_();
    spineIndex_ = spineIndex;
  }

  // Drop all entries and the spine association.
  void detach() {
    clearEntries_();
    spineIndex_ = -1;
  }

  // Same as detach() but preserves spine index. Used on page-load failure
  // to force a clean rebuild without abandoning the current spine.
  void clear() { clearEntries_(); }

  int spineIndex() const { return spineIndex_; }

  // --- Lookup ---
  // Returns the cached page for `pageIndex` on the current spine, or nullptr.
  // Returns nullptr if the cache's spineIndex does not match `expectedSpine`.
  PagePtr get(int pageIndex, int expectedSpine) const {
    if (spineIndex_ != expectedSpine) return {};
    for (const auto& e : entries_) {
      if (e.pageIndex == pageIndex && e.page) return e.page;
    }
    return {};
  }

  bool contains(int pageIndex) const {
    for (const auto& e : entries_) {
      if (e.pageIndex == pageIndex && e.page) return true;
    }
    return false;
  }

  int entryCount() const {
    int n = 0;
    for (const auto& e : entries_) {
      if (e.page) ++n;
    }
    return n;
  }

  // --- Insert ---
  // Adds `pageIndex → page` to the window. If `pageIndex` is already cached,
  // the entry is refreshed in place (no eviction). Otherwise the oldest slot
  // rotates out (entries shift: [1]→[0], [2]→[1], new into [2]).
  void insert(int pageIndex, PagePtr page) {
    if (!page) return;
    for (auto& e : entries_) {
      if (e.pageIndex == pageIndex) {
        e.page = std::move(page);
        return;
      }
    }
    entries_[0] = std::move(entries_[1]);
    entries_[1] = std::move(entries_[2]);
    entries_[2] = Entry{pageIndex, std::move(page)};
  }

  // --- Window slide ---
  // Rebuilds the window centered on `centerPage`. Slots map to
  // [centerPage-1, centerPage, centerPage+1]. Out-of-bounds slots
  // (< 0 or >= sectionPageCount) are left empty.
  //
  // `currentPage` is injected as the centerPage entry (saves a reload).
  // Neighbours are first looked up in the current cache; on miss, `loader`
  // is invoked.
  //
  // If centerPage is out of bounds, cache is cleared.
  void refreshWindow(int centerPage, PagePtr currentPage, int sectionPageCount, const PageLoader& loader) {
    if (centerPage < 0 || centerPage >= sectionPageCount) {
      clearEntries_();
      return;
    }

    std::array<Entry, kWindow> next{};
    const int targets[kWindow] = {centerPage - 1, centerPage, centerPage + 1};

    for (size_t i = 0; i < kWindow; ++i) {
      const int t = targets[i];
      if (t < 0 || t >= sectionPageCount) continue;

      PagePtr p;
      if (t == centerPage) {
        p = currentPage;
      } else {
        p = getRaw_(t);
        if (!p && loader) p = loader(t);
      }
      next[i] = Entry{t, std::move(p)};
    }
    entries_ = std::move(next);
  }

  // --- Pending-nav bundle ---
  void setPending(PendingNav nav) { pending_ = std::move(nav); }
  void clearPending() { pending_.clear(); }
  bool hasPending() const { return !pending_.empty(); }
  const PendingNav& pending() const { return pending_; }
  PendingNav& mutablePending() { return pending_; }

 private:
  struct Entry {
    int pageIndex = -1;
    PagePtr page;
  };

  // Non-spine-checked lookup used during window slide (we may be
  // transitioning and the spine has not yet been re-attached).
  PagePtr getRaw_(int pageIndex) const {
    for (const auto& e : entries_) {
      if (e.pageIndex == pageIndex && e.page) return e.page;
    }
    return {};
  }

  void clearEntries_() {
    for (auto& e : entries_) {
      e.pageIndex = -1;
      e.page.reset();
    }
  }

  std::array<Entry, kWindow> entries_{};
  int spineIndex_ = -1;
  PendingNav pending_{};
};

}  // namespace reader
}  // namespace crosspoint
