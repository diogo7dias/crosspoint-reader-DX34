#include "ClearCacheActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <string>
#include <vector>

#include "MappedInputManager.h"
#include "Paths.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"
#include "util/StatusPopup.h"

namespace {
// Integer-only byte formatter (avoids pulling in float printf on the C3).
std::string formatBytes(uint64_t b) {
  if (b >= 1024ull * 1024) {
    const uint64_t tenths = (b * 10) / (1024ull * 1024);
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + " MB";
  }
  return std::to_string(b / 1024) + " KB";
}
}  // namespace

void ClearCacheActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  scanCacheSize();
  state = WARNING;
  requestUpdate();
}

void ClearCacheActivity::onExit() { ActivityWithSubactivity::onExit(); }

// Stream-scan the reading-cache dirs (/.crosspoint/{epub_,xtc_,txt_}*) and sum
// their file sizes for the "how much will this free" display. Two-pass like
// clearCache(): collect dir names, close root, then sum each dir's files. Bounded
// RAM (just the dir-name list + a running total).
void ClearCacheActivity::scanCacheSize() {
  cacheBytes = 0;
  cacheBooks = 0;

  auto root = Storage.open(Paths::kDataDir);
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return;
  }
  char name[128];
  std::vector<std::string> cacheDirs;
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    const std::string itemName(name);
    const bool isCacheDir = file.isDirectory() && (itemName.rfind("epub_", 0) == 0 || itemName.rfind("xtc_", 0) == 0 ||
                                                   itemName.rfind("txt_", 0) == 0);
    file.close();
    if (isCacheDir) cacheDirs.push_back(std::string(Paths::kDataDir) + "/" + itemName);
  }
  root.close();

  cacheBooks = static_cast<int>(cacheDirs.size());
  for (const auto& dirPath : cacheDirs) {
    auto dir = Storage.open(dirPath.c_str());
    if (!dir || !dir.isDirectory()) {
      if (dir) dir.close();
      continue;
    }
    for (auto f = dir.openNextFile(); f; f = dir.openNextFile()) {
      if (!f.isDirectory()) cacheBytes += static_cast<uint64_t>(f.size());
      f.close();
    }
    dir.close();
  }
}

void ClearCacheActivity::render(Activity::RenderLock&&) {
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_CLEAR_READING_CACHE), true, EpdFontFamily::REGULAR);

  if (state == WARNING) {
    const std::string sizeLine = "Reading cache: " + formatBytes(cacheBytes) + ", " + std::to_string(cacheBooks) +
                                 (cacheBooks == 1 ? " book" : " books");
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 90, sizeLine.c_str(), true, EpdFontFamily::REGULAR);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 60, "Clears generated cache files", true);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 30, "for EPUB/XTC/TXT books.", true);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, "Reading progress is preserved.", true,
                              EpdFontFamily::REGULAR);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 30, "Books may re-index when opened.", true);

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_CLEAR_BUTTON), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == CLEARING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_CLEARING_CACHE), true, EpdFontFamily::REGULAR);
    renderer.displayBuffer();
    return;
  }

  if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_CACHE_CLEARED), true, EpdFontFamily::REGULAR);
    std::string resultText = std::to_string(clearedCount) + " " + std::string(tr(STR_ITEMS_REMOVED));
    if (failedCount > 0) {
      resultText += ", " + std::to_string(failedCount) + " " + std::string(tr(STR_FAILED_LOWER));
    }
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, resultText.c_str());
    const std::string freedLine = "Freed " + formatBytes(cacheBytes);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 40, freedLine.c_str(), true, EpdFontFamily::REGULAR);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_CLEAR_CACHE_FAILED), true,
                              EpdFontFamily::REGULAR);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, tr(STR_CHECK_SERIAL_OUTPUT));

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}

