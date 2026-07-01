#include "StatusBarSettings.h"

#include "CrossPointSettings.h"

namespace crosspoint {
namespace reader {

StatusBarSettings snapshotStatusBarSettings() {
  const auto& s = SETTINGS;
  StatusBarSettings out;
  out.enabled = s.statusBarEnabled;
  out.fontId = s.getStatusBarFontId();
  out.progressBarHeight = s.getStatusBarProgressBarHeight();
  out.pageCounterMode = s.statusBarPageCounterMode;

  out.showBattery = s.statusBarShowBattery;
  out.batteryPosition = s.statusBarBatteryPosition;
  out.showPageCounter = s.statusBarShowPageCounter;
  out.pageCounterPosition = s.statusBarPageCounterPosition;
  out.showBookPercentage = s.statusBarShowBookPercentage;
  out.bookPercentagePosition = s.statusBarBookPercentagePosition;
  out.showChapterPercentage = s.statusBarShowChapterPercentage;
  out.chapterPercentagePosition = s.statusBarChapterPercentagePosition;
  out.showPagesLeft = s.statusBarShowPagesLeft;
  out.pagesLeftPosition = s.statusBarPagesLeftPosition;

  out.showChapterTitle = s.statusBarShowChapterTitle;
  out.titlePosition = s.statusBarTitlePosition;
  out.noTitleTruncation = s.statusBarNoTitleTruncation;
  out.showBookBar = s.statusBarShowBookBar;
  out.bookBarPosition = s.statusBarBookBarPosition;
  out.showChapterBar = s.statusBarShowChapterBar;
  out.chapterBarPosition = s.statusBarChapterBarPosition;

  out.showChapterNumber = s.statusBarShowChapterNumber;
  out.showQuoteCount = s.statusBarShowQuoteCount;
  out.showFreeHeap = s.statusBarShowFreeHeap;
  return out;
}

}  // namespace reader
}  // namespace crosspoint
