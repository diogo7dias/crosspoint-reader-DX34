// PageBuilder — the EPUB page-assembly deep module (architecture RFC follow-up).
//
// Carves the page accumulator out of ChapterHtmlSlimParser: it owns the
// in-flight Page, the running vertical cursor (currentPageNextY), the
// completed-page count, pending anchors, and the complete-when-full + dense-fit
// logic. The parser keeps expat parsing, style resolution (StyleResolver), the
// LayoutEngine that lays out lines, footnote-index tracking, and — crucially —
// image *extraction* (Storage/Epub/decoder are expat-context work). The parser
// feeds this module already-laid-out TextBlock lines and already-extracted
// ImageBlocks; PageBuilder decides where they land and when a page is full.
//
// THE CRASH-RELEVANT POINT: the parser used to signal every allocation failure
// by setting a sticky `parseFailed` bool that ~6 call sites had to remember to
// poll — a forgotten poll meant layout kept running on half-allocated state,
// i.e. a panic waiting to happen. Here every fallible call returns an explicit,
// [[nodiscard]] PageStatus; a missed check is a compile diagnostic, not a silent
// corrupt-state crash. OOM is host-injectable (via the HeapGuard probe override)
// so the OOM/recovery paths are unit-testable off-device.
//
// Pure enough to host-compile: depends on Page/TextBlock/ImageBlock/BlockStyle/
// FootnotePlacer + the HeapGuard probe + Logging. No GfxRenderer (the caller
// pre-computes baseLineHeight), no Storage, no Epub, no expat.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../Page.h"
#include "../blocks/ImageBlock.h"
#include "../blocks/TextBlock.h"
#include "../parsers/FootnotePlacer.h"

namespace crosspoint {
namespace page {

// Explicit replacement for the sticky `parseFailed` bool. Returned by every
// mutating call that can allocate; the caller stops feeding the instant it is
// not Ok. (Image *extraction* failure is NOT represented here — that is handled
// in the parser's <img> branch, which falls back to alt text before ever
// calling addImage; addImage only places an already-built ImageBlock.)
enum class PageStatus : uint8_t {
  Ok,   // element placed / spacing applied; keep feeding
  Oom,  // a Page / PageLine / PageImage allocation (or its heap probe) failed
};
inline bool ok(PageStatus s) { return s == PageStatus::Ok; }

// Per-section geometry, captured once. baseLineHeight is the page-level line
// height = renderer.getLineHeight(fontId) * lineCompression, precomputed by the
// caller so this module never touches GfxRenderer. The dense-fit knobs mirror
// the parser's MIN_DENSE_PAGE_LINES / DENSE_PAGE_THRESHOLD_PERCENT constants.
struct PageConfig {
  uint16_t viewportWidth = 0;
  uint16_t viewportHeight = 0;
  int baseLineHeight = 0;
  int minDensePageLines = 6;
  int densePageThresholdPercent = 80;
};

class PageBuilder {
 public:
  using PageSink = std::function<void(std::unique_ptr<Page>)>;
  using AnchorSink = std::function<void(const std::string&, uint16_t)>;

  // `footnotes` is owned by the caller (the parser registers footnotes during
  // XML traversal); PageBuilder only places them onto the page their anchor
  // line lands on. Sinks mirror the parser's completePageFn / anchorPageFn.
  PageBuilder(const PageConfig& cfg, FootnotePlacer& footnotes, PageSink pageSink, AnchorSink anchorSink)
      : cfg_(cfg), footnotes_(footnotes), pageSink_(std::move(pageSink)), anchorSink_(std::move(anchorSink)) {}

  // Place one laid-out line (the LayoutEngine::flush processLine sink). A null
  // line is the upstream layout-OOM signal and returns Oom, exactly as the old
  // addLineToPage treated it.
  [[nodiscard]] PageStatus addLine(const std::shared_ptr<TextBlock>& line);

  // Place an already-extracted, already-scaled image. Page-breaks if it won't
  // fit the remaining space (same rule as before: only break when the current
  // page is non-empty). The caller owns extraction + the ImageBlock alloc.
  [[nodiscard]] PageStatus addImage(const std::shared_ptr<ImageBlock>& image, int displayWidth, int displayHeight);