void ClearCacheActivity::clearCache() {
  LOG_DBG("CLEAR_CACHE", "Clearing cache...");

  // Open .crosspoint directory
  auto root = Storage.open(Paths::kDataDir);
  if (!root || !root.isDirectory()) {
    LOG_DBG("CLEAR_CACHE", "Failed to open cache directory");
    if (root) root.close();
    state = FAILED;
    requestUpdate();
    return;
  }

  clearedCount = 0;
  failedCount = 0;
  char name[128];
  std::vector<std::string> cacheDirs;

  // Pass 1: collect cache directories first, then close root before mutating FS.
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    const std::string itemName(name);
    const bool isCacheDir = file.isDirectory() && (itemName.rfind("epub_", 0) == 0 || itemName.rfind("xtc_", 0) == 0 ||
                                                   itemName.rfind("txt_", 0) == 0);
    file.close();
    if (isCacheDir) {
      cacheDirs.push_back(std::string(Paths::kDataDir) + "/" + itemName);
    }
  }
  root.close();

  // Files to preserve during cache clear (reading progress, per-book settings, bookmarks).
  static constexpr const char* kPreserveFiles[] = {
      "/progress.bin",
      "/progress.bin.bak",
      "/reader_settings.json",
      "/bookmarks.json",
  };
  constexpr int kPreserveCount = sizeof(kPreserveFiles) / sizeof(kPreserveFiles[0]);

  // Pass 2: process each cache directory independently.
  for (const auto& fullPath : cacheDirs) {
    LOG_DBG("CLEAR_CACHE", "Removing cache: %s", fullPath.c_str());

    // Back up all preserve-worthy files before nuking the directory.
    struct PreservedFile {
      const char* suffix;
      std::vector<uint8_t> data;
      bool valid = false;
    };
    PreservedFile preserved[kPreserveCount];

    for (int i = 0; i < kPreserveCount; i++) {
      preserved[i].suffix = kPreserveFiles[i];
      FsFile f;
      const std::string path = fullPath + kPreserveFiles[i];
      if (Storage.openFileForRead("CLEAR_CACHE", path, f)) {
        const auto sz = static_cast<size_t>(f.size());
        if (sz > 0 && sz <= 4096) {
          preserved[i].data.resize(sz);
          const int rd = f.read(preserved[i].data.data(), sz);
          preserved[i].valid = (rd == static_cast<int>(sz));
          if (!preserved[i].valid) preserved[i].data.clear();
        }
        f.close();
      }
    }

    if (!Storage.removeDir(fullPath.c_str())) {
      LOG_ERR("CLEAR_CACHE", "Failed to remove: %s", fullPath.c_str());
      failedCount++;
      continue;
    }

    if (!Storage.mkdir(fullPath.c_str())) {
      LOG_ERR("CLEAR_CACHE", "Failed to recreate cache dir: %s", fullPath.c_str());
      failedCount++;
      continue;
    }

    // Restore preserved files.
    bool restoreFailed = false;
    for (int i = 0; i < kPreserveCount; i++) {
      if (!preserved[i].valid) continue;
      FsFile out;
      const std::string path = fullPath + preserved[i].suffix;
      if (!Storage.openFileForWrite("CLEAR_CACHE", path, out) ||
          out.write(preserved[i].data.data(), preserved[i].data.size()) != preserved[i].data.size()) {
        LOG_ERR("CLEAR_CACHE", "Failed to restore: %s", path.c_str());
        restoreFailed = true;
        if (out) out.close();
      } else {
        out.close();
      }
    }

    if (restoreFailed) {
      failedCount++;
    } else {
      clearedCount++;
    }
  }

  LOG_DBG("CLEAR_CACHE", "Cache cleared: %d removed, %d failed", clearedCount, failedCount);

  state = SUCCESS;
  requestUpdate();
}

void ClearCacheActivity::loop() {
  if (state == WARNING) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      LOG_DBG("CLEAR_CACHE", "User confirmed, starting cache clear");
      {
        RenderLock lock(*this);
        state = CLEARING;
      }
      StatusPopup::showBlocking(renderer, tr(STR_CLEARING_CACHE));
      clearCache();
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      LOG_DBG("CLEAR_CACHE", "User cancelled");
      goBack();
    }
    return;
  }

  if (state == SUCCESS || state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }
}
