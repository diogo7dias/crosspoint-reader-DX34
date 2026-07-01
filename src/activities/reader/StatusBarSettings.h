#pragma once

#include <cstdint>

/**
 * @file StatusBarSettings.h
 * @brief Flat, host-buildable snapshot of the ~25 statusBar* settings the
 *        status-bar reserve/build logic reads.
 *
 * snapshotStatusBarSettings() reads them once per frame from the SETTINGS global
 * (device-only, defined in StatusBarSettings.cpp); host tests build the struct
 * literally. This is the single SETTINGS touch point for ReaderStatusComposer,
 * so the composer's reserve()/build() are pure given (snapshot + measurer +
 * values). Positions hold the raw CrossPointSettings::STATUS_* enum values.
 */
namespace crosspoint {
namespace reader {

struct StatusBarSettings {
  bool enabled = false;
  int fontId = 0;
  int progressBarHeight = 0;
  uint8_t pageCounterMode = 0;

  // Items that participate in the top/bottom text-row reserve decision.
  bool showBattery = false;
  uint8_t batteryPosition = 0;
  bool showPageCounter = false;
  uint8_t pageCounterPosition = 0;
  bool showBookPercentage = false;
  uint8_t bookPercentagePosition = 0;
  bool showChapterPercentage = false;
  uint8_t chapterPercentagePosition = 0;
  bool showPagesLeft = false;
  uint8_t pagesLeftPosition = 0;

  // Title + progress bars (own reserve geometry).
  bool showChapterTitle = false;
  uint8_t titlePosition = 0;
  bool noTitleTruncation = false;
  bool showBookBar = false;
  uint8_t bookBarPosition = 0;
  bool showChapterBar = false;
  uint8_t chapterBarPosition = 0;

  // Build-only items: they ride an existing text row (no separate reserve slot),
  // so the EPUB-only reserve OR-chains deliberately exclude them.
  bool showChapterNumber = false;
  bool showQuoteCount = false;
  bool showFreeHeap = false;
};

// Reads the live SETTINGS global into a snapshot. Device-only.
StatusBarSettings snapshotStatusBarSettings();

}  // namespace reader
}  // namespace crosspoint
