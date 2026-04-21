#include "ReaderCommon.h"

#include <GfxRenderer.h>

#include <algorithm>

#include "CrossPointSettings.h"

namespace ReaderCommon {


std::string formatPageCounterText(const uint8_t mode, const int currentPage, const int totalPages) {
  const int safeTotalPages = std::max(totalPages, 0);
  const int safeCurrentPage = std::max(currentPage, 0);
  int pagesLeft = safeTotalPages - (currentPage + 1);
  if (pagesLeft < 0) {
    pagesLeft = 0;
  }

  switch (mode) {
    case CrossPointSettings::STATUS_PAGE_LEFT_TEXT:
      return std::to_string(pagesLeft) + " left";
    default:
      return std::to_string(safeCurrentPage + 1) + "/" + std::to_string(safeTotalPages);
  }
}

void applyReaderOrientation(GfxRenderer& renderer, const uint8_t orientation) {
  switch (orientation) {
    case CrossPointSettings::ORIENTATION::PORTRAIT:
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
      break;
    case CrossPointSettings::ORIENTATION::INVERTED:
      renderer.setOrientation(GfxRenderer::Orientation::PortraitInverted);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
      break;
    default:
      break;
  }
}

}  // namespace ReaderCommon
