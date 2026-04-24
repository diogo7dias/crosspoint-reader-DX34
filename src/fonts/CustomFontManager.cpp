#include "CustomFontManager.h"

#include <BdfFilename.h>
#include <BdfIndexBuilder.h>
#include <BdfParser.h>
#include <CustomFont.h>
#include <CustomFontGlyphSource.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <strings.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "activities/util/CustomFontInstallProgressActivity.h"
#include "activities/util/CustomFontPromptActivity.h"
#include "activities/util/FullScreenMessageActivity.h"
#include "fonts/CustomFontIds.h"

// Defined in main.cpp. enterNewActivity only swaps the pointer — callers must
// call exitActivity() first to delete the previous activity, otherwise every
// prompt/skip/install chain leaks the outgoing activity and the heap collapses.
class Activity;
void enterNewActivity(Activity* activity);
void exitActivity();

namespace crosspoint {
namespace fonts {

namespace {

constexpr const char* kModule = "CFONT";
constexpr const char* kCustomFontDir = "/custom-font";
constexpr size_t kScanCap = 1000;        // hard limit on .bdf files scanned per boot
constexpr size_t kScanHeapFloorBytes = 150 * 1024; // stop scanning if free heap drops below this
constexpr size_t kWdtResetInterval = 32; // reset watchdog every N dir entries

// Phase 2b LRU cache budget. Per-variant slab = slots * maxBitmapBytes, where
// maxBitmapBytes scales with the font's bounding box. For a 32pt display font
// with bbx 112x46 that is ~644 B/slot. Only the reader-active family gets the
// full cap. We now size by byte-budget to ensure tiny fonts get more slots
// and huge fonts don't OOM.
// section-cache's 32 KB contiguous block (which shows the OOM/reboot screen
// when it fails).
constexpr size_t kCustomFontCacheBudgetBytes = 16 * 1024;
constexpr size_t kCacheSlotsInactive = 4;

// Stop opening more families when the largest contiguous free block drops
// below this threshold. Leaves room for render buffers, epub section cache,
// webserver, etc. Also protects against fragmentation — free heap may be
// higher but split into pieces too small for a single slab.
constexpr size_t kHeapFloorForNextFamilyBytes = 56 * 1024;

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

namespace {

// Build "<dir>/<stem>.idx" from "<stem>.bdf" filename.
std::string idxPathFor(const std::string& filename) {
  std::string out = std::string("/custom-font/") + filename;
  if (out.size() >= 4) {
    out.resize(out.size() - 4);
    out += ".idx";
  }
  return out;
}

}  // namespace

std::vector<std::string> CustomFontManager::uniqueFamilyNames() const {
  std::vector<std::string> out;
  for (const auto& g : families_) {
    if (g.variantEntryIdx[bdf::CustomFont::SLOT_REGULAR] < 0) continue;
    bool seen = false;
    for (const auto& n : out) {
      if (n == g.fontName) { seen = true; break; }
    }
    if (!seen) out.push_back(g.fontName);
  }
  return out;
}

std::vector<uint8_t> CustomFontManager::sizesForFamily(const std::string& name) const {
  std::vector<uint8_t> out;
  for (const auto& g : families_) {
    if (g.fontName != name) continue;
    if (g.variantEntryIdx[bdf::CustomFont::SLOT_REGULAR] < 0) continue;
    const auto sz = static_cast<uint8_t>(g.sizePt);
    bool seen = false;
    for (auto s : out) {
      if (s == sz) { seen = true; break; }
    }
    if (!seen) out.push_back(sz);
  }
  std::sort(out.begin(), out.end());
  return out;
}

size_t CustomFontManager::deleteFamily(const std::string& fontName, GfxRenderer& renderer) {
  if (fontName.empty()) return 0;
  // Collect filenames first so erasing from entries_ under iteration is safe.
  std::vector<std::string> toRemove;
  for (const auto& e : entries_) {
    if (e.fontName == fontName) toRemove.push_back(e.filename);
  }

  size_t removed = 0;
  for (const auto& filename : toRemove) {
    char bdfPath[320];
    snprintf(bdfPath, sizeof(bdfPath), "%s/%s", kCustomFontDir, filename.c_str());
    const std::string idx = idxPathFor(filename);
    const bool bdfOk = Storage.remove(bdfPath);
    const bool idxOk = Storage.exists(idx.c_str()) ? Storage.remove(idx.c_str()) : true;
    if (bdfOk) ++removed;
    LOG_INF(kModule, "delete %s: bdf=%d idx=%d", filename.c_str(), bdfOk ? 1 : 0, idxOk ? 1 : 0);

    // Prune persisted seen/skipped state so future re-adds re-prompt.
    APP_STATE.seenCustomFonts.erase(
        std::remove(APP_STATE.seenCustomFonts.begin(), APP_STATE.seenCustomFonts.end(), filename),
        APP_STATE.seenCustomFonts.end());
    APP_STATE.skippedCustomFonts.erase(
        std::remove(APP_STATE.skippedCustomFonts.begin(), APP_STATE.skippedCustomFonts.end(), filename),
        APP_STATE.skippedCustomFonts.end());
  }
  if (!toRemove.empty()) APP_STATE.saveToFile();

  // Drop any renderer dispatch pointing at the deleted family so the reader
  // path doesn't try to lookup glyphs from a closed CustomFontGlyphSource.
  // Walk families_ (still reflecting pre-delete state) to find every size
  // we registered for this name and remove each id.
  for (const auto& g : families_) {
    if (g.fontName == fontName) {
      renderer.removeCustomFont(idForFamily(g.fontName, g.sizePt));
    }
  }

  // Clear active selection if it was this family and revert to safe defaults
  // (CHAREINK 12, crisp render mode) so reader stays readable.
  if (SETTINGS.customFontName == fontName) {
    SETTINGS.customFontName.clear();
    SETTINGS.customFontSizePt = 0;
    if (SETTINGS.fontFamily == CrossPointSettings::CUSTOM_FAMILY) {
      SETTINGS.fontFamily = CrossPointSettings::CHAREINK;
      SETTINGS.fontSize = CrossPointSettings::SIZE_12;
      SETTINGS.textRenderMode = CrossPointSettings::TEXT_RENDER_CRISP;
    }
    SETTINGS.saveToFile();
  }

  // Rebuild entries_/families_ from SD and re-register remaining families.
  scanAndQueuePrompts();
  registerWithRenderer(renderer);
  return removed;
}

void CustomFontManager::registerWithRenderer(GfxRenderer& renderer) {
  // Register every (name, sizePt) family group that has a regular variant
  // + .idx on disk. Each group binds to its own hashed fontId so that
  // users with multiple sizes of the same family (e.g. fontA_17 +
  // fontA_20) can switch between them via the reader Font Size picker
  // without re-scanning SD.
  static constexpr const char* kSlotNames[4] = {"regular", "bold", "italic", "bolditalic"};
  // Only the user's currently-active family is registered in the renderer.
  // Every previous attempt to preload multiple families fragmented the
  // ESP32-C3 heap badly enough that the 32 KB epub section ZIP dict could
  // no longer coalesce, crashing book open with a `std::terminate` from
  // operator new. Non-active families are still visible in the picker
  // (from `entries_`/`families_`) — selecting one triggers re-register
  // via ReaderSettingsActivity → this function.
  //
  // Clear prior registrations first so a font switch drops the old slab
  // before the new one is allocated. `insertCustomFont` already does this
  // on collision, but the ID changes across families so explicit sweep is
  // required.
  for (const auto& g : families_) {
    renderer.removeCustomFont(idForFamily(g.fontName, g.sizePt));
  }

  if (SETTINGS.customFontName.empty() || SETTINGS.customFontSizePt == 0) {
    LOG_INF(kModule, "no active custom font; renderer left without custom registrations");
    return;
  }

  const CustomFontFamilyGroup* activeGroup = nullptr;
  for (const auto& g : families_) {
    if (g.fontName == SETTINGS.customFontName && g.sizePt == SETTINGS.customFontSizePt) {
      activeGroup = &g;
      break;
    }
  }
  if (!activeGroup) {
    LOG_INF(kModule, "active font '%s' @ %upt not present in scanned families",
            SETTINGS.customFontName.c_str(), static_cast<unsigned>(SETTINGS.customFontSizePt));
    return;
  }

  // Single-family loop — keeps the original variant/.idx handling code
  // below untouched. `continue` inside this loop always falls through to
  // the end of the function, which is the desired behaviour.
  for (const auto* gp : {activeGroup}) {
    const auto& g = *gp;
    if (g.variantEntryIdx[bdf::CustomFont::SLOT_REGULAR] < 0) continue;

    const int regIdx = g.variantEntryIdx[bdf::CustomFont::SLOT_REGULAR];
    const auto& regEntry = entries_[regIdx];
    char bdfPath[320];
    snprintf(bdfPath, sizeof(bdfPath), "%s/%s", kCustomFontDir, regEntry.filename.c_str());
    const std::string regIdxPath = idxPathFor(regEntry.filename);
    if (!Storage.exists(regIdxPath.c_str())) {
      LOG_INF(kModule, "skip register: regular .idx missing for %s", regEntry.filename.c_str());
      continue;
    }

    const size_t familyCacheBudgetBytes = kCustomFontCacheBudgetBytes;

    const size_t largestFree = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (largestFree < kHeapFloorForNextFamilyBytes) {
      LOG_INF(kModule, "skip register: heap floor reached (largest=%u B) at %s",
              static_cast<unsigned>(largestFree), regEntry.filename.c_str());
      continue;
    }

    auto* font = new (std::nothrow) bdf::CustomFont();
    if (font == nullptr) {
      LOG_INF(kModule, "alloc CustomFont failed for %s", regEntry.filename.c_str());
      continue;
    }
    
    font->setSizePt(g.sizePt);
    font->openVariant(bdf::CustomFont::SLOT_REGULAR, bdfPath, regIdxPath.c_str(), familyCacheBudgetBytes);

    for (size_t slot = bdf::CustomFont::SLOT_BOLD; slot < 4; ++slot) {
      const int idx = g.variantEntryIdx[slot];
      if (idx < 0) continue;
      const auto& ve = entries_[idx];
      char vBdfPath[320];
      snprintf(vBdfPath, sizeof(vBdfPath), "%s/%s", kCustomFontDir, ve.filename.c_str());
      const std::string vIdxPath = idxPathFor(ve.filename);
      if (!Storage.exists(vIdxPath.c_str())) {
        LOG_INF(kModule, "variant %s present but .idx missing: %s", kSlotNames[slot], ve.filename.c_str());
        continue;
      }
      if (!font->openVariant(slot, vBdfPath, vIdxPath.c_str(), familyCacheBudgetBytes)) {
        LOG_INF(kModule, "variant %s open failed: %s", kSlotNames[slot], ve.filename.c_str());
        continue;
      }
      LOG_INF(kModule, "variant %s loaded: %s", kSlotNames[slot], ve.filename.c_str());
    }

    const int fontId = idForFamily(g.fontName, g.sizePt);
    LOG_INF(kModule, "registered custom font '%s' @ id=0x%08X (size=%upt, variants=%s%s%s%s)", g.fontName.c_str(),
            static_cast<unsigned>(fontId), static_cast<unsigned>(g.sizePt),
            font->hasVariant(bdf::CustomFont::SLOT_REGULAR) ? "R" : "-",
            font->hasVariant(bdf::CustomFont::SLOT_BOLD) ? "B" : "-",
            font->hasVariant(bdf::CustomFont::SLOT_ITALIC) ? "I" : "-",
            font->hasVariant(bdf::CustomFont::SLOT_BOLD_ITALIC) ? "Z" : "-");
    renderer.insertCustomFont(fontId, font);
  }
}

void CustomFontManager::trimAllCaches(GfxRenderer& renderer) {
  size_t trimmed = 0;
  for (const auto& g : families_) {
    const int fontId = idForFamily(g.fontName, g.sizePt);
    if (auto* font = renderer.findCustomFont(fontId)) {
      font->trimCache(1);
      ++trimmed;
    }
  }
  LOG_INF(kModule, "trimmed %u custom font caches (largest free now=%u B)",
          static_cast<unsigned>(trimmed),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
}

void CustomFontManager::scanAndQueuePrompts() {
  entries_.clear();
  families_.clear();
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

    const size_t largestFree = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (largestFree < kScanHeapFloorBytes) {
      LOG_INF(kModule, "scan heap budget reached (largest=%u B); stopping at %u files", 
              static_cast<unsigned>(largestFree), static_cast<unsigned>(scanned));
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
    entry.variant = fname->variant;
    entries_.push_back(std::move(entry));
    ++valid;

    LOG_INF(kModule, "%s header OK glyphs=%u bbx=%dx%d ascent=%d descent=%d variant=%u", name,
            static_cast<unsigned>(header.glyphCount), header.bbxW, header.bbxH, header.ascent, header.descent,
            static_cast<unsigned>(fname->variant));

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

  rebuildFamilyGroups();
}

void CustomFontManager::rebuildFamilyGroups() {
  families_.clear();
  // Linear scan; entries_ is capped at 50 so N^2 is fine. Group key is
  // (fontName, sizePt); within a group each variant gets its own slot.
  for (size_t i = 0; i < entries_.size(); ++i) {
    const auto& e = entries_[i];
    if (!e.headerOk) continue;
    CustomFontFamilyGroup* group = nullptr;
    for (auto& g : families_) {
      if (g.fontName == e.fontName && g.sizePt == e.sizePt) {
        group = &g;
        break;
      }
    }
    if (group == nullptr) {
      families_.push_back({});
      group = &families_.back();
      group->fontName = e.fontName;
      group->sizePt = e.sizePt;
    }
    const auto slot = static_cast<size_t>(e.variant);
    if (slot < 4) {
      if (group->variantEntryIdx[slot] >= 0) {
        LOG_INF(kModule, "duplicate variant %u for %s %upt — keeping first (%s)", static_cast<unsigned>(slot),
                e.fontName.c_str(), static_cast<unsigned>(e.sizePt),
                entries_[group->variantEntryIdx[slot]].filename.c_str());
      } else {
        group->variantEntryIdx[slot] = static_cast<int>(i);
      }
    }
  }
  LOG_INF(kModule, "family groups: %u", static_cast<unsigned>(families_.size()));
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

    auto onComplete = [this, filename, bdfPath = std::string(bdfPath), idxPath = std::string(idxPath), &renderer, &mappedInput, onAllDismissed](crosspoint::bdf::BuildIndexResult build) {
      if (!build.ok) {
        LOG_INF(kModule, "index build FAILED for %s: %s", filename.c_str(),
                build.error ? build.error : "unknown");
      } else {
        // Register with the renderer right away so the user can pick the
        // font immediately after install without rebooting.
        registerWithRenderer(renderer);
        LOG_INF(kModule, "index built OK: %u glyphs (skipped %u) in %u ms",
                static_cast<unsigned>(build.glyphsWritten), static_cast<unsigned>(build.glyphsSkipped),
                static_cast<unsigned>(build.parseTimeMs));

        // Verification dump — open the source and resolve a few well-known
        // codepoints. Confirms the entire pipeline (idx → seek → bitmap decode).
        bdf::CustomFontGlyphSource src;
        if (src.open(bdfPath.c_str(), idxPath.c_str())) {
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
          LOG_INF(kModule, "verification: failed to reopen %s+%s", bdfPath.c_str(), idxPath.c_str());
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
        exitActivity();
        enterNewActivity(doneActivity);
        doneActivity->requestUpdateAndWait();
        delay(2500);
      }

      showNextPromptIfAny(renderer, mappedInput, onAllDismissed);
    };

    auto* progressActivity = new CustomFontInstallProgressActivity(renderer, mappedInput, filename, std::string(bdfPath), std::string(idxPath), onComplete);
    exitActivity();
    enterNewActivity(progressActivity);
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

  exitActivity();
  enterNewActivity(new CustomFontPromptActivity(renderer, mappedInput, body, std::move(onInstall), std::move(onSkip),
                                                std::move(onSkipForever)));
}

}  // namespace fonts
}  // namespace crosspoint
