#pragma once
#include <Epub.h>

#include <memory>
#include <vector>

#include "../ActivityWithSubactivity.h"
#include "util/ButtonNavigator.h"

class EpubReaderChapterSelectionActivity final : public ActivityWithSubactivity {
  std::shared_ptr<Epub> epub;
  std::string epubPath;
  ButtonNavigator buttonNavigator;
  int currentSpineIndex = 0;
  int currentTocIndex = 0;
  int currentSectionPageCount = 0;  // Exact page count of the chapter currently being read (0 if unknown)
  int selectorIndex = 0;
  int resolvedCurrentTocIndex = 0;  // Resolved reading position in TOC, preserved during navigation

  // Pages-per-byte ratio derived from the current chapter's layout, used to estimate the
  // page length of every chapter at the active font/settings. <= 0 means "cannot estimate".
  float pagesPerByte = 0.0f;

  // Double-tap Up/Down detection (jump to current chapter)
  unsigned long lastNavReleaseMs = 0;
  int lastNavDirection = 0;  // +1 for next, -1 for previous

  const std::function<void()> onGoBack;
  const std::function<void(int tocIndex)> onSelectTocIndex;
  const std::function<void(int newSpineIndex, int newPage)> onSyncPosition;

  static constexpr int kLineHeight = 30;
  static constexpr unsigned long kDoubleTapMs = 350;
  // Width reserved on the right of each row for the chapter page-count badge.
  static constexpr int kPageCountReserve = 52;
  // How many chapters AFTER the current one show a page-count badge. The badge is
  // only drawn for the current chapter (exact count) plus this many following
  // chapters (estimated), because the byte-size extrapolation is a weak proxy
  // that grows unreliable the further it reaches from the measured chapter.
  static constexpr int kPageCountLookahead = 3;
  // Sanity ceiling on an estimated chapter length. The estimate scales the
  // current chapter's pages-per-byte onto another chapter's byte size; an
  // asset-heavy chapter (large bytes, few visible pages) can blow that up into
  // an absurd value. Anything above this is treated as untrustworthy and the
  // badge is hidden rather than rendering a nonsense number.
  static constexpr int kMaxPlausibleChapterPages = 5000;

  // Whether the row at `tocIndex` should display a page-count badge: only the
  // current chapter and the next kPageCountLookahead chapters, and only when a
  // ratio is available.
  bool rowShowsPageCount(int tocIndex) const;

  // Estimate the page length of the chapter at `tocIndex` for the active font/settings.
  // Returns the exact count for the chapter currently being read, an extrapolated estimate
  // for the others (clamped to a plausible range), or 0 when no trustworthy estimate exists.
  int estimateChapterPages(int tocIndex) const;

  // Compute wrapped line count for a single TOC item given available width.
  int getItemLineCount(int itemIndex, int maxTextWidth) const;

  // Find the page start index and compute layout for the page containing selectorIndex.
  // Returns the index of the first item on the page.
  int computePageStart(int availableHeight, int maxTextWidth) const;

  // Total TOC items count
  int getTotalItems() const;

 public:
  explicit EpubReaderChapterSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                              const std::shared_ptr<Epub>& epub, const std::string& epubPath,
                                              const int currentSpineIndex, const int currentTocIndex,
                                              const int currentSectionPageCount, const std::function<void()>& onGoBack,
                                              const std::function<void(int tocIndex)>& onSelectTocIndex,
                                              const std::function<void(int newSpineIndex, int newPage)>& onSyncPosition)
      : ActivityWithSubactivity("EpubReaderChapterSelection", renderer, mappedInput),
        epub(epub),
        epubPath(epubPath),
        currentSpineIndex(currentSpineIndex),
        currentTocIndex(currentTocIndex),
        currentSectionPageCount(currentSectionPageCount),
        onGoBack(onGoBack),
        onSelectTocIndex(onSelectTocIndex),
        onSyncPosition(onSyncPosition) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;
};
