#pragma once

#include <BdfFilename.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace crosspoint {
namespace bdf {
class CustomFont;
}
}  // namespace crosspoint

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
// Phase 2: real install + render integration + single-active registration
// (PR #69).
// Phase 3 (in progress): architecture + performance refactor — see the
// v2 PR for the roadmap (lazy variant loading, per-font cache sizing,
// prewarm, zero-copy decode, etc.).
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

  // Log a one-line heap snapshot (free / largest contiguous / min-free-ever)
  // under a caller-supplied tag. Cheap; guarded by a compile-time verbose
  // flag inside the .cpp. Call at the ends of the font-heap dance so a
  // regression is diagnosable from logs alone.
  static void logHeapSnapshot(const char* tag);

  // Dump active custom font's cache stats (hits / misses / evictions /
  // mean decode µs) and reset them. Call pre/post render to isolate a
  // single frame. No-op when no custom font is registered.
  void logAndResetCacheStats(const char* tag, GfxRenderer& renderer);

  // Fully release every registered CustomFont's glyph slab so the heap can
  // coalesce a 32 KB+ contiguous block for the epub section ZIP dictionary.
  // Unlike the old trimAllCaches (which kept 1 slot alive and left slots_ /
  // cacheMap_ in the way of the region we actually wanted), this drops
  // EVERYTHING: slab pointer, slots vector, hash map buckets. Call
  // restoreAllCaches() after the contiguous allocation has succeeded — a
  // cached cap/budget per font brings the cache back to its prior shape
  // and the prewarm scan pass re-fills it on the next frame.
  void releaseAllCaches(GfxRenderer& renderer);

  // Inverse of releaseAllCaches(). Safe to call even if nothing was released.
  void restoreAllCaches(GfxRenderer& renderer);

  // Legacy alias kept for call sites that still want a "make the cache
  // small" knob. New code should prefer releaseAllCaches/restoreAllCaches
  // for the book-open memory dance.
  void trimAllCaches(GfxRenderer& renderer) { releaseAllCaches(renderer); }

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

  // Delete only the BDF + IDX files for a single (fontName, sizePt) pair —
  // up to four variant files. State.json seen/skipped entries are pruned
  // per filename. If the currently-active reader font is the deleted
  // (name, size), falls back to another installed size of the same family
  // (smallest size ≥ deleted; else largest size <); if no sizes remain,
  // reverts to CHAREINK 12 crisp — same hard-reset deleteFamily uses.
  // Returns files removed.
  size_t deleteFamilySize(const std::string& fontName, uint16_t sizePt, GfxRenderer& renderer);

 private:
  CustomFontManager();
  ~CustomFontManager();
  // Rebuild families_ from entries_. Called at the tail of scanAndQueuePrompts.
  void rebuildFamilyGroups();

  std::vector<CustomFontEntry> entries_;
  std::vector<CustomFontFamilyGroup> families_;
  std::vector<size_t> pendingPromptIdx_;

  // Owned CustomFont objects. Renderer holds borrowed raw pointers keyed by
  // the same fontId. Keys are idForFamily(name, sizePt). We previously
  // let the renderer own these via new/delete in insertCustomFont, which
  // made ownership ambiguous on error paths (a failed open would leak, and
  // deleteFamily had to reach into renderer internals to clean up).
  std::unordered_map<int, std::unique_ptr<crosspoint::bdf::CustomFont>> ownedFonts_;
};

}  // namespace fonts
}  // namespace crosspoint
