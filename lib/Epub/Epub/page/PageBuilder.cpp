#include "PageBuilder.h"

#include <HeapGuard.h>
#include <Logging.h>

#include <algorithm>
#include <new>

namespace crosspoint {
namespace page {

PageStatus PageBuilder::ensurePage() {
  if (currentPage_) return PageStatus::Ok;
  currentPage_.reset(new (std::nothrow) Page());
  if (!currentPage_) {
    LOG_DIAG("PB", "OOM new Page (ensurePage) largest=%u", (unsigned)crosspoint::heap::largestFreeBlockBytes());
    return PageStatus::Oom;
  }
  currentPageNextY_ = 0;
  return PageStatus::Ok;
}

void PageBuilder::completePage() {
  if (!currentPage_) return;

  // Dense-fit uses the PAGE-level line height (baseLineHeight), not the
  // per-line resolved height — identical to the old completeCurrentPage().
  const int lineHeight = cfg_.baseLineHeight;
  if (lineHeight > 0 && cfg_.viewportHeight > 0) {
    const int maxPossibleLines = cfg_.viewportHeight / lineHeight;
    const int minDenseLines =
        std::max(cfg_.minDensePageLines, (maxPossibleLines * cfg_.densePageThresholdPercent) / 100);
    currentPage_->applyDensePageVerticalFit(lineHeight, cfg_.viewportHeight, minDenseLines, lineHeight / 2);
  }

  pageSink_(std::move(currentPage_));  // currentPage_ becomes null
  completedPageCount_++;
}

PageStatus PageBuilder::bindPendingAnchors() {
  if (pendingAnchors_.empty()) return PageStatus::Ok;

  if (const PageStatus s = ensurePage(); s != PageStatus::Ok) return s;

  for (const auto& anchor : pendingAnchors_) {
    if (!anchor.empty()) anchorSink_(anchor, completedPageCount_);
  }
  pendingAnchors_.clear();
  return PageStatus::Ok;
}

PageStatus PageBuilder::addLine(const std::shared_ptr<TextBlock>& line) {
  // Upstream layout OOM is signalled by a null line (LayoutEngine emits null
  // through the processLine callback) — treat it as our own OOM.
  if (!line) {
    LOG_DIAG("PB", "OOM upstream null TextBlock largest=%u", (unsigned)crosspoint::heap::largestFreeBlockBytes());
    return PageStatus::Oom;
  }

  const int lineHeight = line->getBlockStyle().resolveLineHeight(cfg_.baseLineHeight);

  if (const PageStatus s = ensurePage(); s != PageStatus::Ok) return s;

  if (currentPageNextY_ + lineHeight > cfg_.viewportHeight) {
    completePage();
    if (const PageStatus s = ensurePage(); s != PageStatus::Ok) return s;
  }

  if (const PageStatus s = bindPendingAnchors(); s != PageStatus::Ok) return s;

  // Assign footnotes whose anchor word falls on this line to the current page.
  footnotes_.placeForLine(line->wordCount(),
                          [this](const char* number, const char* href) { currentPage_->addFootnote(number, href); });

  const int16_t xOffset = line->getBlockStyle().leftInset();

  // Heap-probe before the throwing make_shared (fuses the PageLine + control
  // block into one allocation) — same contract as the old addLineToPage.
  if (!crosspoint::heap::canAllocateContiguous(sizeof(PageLine))) {
    LOG_DIAG("PB", "OOM PageLine probe largest=%u", (unsigned)crosspoint::heap::largestFreeBlockBytes());
    return PageStatus::Oom;
  }
  currentPage_->elements.push_back(std::make_shared<PageLine>(line, xOffset, currentPageNextY_));
  currentPageNextY_ = static_cast<int16_t>(currentPageNextY_ + lineHeight);
  return PageStatus::Ok;
}

PageStatus PageBuilder::addImage(const std::shared_ptr<ImageBlock>& image, const int displayWidth,
                                 const int displayHeight) {
  // Only break to a new page if the current one already has content and the
  // image won't fit — a too-tall image on a fresh page is kept, not re-broken
  // forever. Identical to the old inline <img> placement.
  if (currentPage_ && !currentPage_->elements.empty() && (currentPageNextY_ + displayHeight > cfg_.viewportHeight)) {
    completePage();
    if (const PageStatus s = ensurePage(); s != PageStatus::Ok) return s;
  } else if (!currentPage_) {
    if (const PageStatus s = ensurePage(); s != PageStatus::Ok) return s;
  }

  if (const PageStatus s = bindPendingAnchors(); s != PageStatus::Ok) return s;

  const int16_t xPos = static_cast<int16_t>((cfg_.viewportWidth - displayWidth) / 2);
  auto* rawPageImage = new (std::nothrow) PageImage(image, xPos, currentPageNextY_);
  if (!rawPageImage) {
    LOG_DIAG("PB", "OOM new PageImage largest=%u", (unsigned)crosspoint::heap::largestFreeBlockBytes());
    return PageStatus::Oom;
  }
  currentPage_->elements.push_back(std::shared_ptr<PageImage>(rawPageImage));
  currentPageNextY_ = static_cast<int16_t>(currentPageNextY_ + displayHeight);
  return PageStatus::Ok;
}

PageStatus PageBuilder::addHorizontalRule(const int16_t width, const uint8_t thickness, const int16_t xPos,
                                          const int topSpacing, const int bottomSpacing) {
  const int totalHeight = topSpacing + thickness + bottomSpacing;
  // Same placement rule as addImage: only break to a new page when the current
  // one already has content and the rule + its spacing won't fit.
  if (currentPage_ && !currentPage_->elements.empty() && (currentPageNextY_ + totalHeight > cfg_.viewportHeight)) {
    completePage();
    if (const PageStatus s = ensurePage(); s != PageStatus::Ok) return s;
  } else if (!currentPage_) {
    if (const PageStatus s = ensurePage(); s != PageStatus::Ok) return s;
  }

  if (const PageStatus s = bindPendingAnchors(); s != PageStatus::Ok) return s;

  currentPageNextY_ = static_cast<int16_t>(currentPageNextY_ + topSpacing);
  auto* rawRule = new (std::nothrow) PageHorizontalRule(width, thickness, xPos, currentPageNextY_);
  if (!rawRule) {
    LOG_DIAG("PB", "OOM new PageHorizontalRule largest=%u", (unsigned)crosspoint::heap::largestFreeBlockBytes());
    return PageStatus::Oom;
  }
  currentPage_->elements.push_back(std::shared_ptr<PageHorizontalRule>(rawRule));
  currentPageNextY_ = static_cast<int16_t>(currentPageNextY_ + thickness + bottomSpacing);
  return PageStatus::Ok;
}

}  // namespace page
}  // namespace crosspoint
