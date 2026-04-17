#include "BootActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>

#include <algorithm>
#include <string>

#include "Bitmap.h"
#include "fontIds.h"
#include "images/BootImage.h"

namespace {

constexpr int kBootImageVerticalOffset = -24;
constexpr int kProgressBarWidth = 300;
constexpr int kProgressBarHeight = 24;
constexpr int kProgressBarBorderWidth = 1;
constexpr int kProgressBarInnerPadding = 3;
constexpr int kProgressBarTopGap = 34;
constexpr int kVersionTextBottom = 30;
constexpr int kDynamicRegionPadding = 6;

int clampPercent(const int percent) { return std::max(0, std::min(100, percent)); }

std::string bootVersionText() {
  constexpr const char* kPrefix = "CrossPoint-Mod-DX34-";
  std::string version = CROSSPOINT_VERSION;
  if (version.rfind(kPrefix, 0) == 0) {
    version.erase(0, std::char_traits<char>::length(kPrefix));
  }
  return "[MOD] DX34 " + version;
}

}  // namespace

void BootActivity::onEnter() { renderEmbeddedBootScreen(HalDisplay::HALF_REFRESH, true); }

void BootActivity::setProgress(const int percent, const char* status) {
  progressPercent = clampPercent(percent);
  (void)status;
  renderEmbeddedBootScreen(HalDisplay::FAST_REFRESH, false);
}

bool BootActivity::tryDrawCustomBootImage() const {
  FsFile file;
  if (!Storage.openFileForRead("BOOT", "/boot.bmp", file)) {
    return false;
  }
  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) {
    file.close();
    return false;
  }
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Use 2x area for larger images, 1x for small ones.
  const bool isLarge = bitmap.getWidth() > kBootImageWidth || bitmap.getHeight() > kBootImageHeight;
  const int maxW = isLarge ? kBootImageWidth * 2 : kBootImageWidth;
  const int maxH = isLarge ? kBootImageHeight * 2 : kBootImageHeight;
  const int drawWidth = std::min(bitmap.getWidth(), maxW);
  const int drawHeight = std::min(bitmap.getHeight(), maxH);
  const int x = (pageWidth - drawWidth) / 2;
  const int centeredY = (pageHeight - drawHeight) / 2;
  const int y = centeredY + kBootImageVerticalOffset;

  renderer.clearScreen();
  renderer.drawBitmap(bitmap, x, y, maxW, maxH);
  file.close();
  customBootImageLoaded = true;
  return true;
}

void BootActivity::drawStaticBootScreen() const {
  if (tryDrawCustomBootImage()) {
    return;
  }
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const int x = (pageWidth - kBootImageWidth) / 2;
  const int centeredY = (pageHeight - kBootImageHeight) / 2;
  const int y = centeredY + kBootImageVerticalOffset;
  renderer.clearScreen();
  renderer.drawImage(BootImage, x, y, kBootImageWidth, kBootImageHeight);
  const std::string versionText = bootVersionText();
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - kVersionTextBottom, versionText.c_str());
}

void BootActivity::drawDynamicBootScreen() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const int centeredY = (pageHeight - kBootImageHeight) / 2;
  const int y = centeredY + kBootImageVerticalOffset;
  const int barX = (pageWidth - kProgressBarWidth) / 2;
  const int barY = y + kBootImageHeight + kProgressBarTopGap;
  const int innerX = barX + kProgressBarInnerPadding;
  const int innerY = barY + kProgressBarInnerPadding;
  const int innerWidth = kProgressBarWidth - kProgressBarInnerPadding * 2;
  const int innerHeight = kProgressBarHeight - kProgressBarInnerPadding * 2;
  const int fillWidth = innerWidth * progressPercent / 100;
  const int dynamicRegionTop = barY - kDynamicRegionPadding;
  const int dynamicRegionBottom = barY + kProgressBarHeight + kDynamicRegionPadding;

  renderer.fillRect(barX - kDynamicRegionPadding, dynamicRegionTop, kProgressBarWidth + kDynamicRegionPadding * 2,
                    dynamicRegionBottom - dynamicRegionTop, false);
  renderer.drawRect(barX, barY, kProgressBarWidth, kProgressBarHeight, true);
  renderer.fillRect(barX + kProgressBarBorderWidth, barY + kProgressBarBorderWidth,
                    kProgressBarWidth - kProgressBarBorderWidth * 2, kProgressBarHeight - kProgressBarBorderWidth * 2,
                    false);

  if (fillWidth > 0) {
    renderer.fillRect(innerX, innerY, fillWidth, innerHeight, true);
  }
}

void BootActivity::renderEmbeddedBootScreen(const HalDisplay::RefreshMode refreshMode, const bool fullRedraw) const {
  if (fullRedraw) {
    drawStaticBootScreen();
  }
  if (!customBootImageLoaded) {
    drawDynamicBootScreen();
  }
  renderer.displayBuffer(refreshMode);
}
