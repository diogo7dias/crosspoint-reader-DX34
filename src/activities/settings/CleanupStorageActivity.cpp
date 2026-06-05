#include "CleanupStorageActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <cstring>
#include <string>
#include <vector>

#include "MappedInputManager.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"
#include "util/StatusPopup.h"

namespace {

bool endsWith(const std::string& s, const char* suffix) {
  const size_t n = std::strlen(suffix);
  return s.size() >= n && std::memcmp(s.data() + s.size() - n, suffix, n) == 0;
}

bool startsWithJunk(const std::string& s) {
  // Names contain ".junk-<millis>" — match the substring anywhere because
  // the prefix is the primary file name (e.g. "settings.json.junk-12345").
  return s.find(".junk-") != std::string::npos;
}

// True for files that are atomic-write debris and safe to drop. We do NOT
// touch .bak files — those are the rollback half of the 2-layer atomic
// write and are part of the durable-state contract.
bool isOrphanTmpName(const std::string& name) {
  return endsWith(name, ".tmp") || endsWith(name, ".tmp2") || startsWithJunk(name);
}

struct CleanupResult {
  int removed = 0;
  int failed = 0;
  uint32_t bytesFreed = 0;
};

void cleanupOrphansInDir(const char* dirPath, CleanupResult& result) {
  FsFile dir = Storage.open(dirPath, O_RDONLY);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }

  // Pass 1: collect orphan paths + sizes before any deletion so we don't
  // invalidate the directory iterator.
  struct Victim {
    std::string path;
    uint32_t size;
  };
  std::vector<Victim> victims;
  char nameBuf[128];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    file.getName(nameBuf, sizeof(nameBuf));
    const std::string name(nameBuf);
    const bool isDir = file.isDirectory();
    const uint32_t sz = isDir ? 0u : static_cast<uint32_t>(file.size());
    file.close();
    if (isDir) continue;
    if (!isOrphanTmpName(name)) continue;

    std::string path = dirPath;
    if (path.empty() || path.back() != '/') path += "/";
    path += name;
    victims.push_back({std::move(path), sz});
  }
  dir.close();

  // Pass 2: delete. SdFat sometimes refuses Storage.remove() on a file whose
  // directory entry is in an odd state (interrupted prior write, FAT
  // inconsistency). Mirror the truncate-then-remove fallback used by the
  // boot sweepOrphanTmp() in main.cpp before declaring failure.
  for (const auto& v : victims) {
    // Skip tombstoned files: when SdFat can't read a file's FAT chain it
    // returns UINT32_MAX from size(), and remove() / openFileForWrite()
    // both fail downstream. Boot sweepOrphanTmp would already have renamed
    // any such .tmp to .junk-<millis>, making the file inert (no longer
    // confused with a live atomic-write tmp). Treating these as failures
    // would make every cleanup invocation report a non-removable error
    // for a file that no SdFat path can ever delete — better to skip
    // quietly. Real cleanup is to pull the SD into a host OS and delete
    // the file there, or full-format the card.
    if (v.size == UINT32_MAX) {
      LOG_DIAG("CLEANUP", "skip tombstoned (corrupt FAT entry): %s", v.path.c_str());
      continue;
    }
    if (Storage.remove(v.path.c_str())) {
      result.removed++;
      result.bytesFreed += v.size;
      LOG_DIAG("CLEANUP", "removed orphan: %s (%u B)", v.path.c_str(), (unsigned)v.size);
      continue;
    }
    // Reopen for write to truncate, close, then retry remove. On SdFat this
    // often unsticks a directory entry that plain remove() rejects.
    FsFile f;
    if (Storage.openFileForWrite("CLEANUP", v.path.c_str(), f)) {
      f.close();
      if (Storage.remove(v.path.c_str())) {
        result.removed++;
        result.bytesFreed += v.size;
        LOG_DIAG("CLEANUP", "removed orphan after truncate: %s (%u B)", v.path.c_str(), (unsigned)v.size);
        continue;
      }
    }
    result.failed++;
    LOG_DIAG("CLEANUP", "FAILED to remove orphan: %s (size=%u)", v.path.c_str(), (unsigned)v.size);
  }
}

