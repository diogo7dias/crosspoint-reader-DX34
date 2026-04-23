#include "CustomFontManager.h"

#include <BdfFilename.h>
#include <BdfParser.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_task_wdt.h>
#include <strings.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include "CrossPointState.h"
#include "activities/util/CustomFontPromptActivity.h"

// Defined in main.cpp; replaces the current activity slot.
class Activity;
void enterNewActivity(Activity* activity);

namespace crosspoint {
namespace fonts {

namespace {

constexpr const char* kModule = "CFONT";
constexpr const char* kCustomFontDir = "/custom-font";
constexpr size_t kScanCap = 50;          // hard limit on .bdf files scanned per boot
constexpr size_t kWdtResetInterval = 32; // reset watchdog every N dir entries

bool isBdfName(const char* name) {
  if (!name || name[0] == '\0' || name[0] == '.') return false;
  const size_t len = std::strlen(name);
  if (len < 5) return false;  // "x.bdf"
  return strcasecmp(name + len - 4, ".bdf") == 0;
}

bool contains(const std::vector<std::string>& v, const std::string& needle) {
  return std::find(v.begin(), v.end(), needle) != v.end();
}

}  // namespace

CustomFontManager& CustomFontManager::instance() {
  static CustomFontManager inst;
  return inst;
}

void CustomFontManager::scanAndQueuePrompts() {
  entries_.clear();
  pendingPromptIdx_.clear();

  auto dir = Storage.open(kCustomFontDir);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    LOG_DBG(kModule, "%s missing or not a directory; skipping scan", kCustomFontDir);
    return;
  }

  size_t scanned = 0;
  size_t iter = 0;
  size_t valid = 0;
  char name[256];

  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (file.isDirectory()) {
      file.close();
      continue;
    }
    file.getName(name, sizeof(name));
    if (!isBdfName(name)) {
      file.close();
      if (++iter % kWdtResetInterval == 0) {
        esp_task_wdt_reset();
        yield();
      }
      continue;
    }
    if (++scanned > kScanCap) {
      LOG_INF(kModule, "scan cap reached (%u files); stopping", static_cast<unsigned>(kScanCap));
      file.close();
      break;
    }

    const auto fname = bdf::parseBdfFilename(name);
    if (!fname.has_value()) {
      LOG_INF(kModule, "%s filename invalid, skipping", name);
      file.close();
      if (++iter % kWdtResetInterval == 0) {
        esp_task_wdt_reset();
        yield();
      }
      continue;
    }

    // file is already open at the directory entry. We need a fresh open at the
    // full path because openNextFile() does not give us a Stream we can re-seek
    // to position 0 (some HAL impls leave the cursor mid-file). Close + reopen.
    file.close();

    char fullPath[320];
    snprintf(fullPath, sizeof(fullPath), "%s/%s", kCustomFontDir, name);
    auto bdfFile = Storage.open(fullPath);
    if (!bdfFile) {
      LOG_INF(kModule, "%s open failed, skipping", name);
      if (++iter % kWdtResetInterval == 0) {
        esp_task_wdt_reset();
        yield();
      }
      continue;
    }

    const auto header = bdf::BdfParser::readHeader(bdfFile);
    bdfFile.close();

    if (!header.ok) {
      LOG_INF(kModule, "%s header error: %s", name, header.error ? header.error : "unknown");
      if (++iter % kWdtResetInterval == 0) {
        esp_task_wdt_reset();
        yield();
      }
      continue;
    }

    CustomFontEntry entry;
    entry.filename = name;
    entry.fontName = fname->name;
    entry.sizePt = fname->sizePt;
    entry.glyphCount = header.glyphCount;
    entry.headerOk = true;
    entries_.push_back(std::move(entry));
    ++valid;

    LOG_INF(kModule, "%s header OK glyphs=%u bbx=%dx%d ascent=%d descent=%d", name,
            static_cast<unsigned>(header.glyphCount), header.bbxW, header.bbxH, header.ascent, header.descent);

    if (++iter % kWdtResetInterval == 0) {
      esp_task_wdt_reset();
      yield();
    }
  }
  dir.close();

  // Cross-check vs persistent state.
  for (size_t i = 0; i < entries_.size(); ++i) {
    const auto& fn = entries_[i].filename;
    if (contains(APP_STATE.skippedCustomFonts, fn)) continue;
    if (contains(APP_STATE.seenCustomFonts, fn)) continue;
    pendingPromptIdx_.push_back(i);
  }

  LOG_INF(kModule, "scanned %u files, %u valid, %u queued", static_cast<unsigned>(scanned),
          static_cast<unsigned>(valid), static_cast<unsigned>(pendingPromptIdx_.size()));
}

void CustomFontManager::showNextPromptIfAny(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                            std::function<void()> onAllDismissed) {
  if (pendingPromptIdx_.empty()) {
    if (onAllDismissed) onAllDismissed();
    return;
  }

  const size_t idx = pendingPromptIdx_.front();
  pendingPromptIdx_.erase(pendingPromptIdx_.begin());
  const auto& entry = entries_[idx];

  char body[160];
  snprintf(body, sizeof(body), "Found custom font:\n%s (%upt)\n%u glyphs", entry.fontName.c_str(),
           static_cast<unsigned>(entry.sizePt), static_cast<unsigned>(entry.glyphCount));

  // All three lambdas capture `this` (singleton — stable for program lifetime),
  // the entry filename by value, the renderer/input refs, and onAllDismissed
  // by value so we can chain another prompt.
  const std::string filename = entry.filename;

  auto onInstall = [this, filename, &renderer, &mappedInput, onAllDismissed]() {
    if (!std::any_of(APP_STATE.seenCustomFonts.begin(), APP_STATE.seenCustomFonts.end(),
                     [&](const std::string& s) { return s == filename; })) {
      APP_STATE.seenCustomFonts.push_back(filename);
      APP_STATE.saveToFile();
    }
    LOG_INF(kModule, "install requested: %s", filename.c_str());
    showNextPromptIfAny(renderer, mappedInput, onAllDismissed);
  };
  auto onSkip = [this, filename, &renderer, &mappedInput, onAllDismissed]() {
    LOG_INF(kModule, "skip (this boot): %s", filename.c_str());
    showNextPromptIfAny(renderer, mappedInput, onAllDismissed);
  };
  auto onSkipForever = [this, filename, &renderer, &mappedInput, onAllDismissed]() {
    if (!std::any_of(APP_STATE.skippedCustomFonts.begin(), APP_STATE.skippedCustomFonts.end(),
                     [&](const std::string& s) { return s == filename; })) {
      APP_STATE.skippedCustomFonts.push_back(filename);
      APP_STATE.saveToFile();
    }
    LOG_INF(kModule, "skip forever: %s", filename.c_str());
    showNextPromptIfAny(renderer, mappedInput, onAllDismissed);
  };

  enterNewActivity(new CustomFontPromptActivity(renderer, mappedInput, body, std::move(onInstall), std::move(onSkip),
                                                std::move(onSkipForever)));
}

}  // namespace fonts
}  // namespace crosspoint
