#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "IStatusMeasurePort.h"
#include "ReaderLayoutSafety.h"
#include "StatusBarLayout.h"
#include "StatusBarSettings.h"

/**
 * @file ReaderStatusComposer.h
 * @brief Deep, host-testable status-bar reserve+build module shared by the
 *        EPUB and TXT readers (RFC: status-bar deepening, design D + A-freeze).
 *
 * Replaces the ~95%-identical reserve blocks and buildStatusBarLayout bodies that
 * lived inline in EpubReaderActivity / TxtReaderActivity. The two jobs:
 *
 *   reserve() — from the settings snapshot + screen geometry, decide the top and
 *     bottom reserved pixel heights and the resolved title line count. Owns the
 *     two text-row OR-chains, the band-config construction, the title-line-count
 *     sampling (cached), and resolveStatusBarBudget orchestration.
 *
 *   build() — populate the StatusBarLayout the renderer consumes: format the
 *     shared items (book/chapter %, pages-left, free-heap), copy reader-resolved
 *     item strings (page counter, book-page counter, chapter number, quote count),
 *     and wrap the title (owned cache). Every string is pre-measured to a width.
 *
 * Dependencies cross three seams, all injected so the module never includes
 * GfxRenderer / SETTINGS / Epub / Txt:
 *   - measurement: IStatusMeasurePort (Local-substitutable; fake in tests).
 *   - settings:    StatusBarSettings snapshot (built from SETTINGS device-side).
 *   - reader data: cheap scalars + reader-resolved strings via StatusValues, and
 *     the lazy/cached title text via StatusTitleHooks (called only on cache miss).
 *
 * A-freeze: the reserve title wrap width is a caller input (EPUB screenWidth-8,
 * TXT usableWidth) so the shipped EPUB/TXT inconsistency is preserved byte-for-
 * byte rather than "fixed" here (that would move pixels).
 */
namespace crosspoint {
namespace reader {

// Per-frame reader-resolved values. The reader does the EPUB/TXT-specific work
// (progress math, book-page extrapolation, TOC/filename title resolution, i18n)
// and hands the module plain data. Reader-resolved item strings are "" when the
// item is not drawable; the module also gates each on the matching show flag.
struct StatusValues {
  float bookProgress = 0.0f;     // 0..100
  float chapterProgress = 0.0f;  // 0..100 (TXT: == bookProgress)
  int currentPage0 = 0;          // 0-based current page in chapter/file
  int pageCount = 0;             // pages in chapter/file
  uint32_t freeHeapBytes = 0;
  const char* pagesLeftLabel = "";  // i18n label, resolved reader-side
  std::string pageCounterText;      // ReaderCommon::formatPageCounterText (shared helper)
  std::string bookPageCounterText;  // EPUB extrapolation / TXT "cur/total"
  std::string chapterNumberText;    // EPUB only
  std::string quoteCountText;       // EPUB only
};

// Title text feeds, called only on a title-cache miss so the expensive TOC title
// resolution (~140 ms/entry cold) and per-entry sampling stay cached in-module.
struct StatusTitleHooks {
  std::function<int()> reserveTitleKey;                                         // cache key for the reserve sample set
  std::function<std::vector<std::string>(int maxSamples)> reserveTitleSamples;  // raw candidates (capped)
  std::function<int()> displayTitleKey;                                         // cache key for the shown title
  std::function<std::string()> displayTitleText;                                // raw shown title text
};

struct ReserveInput {
  int screenHeight = 0;
  int statusTopInset = 0;
  int statusBottomInset = 0;
  int marginTop = 0;
  int marginBottom = 0;
  int minContentHeight = 0;
  int titleReserveWrapWidth = 0;  // EPUB screenWidth-8, TXT usableWidth (frozen quirk)
};

struct ReserveResult {
  int topReservedHeight = 0;
  int bottomReservedHeight = 0;
  int resolvedTitleLineCount = 0;
};

class ReaderStatusComposer {
 public:
  // measure + logTag live for the activity's lifetime; hooks are moved in once.
  ReaderStatusComposer(const IStatusMeasurePort& measure, const char* logTag, StatusTitleHooks hooks);

  // Phase 1: reserved band heights + resolved title line count.
  ReserveResult reserve(const StatusBarSettings& s, const ReserveInput& in);

  // Phase 2: the layout POD the renderer consumes.
  ReaderStatusBar::StatusBarLayout build(const StatusBarSettings& s, int usableWidth, const ReserveResult& reserved,
                                         const StatusValues& values);

  // Drop the title sub-caches on font / orientation / book / setting change.
  void invalidateTitleCaches();

 private:
  int reserveTitleLineCountCached(int fontId, int wrapWidth, bool noTitleTruncation);
  const std::vector<std::string>& displayTitleLinesCached(int fontId, int wrapWidth, bool noTitleTruncation,
                                                          int maxLineCount);

  const IStatusMeasurePort& measure_;
  const char* logTag_;
  StatusTitleHooks hooks_;

  // Reserve title-line-count memo (was EpubReaderActivity's cachedReserve* fields).
  struct ReserveCache {
    bool valid = false;
    int key = 0;
    int width = -1;
    bool noTrunc = false;
    int lineCount = 1;
  } reserveCache_;

  // Display title-lines memo (was StatusBarTitleCache / TXT's loose cachedTitle* fields).
  struct TitleCache {
    bool valid = false;
    int key = 0;
    int width = -1;
    int maxLines = -1;
    bool noTrunc = false;
    std::vector<std::string> lines;
  } titleCache_;
};

}  // namespace reader
}  // namespace crosspoint
