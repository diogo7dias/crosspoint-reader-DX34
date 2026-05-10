#pragma once

#include <EpdFont.h>
#include <EpdFontFamily.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class CrossPointSettings;
class GfxRenderer;

namespace crosspoint {
namespace binfont {
class EpdBinFontLoader;
}
namespace fonts {

struct CustomBinFontSize {
  uint16_t sizePt = 0;
  bool hasRegular = false;
  bool hasBold = false;
  bool hasItalic = false;
  bool hasBoldItalic = false;
};

struct CustomBinFontFamily {
  std::string name;
  std::vector<CustomBinFontSize> sizes;  // sorted ascending by sizePt
};

// Legal family name: [A-Za-z0-9][A-Za-z0-9_-]{0,31}. Enforced by the
// upload endpoint (path-traversal guard) and the scan filter.
bool isValidFamilyName(const std::string& name);

// Singleton owning the on-SD .bin (CPBN) custom-font inventory.
//
// At boot, main.cpp calls setRenderer() once and then scan(). Activation
// is single-family: a second activate() swaps out the previous one.
// All mutation paths route through the stashed renderer so callers
// never have to pass it.
class CustomBinFontManager {
 public:
  static CustomBinFontManager& instance();

  // Must be called once at boot, before scan()/activate()/delete*.
  void setRenderer(GfxRenderer* renderer) { renderer_ = renderer; }

  // Rebuild the in-memory family list from /custom-font/<family>/*.bin.
  // Bounded by family / size caps so a bogus SD can't stall boot.
  void scan();

  const std::vector<CustomBinFontFamily>& families() const { return families_; }
  std::vector<std::string> familyNames() const;
  std::vector<uint8_t> installedSizesFor(const std::string& name) const;

  // Open the four variant files for (name, size), wrap them in an
  // EpdFontFamily, register it with the renderer. Missing optional
  // variants fall back to regular at render time via the family's
  // own lookup. Atomic: on failure, the previous activation stays.
  bool activate(const std::string& name, uint16_t sizePt);

  // Drop the active registration from the renderer and release its
  // loader heap buffers. Idempotent.
  void deactivate();

  // Validate the on-SD CPBN header for the regular variant of (name,
  // sizePt) without opening it persistently. Used by the boot-time scan
  // to refuse a corrupt active font before any reader open is attempted.
  // True iff the file exists, parses as a valid CPBN v1 header, and
  // reports a sensible glyph/group count. The italic/bold variants are
  // not checked here — they fall back to regular at render time, so a
  // corrupt non-regular variant cannot brick the reader.
  static bool validateInstalledRegular(const std::string& name, uint16_t sizePt);

  // Delete every .bin under /custom-font/<name>/ (and the dir if
  // empty). If the active family was this one, deactivate first.
  size_t deleteFamily(const std::string& name);

  // Delete the (up to 4) variant files for (name, sizePt). Removes
  // the family dir if empty. Deactivates if the active family matches.
  size_t deleteFamilySize(const std::string& name, uint16_t sizePt);

 private:
  CustomBinFontManager() = default;
  ~CustomBinFontManager() = default;

  struct ActiveFamily {
    std::string name;
    uint16_t sizePt = 0;
    int fontId = 0;
    std::array<std::unique_ptr<binfont::EpdBinFontLoader>, 4> loaders;
    std::array<std::unique_ptr<EpdFont>, 4> fonts;
  };

  // Drops active_ and removes its fontId from the renderer. No-op if
  // either is missing — the only reason that happens is a delete
  // slipping in before setRenderer(), which is flagged with a log
  // line since it would otherwise leak the active heap until the
  // next activate() cycles it.
  void clearActive();

  std::vector<CustomBinFontFamily> families_;
  std::unique_ptr<ActiveFamily> active_;
  GfxRenderer* renderer_ = nullptr;
};

// One-shot cleanup of the previous BDF pipeline's files (.bdf / .idx)
// under /custom-font/. Guarded by a state.json flag; runs at most
// once per SD card.
size_t cleanupLegacyBdfFiles();

// Boot-time custom-font initialization. Stashes the renderer on the
// manager singleton, scans /custom-font/ for installed families, and
// reconciles `settings.fontFamily` with on-disk reality. If the
// active custom font is missing on disk or its CPBN header is
// malformed, settings are reset to the default built-in family
// (CHAREINK 12, crisp render mode) and persisted via saveToFile().
//
// Single call replaces the multi-step orchestration that used to live
// in main.cpp (setRenderer + scan + size lookup + header validate +
// settings mutation + save). Caller no longer needs to know the
// fallback policy or SETTINGS schema for fonts.
void bootInitializeCustomFonts(GfxRenderer& renderer, CrossPointSettings& settings);

}  // namespace fonts
}  // namespace crosspoint
