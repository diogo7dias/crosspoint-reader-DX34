#pragma once

#include <BdfFilename.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class GfxRenderer;
class MappedInputManager;

namespace crosspoint {
namespace fonts {

struct CustomFontEntry {
  std::string filename;  // "unifont-bold_16.bdf" (basename only)
  std::string fontName;  // "unifont" — family name WITHOUT the -variant tag
  uint16_t sizePt = 0;
  uint32_t glyphCount = 0;
  bool headerOk = false;
  bdf::BdfVariant variant = bdf::BdfVariant::Regular;
};

// Groups up to four BDF files (regular / bold / italic / bolditalic) under
// a single (fontName, sizePt) identity. Populated by
// scanAndQueuePrompts() alongside the flat entries_ list so callers that
// only care about families (picker, registerWithRenderer, manage-fonts
// screen) can iterate here instead of re-grouping.
struct CustomFontFamilyGroup {
  std::string fontName;
  uint16_t sizePt = 0;
  // Indices into CustomFontManager::entries_. -1 slot means variant absent.
  int variantEntryIdx[4] = {-1, -1, -1, -1};
};

// Singleton that scans /custom-font/ at boot, parses BDF headers, and queues
// install-prompts for any font the user has not already acknowledged (via
// state.json seenCustomFonts / skippedCustomFonts).
//
// Phase 1: prompt only — "Install" logs + marks seen, no glyph extraction.
// Phase 2 will hang real install + render integration off the same entries.
class CustomFontManager {
 public:
  static CustomFontManager& instance();

  // Open /custom-font/, enumerate *.bdf, parse headers. Build entries_ +
  // pendingPromptIdx_. Safe to call when SD is missing or dir is absent
  // (silent no-op). Caps total scan at 50 files.
  void scanAndQueuePrompts();

  // If a queued prompt exists, push CustomFontPromptActivity. The activity's
  // callbacks chain back into showNextPromptIfAny(onAllDismissed) so all
  // pending prompts are walked sequentially. When the queue empties (or is
  // empty on entry) onAllDismissed() runs.
  void showNextPromptIfAny(GfxRenderer& renderer, MappedInputManager& mappedInput,
                           std::function<void()> onAllDismissed);

bool hasPendingPrompt() const { return !pendingPromptIdx_.empty(); }
  const std::vector<CustomFontEntry>& entries() const { return entries_; }
  const std::vector<CustomFontFamilyGroup>& families() const { return families_; }

  // Unique family names (deduped across sizes). Preserves first-seen order
  // so the reader-settings picker is stable across boots.
  std::vector<std::string> uniqueFamilyNames() const;
  // Available pixel sizes for `name`, sorted ascending. Empty when the
  // family has no regular-variant .idx-installable entry.
  std::vector<uint8_t> sizesForFamily(const std::string& name) const;

  // Registers every family (one CustomFont per fontName) with the renderer
  // so the reader text path can dispatch to them. When multiple sizes of
  // one family are installed, the largest wins (Phase 2c+ will expose
  // per-size picker options). Safe to call after scanAndQueuePrompts() and
  // also after an install-button-triggered build completes.
  void registerWithRenderer(GfxRenderer& renderer);

  // Shrink every registered CustomFont's glyph cache to the minimum size,
  // freeing each slab so a single large contiguous allocation (epub section
  // ZIP dict = 32 KB) can succeed. Used by EpubReaderActivity before
  // createSectionFile to avoid the "REBOOT DEVICE" OOM screen when custom
  // fonts are fragmenting the heap. Caches rebuild lazily on next render.
  void trimAllCaches(GfxRenderer& renderer);

  // Delete every BDF + IDX file belonging to `fontName` from /custom-font/
  // and drop the font from the renderer. Re-scans the directory to rebuild
  // entries_/families_. Returns the number of files removed.
  //
  // The seenCustomFonts / skippedCustomFonts state entries tied to the
  // family's filenames are also pruned so a future re-add re-prompts
  // cleanly. When the active reader font was the deleted one,
  // SETTINGS.customFontName is cleared; the caller is responsible for
  // re-render / settings save if it cares about the visible state.
  size_t deleteFamily(const std::string& fontName, GfxRenderer& renderer);

 private:
  CustomFontManager() = default;
  // Rebuild families_ from entries_. Called at the tail of scanAndQueuePrompts.
  void rebuildFamilyGroups();

  std::vector<CustomFontEntry> entries_;
  std::vector<CustomFontFamilyGroup> families_;
  std::vector<size_t> pendingPromptIdx_;
};

}  // namespace fonts
}  // namespace crosspoint
