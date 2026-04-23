#pragma once

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
  std::string filename;  // "unifont_16.bdf" (basename only)
  std::string fontName;  // "unifont"
  uint16_t sizePt = 0;
  uint32_t glyphCount = 0;
  bool headerOk = false;
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

 private:
  CustomFontManager() = default;

  std::vector<CustomFontEntry> entries_;
  std::vector<size_t> pendingPromptIdx_;
};

}  // namespace fonts
}  // namespace crosspoint
