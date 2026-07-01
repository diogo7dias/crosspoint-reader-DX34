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

void cleanupOrphansInDir(const char* dirPath, CleanupResult& result, bool dryRun) {
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

  // Dry-run: just tally what WOULD be removed (skip tombstoned files no SdFat
  // path can delete), no mutation.
  if (dryRun) {
    for (const auto& v : victims) {
      if (v.size == UINT32_MAX) continue;
      result.removed++;
      result.bytesFreed += v.size;
    }
    return;
  }

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

// The three per-type report files (panic /crash_report.txt, WiFi/OTA
// /diag_report.txt, heap /heap_report.txt) plus the pre-RFC-#146
// /wifi_report.txt are all superseded by the single appended, self-capping
// /CRASH_INFO.TXT. On an upgraded SD card the old files are dead weight — remove
// them. /CRASH_INFO.TXT itself is the live diagnostics file (bounded by its own
// cap) and is intentionally NOT removed here.
void cleanupLegacyReports(CleanupResult& result, bool dryRun) {
  constexpr const char* kLegacyReports[] = {"/wifi_report.txt", "/crash_report.txt", "/diag_report.txt",
                                            "/heap_report.txt"};
  for (const char* path : kLegacyReports) {
    if (!Storage.exists(path)) continue;

    FsFile f;
    uint32_t sz = 0;
    if (Storage.openFileForRead("CLEANUP", path, f)) {
      sz = static_cast<uint32_t>(f.size());
      f.close();
    }
    if (dryRun) {
      result.removed++;
      result.bytesFreed += sz;
      continue;
    }
    if (Storage.remove(path)) {
      result.removed++;
      result.bytesFreed += sz;
      LOG_DIAG("CLEANUP", "removed legacy %s (%u B)", path, (unsigned)sz);
    } else {
      result.failed++;
      LOG_DIAG("CLEANUP", "FAILED to remove legacy %s", path);
    }
  }
}

// Empty the deleted-books trash (/.crosspoint/trash). Scoped STRICTLY to that
// folder — never touches books, progress, bookmarks, settings, or CRASH_INFO.
// Handles top-level files and one level of subdirectories.
void cleanupTrash(CleanupResult& result, bool dryRun) {
  constexpr const char* kTrash = "/.crosspoint/trash";
  FsFile dir = Storage.open(kTrash, O_RDONLY);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }
  struct Item {
    std::string path;
    bool isDir;
    uint32_t size;
  };
  std::vector<Item> items;
  char nameBuf[128];
  for (auto f = dir.openNextFile(); f; f = dir.openNextFile()) {
    f.getName(nameBuf, sizeof(nameBuf));
    const std::string name(nameBuf);
    const bool isDir = f.isDirectory();
    const uint32_t sz = isDir ? 0u : static_cast<uint32_t>(f.size());
    f.close();
    items.push_back({std::string(kTrash) + "/" + name, isDir, sz});
  }
  dir.close();

  for (const auto& it : items) {
    if (it.isDir) {
      uint32_t subBytes = 0;  // one level of contained files, for the size estimate
      FsFile sub = Storage.open(it.path.c_str(), O_RDONLY);
      if (sub && sub.isDirectory()) {
        for (auto sf = sub.openNextFile(); sf; sf = sub.openNextFile()) {
          if (!sf.isDirectory()) subBytes += static_cast<uint32_t>(sf.size());
          sf.close();
        }
      }
      if (sub) sub.close();
      if (dryRun || Storage.removeDir(it.path.c_str())) {
        result.removed++;
        result.bytesFreed += subBytes;
      } else {
        result.failed++;
      }
    } else {
      if (it.size == UINT32_MAX) continue;
      if (dryRun || Storage.remove(it.path.c_str())) {
        result.removed++;
        result.bytesFreed += it.size;
      } else {
        result.failed++;
      }
    }
  }
}

// Integer-only byte formatter (avoids float printf on the C3).
std::string formatBytes(uint32_t b) {
  if (b >= 1024u * 1024) {
    const uint32_t tenths = static_cast<uint32_t>((static_cast<uint64_t>(b) * 10) / (1024u * 1024));
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + " MB";
  }
  if (b >= 1024) return std::to_string(b / 1024) + " KB";
  return std::to_string(b) + " B";
}

}  // namespace

void CleanupStorageActivity::scanCleanup() {
  CleanupResult tmp, rep, trash;
  cleanupOrphansInDir("/", tmp, true);
  cleanupOrphansInDir("/.crosspoint", tmp, true);
  cleanupLegacyReports(rep, true);
  cleanupTrash(trash, true);
  tmpCount = tmp.removed;
  tmpBytes = tmp.bytesFreed;
  reportCount = rep.removed;
  reportBytes = rep.bytesFreed;
  trashCount = trash.removed;
  trashBytes = trash.bytesFreed;
}

void CleanupStorageActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  scanCleanup();
  state = WARNING;
  requestUpdate();
}

void CleanupStorageActivity::onExit() { ActivityWithSubactivity::onExit(); }

void CleanupStorageActivity::runCleanup() {
  LOG_DBG("CLEANUP", "Scanning SD for orphan tmp/junk files...");
  CleanupResult result;

  cleanupOrphansInDir("/", result, false);
  cleanupOrphansInDir("/.crosspoint", result, false);
  cleanupLegacyReports(result, false);
  cleanupTrash(result, false);

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
    const int total = tmpCount + reportCount + trashCount;
    const uint32_t totalBytes = tmpBytes + reportBytes + trashBytes;
    int y = pageHeight / 2 - 90;
    renderer.drawCenteredText(UI_10_FONT_ID, y, "Will remove (books & progress kept):", true, EpdFontFamily::REGULAR);
    y += 34;
    const std::string l1 = "Temp files: " + std::to_string(tmpCount) + " (" + formatBytes(tmpBytes) + ")";
    renderer.drawCenteredText(UI_10_FONT_ID, y, l1.c_str(), true);
    y += 28;
    const std::string l2 = "Old reports: " + std::to_string(reportCount) + " (" + formatBytes(reportBytes) + ")";
    renderer.drawCenteredText(UI_10_FONT_ID, y, l2.c_str(), true);
    y += 28;
    const std::string l3 = "Trash: " + std::to_string(trashCount) + " (" + formatBytes(trashBytes) + ")";
    renderer.drawCenteredText(UI_10_FONT_ID, y, l3.c_str(), true);
    y += 34;
    const std::string lt = "Total: " + std::to_string(total) + " items, " + formatBytes(totalBytes);
    renderer.drawCenteredText(UI_10_FONT_ID, y, lt.c_str(), true, EpdFontFamily::REGULAR);

    const auto labels = (total == 0) ? mappedInput.mapLabels(tr(STR_BACK), "", "", "")
                                     : mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_CLEAR_BUTTON), "", "");
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
