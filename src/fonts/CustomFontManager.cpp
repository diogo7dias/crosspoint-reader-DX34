#include "CustomFontManager.h"

#include <BdfFilename.h>
#include <BdfIndexBuilder.h>
#include <BdfParser.h>
#include <CustomFontGlyphSource.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_task_wdt.h>
#include <strings.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include "CrossPointState.h"
#include "activities/util/CustomFontPromptActivity.h"
#include "activities/util/FullScreenMessageActivity.h"

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

  // Cross-check vs persistent state. Prompt unless:
  //   - user said "Skip forever" (always honored), OR
  //   - user said "Install" AND the .idx file is on disk (genuine completed
  //     install). If .idx is missing, re-prompt so the build can re-run —
  //     this also retroactively triggers the Phase 2 index build for any
  //     font that was "installed" under the Phase 1 stub.
  for (size_t i = 0; i < entries_.size(); ++i) {
    const auto& fn = entries_[i].filename;
    if (contains(APP_STATE.skippedCustomFonts, fn)) continue;

    if (contains(APP_STATE.seenCustomFonts, fn)) {
      // .bdf -> .idx; same dir.
      std::string idxPath = std::string(kCustomFontDir) + "/" + fn;
      if (idxPath.size() >= 4) {
        idxPath.resize(idxPath.size() - 4);
        idxPath += ".idx";
      }
      if (Storage.exists(idxPath.c_str())) continue;
    }
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

    // Phase 2 Slice 2a: build the on-disk glyph index. This is the slow
    // part — for 9 MB Unifont it walks every line. Watchdog resets are
    // handled inside the parser (every 32 lines).
    char bdfPath[320];
    char idxPath[320];
    snprintf(bdfPath, sizeof(bdfPath), "%s/%s", kCustomFontDir, filename.c_str());
    // .bdf -> .idx
    std::string idxStr = "/custom-font/" + filename;
    if (idxStr.size() >= 4) {
      idxStr.resize(idxStr.size() - 4);
      idxStr += ".idx";
    }
    snprintf(idxPath, sizeof(idxPath), "%s", idxStr.c_str());

    // Visual feedback — without this the device sits silent for 30+ seconds
    // while buildIndex walks Unifont. Push a full-screen "Building..." page,
    // force its first frame onto the e-ink, then run the build synchronously.
    {
      std::string msg = "Building font index\n\n";
      msg += filename;
      msg += "\n\nThis may take 1-3 min\nDo not reboot";
      auto* progressActivity = new FullScreenMessageActivity(renderer, mappedInput, msg);
      enterNewActivity(progressActivity);
      progressActivity->requestUpdateAndWait();
    }

    LOG_INF(kModule, "building index: %s -> %s", bdfPath, idxPath);
    const auto build = bdf::BdfIndexBuilder::buildIndex(bdfPath, idxPath);
    if (!build.ok) {
      LOG_INF(kModule, "index build FAILED for %s: %s", filename.c_str(),
              build.error ? build.error : "unknown");
    } else {
      LOG_INF(kModule, "index built OK: %u glyphs (skipped %u) in %u ms",
              static_cast<unsigned>(build.glyphsWritten), static_cast<unsigned>(build.glyphsSkipped),
              static_cast<unsigned>(build.parseTimeMs));

      // Verification dump — open the source and resolve a few well-known
      // codepoints. Confirms the entire pipeline (idx → seek → bitmap decode).
      bdf::CustomFontGlyphSource src;
      if (src.open(bdfPath, idxPath)) {
        const uint32_t samples[] = {0x0041, 0x0042, 0x0048, 0x0069, 0x0420, 0x4E2D};  // A B H i Р 中
        for (uint32_t cp : samples) {
          const auto* g = src.lookup(cp);
          if (!g) {
            LOG_INF(kModule, "  cp U+%04X: not in font", static_cast<unsigned>(cp));
            continue;
          }
          LOG_INF(kModule, "  cp U+%04X: %ux%u adv=%u offX=%d offY=%d bitmapBytes=%u",
                  static_cast<unsigned>(cp), static_cast<unsigned>(g->bbxW), static_cast<unsigned>(g->bbxH),
                  static_cast<unsigned>(g->advance), static_cast<int>(g->bbxOffX), static_cast<int>(g->bbxOffY),
                  static_cast<unsigned>(g->bitmapBytes));
        }
        src.close();
      } else {
        LOG_INF(kModule, "verification: failed to reopen %s+%s", bdfPath, idxPath);
      }
    }

    // Result screen — brief, then chain. Even on failure the user sees
    // something other than the build screen.
    {
      char doneMsg[200];
      if (build.ok) {
        snprintf(doneMsg, sizeof(doneMsg), "Font installed\n\n%u glyphs\n%u ms",
                 static_cast<unsigned>(build.glyphsWritten), static_cast<unsigned>(build.parseTimeMs));
      } else {
        snprintf(doneMsg, sizeof(doneMsg), "Install failed\n\n%s", build.error ? build.error : "unknown");
      }
      auto* doneActivity = new FullScreenMessageActivity(renderer, mappedInput, doneMsg);
      enterNewActivity(doneActivity);
      doneActivity->requestUpdateAndWait();
      delay(2500);
    }

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
