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

  // Pass 2: delete.
  for (const auto& v : victims) {
    if (Storage.remove(v.path.c_str())) {
      result.removed++;
      result.bytesFreed += v.size;
      LOG_INF("CLEANUP", "Removed orphan: %s (%u bytes)", v.path.c_str(), (unsigned)v.size);
    } else {
      result.failed++;
      LOG_ERR("CLEANUP", "Failed to remove orphan: %s", v.path.c_str());
    }
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
    LOG_INF("CLEANUP", "Removed legacy %s (%u bytes)", kLegacy, (unsigned)sz);
  } else {
    result.failed++;
    LOG_ERR("CLEANUP", "Failed to remove legacy %s", kLegacy);
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

  LOG_INF("CLEANUP", "Done: %d removed, %d failed, %u bytes freed", removedCount, failedCount,
          (unsigned)bytesFreed);

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
    std::string resultText =
        std::to_string(removedCount) + " " + std::string(tr(STR_ITEMS_REMOVED));
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
