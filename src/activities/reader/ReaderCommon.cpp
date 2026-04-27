#include "ReaderCommon.h"

#include <GfxRenderer.h>

#include <algorithm>

#include "CrossPointSettings.h"

namespace ReaderCommon {

namespace {
struct LastEnterSig {
  std::string bookPath;
  uint8_t fontFamily = 0xFF;
  uint8_t fontSize = 0xFF;
  std::string customFontName;
  uint8_t customFontSizePt = 0xFF;
  uint8_t orientation = 0xFF;
  uint8_t readerStyleMode = 0xFF;
  uint8_t imageDither = 0xFF;
  bool valid = false;
};
LastEnterSig g_lastEnter;
}  // namespace

bool shouldFullRefreshOnEnter(const std::string& bookPath) {
  const auto& s = SETTINGS;
  const bool needsFull = !g_lastEnter.valid || g_lastEnter.bookPath != bookPath ||
                         g_lastEnter.fontFamily != s.fontFamily || g_lastEnter.fontSize != s.fontSize ||
                         g_lastEnter.customFontName != s.customFontName ||
                         g_lastEnter.customFontSizePt != s.customFontSizePt ||
                         g_lastEnter.orientation != s.orientation ||
                         g_lastEnter.readerStyleMode != s.readerStyleMode ||
                         g_lastEnter.imageDither != s.imageDither;

  g_lastEnter.bookPath = bookPath;
  g_lastEnter.fontFamily = s.fontFamily;
  g_lastEnter.fontSize = s.fontSize;
  g_lastEnter.customFontName = s.customFontName;
  g_lastEnter.customFontSizePt = s.customFontSizePt;
  g_lastEnter.orientation = s.orientation;
  g_lastEnter.readerStyleMode = s.readerStyleMode;
  g_lastEnter.imageDither = s.imageDither;
  g_lastEnter.valid = true;

  return needsFull;
}

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