// Pre-RFC-#146 firmware wrote to /wifi_report.txt; the current path is
// /diag_report.txt. Old file is dead weight on upgraded SD cards.
void cleanupLegacyWifiReport(CleanupResult& result) {
  constexpr const char* kLegacy = "/wifi_report.txt";
  if (!Storage.exists(kLegacy)) return;

  FsFile f;
  uint32_t sz = 0;
  if (Storage.openFileForRead("CLEANUP", kLegacy, f)) {
    sz = static_cast<uint32_t>(f.size());
    f.close();
  }
  if (Storage.remove(kLegacy)) {
    result.removed++;
    result.bytesFreed += sz;
    LOG_DIAG("CLEANUP", "removed legacy %s (%u B)", kLegacy, (unsigned)sz);
  } else {
    result.failed++;
    LOG_DIAG("CLEANUP", "FAILED to remove legacy %s", kLegacy);
  }
}

}  // namespace

void CleanupStorageActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  state = WARNING;
  requestUpdate();
}

void CleanupStorageActivity::onExit() { ActivityWithSubactivity::onExit(); }

void CleanupStorageActivity::runCleanup() {
  LOG_DBG("CLEANUP", "Scanning SD for orphan tmp/junk files...");
  CleanupResult result;

  cleanupOrphansInDir("/", result);
  cleanupOrphansInDir("/.crosspoint", result);
  cleanupLegacyWifiReport(result);

  removedCount = result.removed;
  failedCount = result.failed;
  bytesFreed = result.bytesFreed;

  LOG_DIAG("CLEANUP", "done: %d removed, %d failed, %u B freed", removedCount, failedCount, (unsigned)bytesFreed);

  if (failedCount > 0 && removedCount == 0) {
    state = FAILED;
  } else if (removedCount == 0) {
    state = NOTHING_TO_DO;
  } else {
    state = SUCCESS;
  }
  requestUpdate();
}

void CleanupStorageActivity::render(Activity::RenderLock&&) {
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_CLEANUP_STORAGE), true, EpdFontFamily::REGULAR);

  if (state == WARNING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 60, tr(STR_CLEANUP_STORAGE_DESC1), true);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 30, tr(STR_CLEANUP_STORAGE_DESC2), true);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, tr(STR_CLEANUP_STORAGE_DESC3), true,
                              EpdFontFamily::REGULAR);

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_CLEAR_BUTTON), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == CLEANING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_CLEANUP_STORAGE_SCANNING), true,
                              EpdFontFamily::REGULAR);
    renderer.displayBuffer();
    return;
  }

  if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_CLEANUP_STORAGE_DONE), true,
                              EpdFontFamily::REGULAR);
    std::string resultText = std::to_string(removedCount) + " " + std::string(tr(STR_ITEMS_REMOVED));
    if (bytesFreed > 0) {
      resultText += ", " + std::to_string(bytesFreed) + " " + std::string(tr(STR_BYTES_FREED));
    }
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, resultText.c_str());

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == NOTHING_TO_DO) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_CLEANUP_STORAGE_NOTHING), true,
                              EpdFontFamily::REGULAR);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_CLEANUP_STORAGE_FAILED), true,
                              EpdFontFamily::REGULAR);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, tr(STR_CHECK_SERIAL_OUTPUT));

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}

void CleanupStorageActivity::loop() {
  if (state == WARNING) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      LOG_DBG("CLEANUP", "User confirmed, starting SD cleanup");
      {
        RenderLock lock(*this);
        state = CLEANING;
      }
      StatusPopup::showBlocking(renderer, tr(STR_CLEANUP_STORAGE_SCANNING));
      runCleanup();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }

  if (state == SUCCESS || state == NOTHING_TO_DO || state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }
}