  // Place a horizontal rule (<hr>). topSpacing/bottomSpacing are the vertical
  // margins around the rule; page-breaks if the rule + spacing won't fit the
  // remaining space (same non-empty-page rule as addImage). (#7accc607)
  [[nodiscard]] PageStatus addHorizontalRule(int16_t width, uint8_t thickness, int16_t xPos, int topSpacing,
                                             int bottomSpacing);

  // Ensure an in-flight page exists (lazily allocating one, cursor reset to 0)
  // WITHOUT placing content. The parser calls this at paragraph start so a
  // fresh page's cursor is 0 BEFORE top margin/padding is applied via advanceY
  // — matching the old makePages(), which created the page (cursor=0) and only
  // then added top spacing. A no-op (cursor unchanged) when a page is already
  // open. Without this seam, advanceY-before-the-first-addLine would be lost to
  // addLine's own ensurePage cursor reset.
  [[nodiscard]] PageStatus ensureOpenPage() { return ensurePage(); }

  // Advance the vertical cursor (paragraph margins / padding / inter-paragraph
  // gap). Pure bookkeeping, cannot allocate. The caller passes the same
  // positive deltas the parser added inline.
  void advanceY(int pixels) {
    if (pixels > 0) currentPageNextY_ = static_cast<int16_t>(currentPageNextY_ + pixels);
  }

  // Queue anchors (id=/name=) seen on an element; they bind to whatever page
  // the next line/image lands on.
  void queueAnchors(const std::vector<std::string>& anchors) {
    if (!anchors.empty()) pendingAnchors_.insert(pendingAnchors_.end(), anchors.begin(), anchors.end());
  }

  // Queue a single anchor, appending directly to the pending list. Lets the
  // parser hot path push anchors straight from the expat attribute buffer
  // without first materializing a per-element std::vector<std::string> (and the
  // subsequent copy that queueAnchors does). No-op on null/empty.
  void queueAnchor(const char* anchor) {
    if (anchor && anchor[0]) pendingAnchors_.emplace_back(anchor);
  }

  // End-of-block fallback: drain any footnotes whose word index fell exactly on
  // the block boundary onto the current page (mirrors makePages' tail).
  void drainFootnotes() {
    if (!footnotes_.empty() && currentPage_) {
      footnotes_.drainRemaining(
          [this](const char* number, const char* href) { currentPage_->addFootnote(number, href); });
    }
  }

  // Bind any still-pending anchors to the in-flight page at end of section —
  // but only when that page actually has content, mirroring the old parse-end
  // guard `pendingAnchors non-empty && currentPage && !elements.empty()`. A
  // trailing anchor on an empty/absent page stays unbound (book ends), exactly
  // as before.
  [[nodiscard]] PageStatus bindTrailingAnchors() {
    if (pendingAnchors_.empty()) return PageStatus::Ok;
    if (!currentPage_ || currentPage_->elements.empty()) return PageStatus::Ok;
    return bindPendingAnchors();
  }

  // Emit the trailing in-flight page (dense-fit applied) at end of section —
  // ONLY when it has content. The old parse-end completed the last page solely
  // under `currentPage && !elements.empty()`, so an empty page left open by a
  // trailing ensureOpenPage / empty final paragraph must NOT be emitted (it
  // would inflate the page count by one).
  void finish() {
    if (currentPage_ && !currentPage_->elements.empty()) {
      completePage();
    } else {
      currentPage_.reset();
    }
  }

  uint16_t completedPageCount() const { return completedPageCount_; }
  // Visible for tests / diagnostics.
  int16_t cursorY() const { return currentPageNextY_; }
  bool hasOpenPage() const { return static_cast<bool>(currentPage_); }

 private:
  [[nodiscard]] PageStatus ensurePage();  // lazy new(nothrow) Page, resets cursor
  void completePage();                    // dense-fit + emit + count++ (no alloc)
  [[nodiscard]] PageStatus bindPendingAnchors();

  PageConfig cfg_;
  FootnotePlacer& footnotes_;
  PageSink pageSink_;
  AnchorSink anchorSink_;
  std::unique_ptr<Page> currentPage_;
  int16_t currentPageNextY_ = 0;
  uint16_t completedPageCount_ = 0;
  std::vector<std::string> pendingAnchors_;
};

}  // namespace page
}  // namespace crosspoint
