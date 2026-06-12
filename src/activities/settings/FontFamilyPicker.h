#pragma once

#include <cstddef>
#include <string>
#include <vector>

class GfxRenderer;

// Shared modal vertical-list picker for the reader font family, embedded as an
// in-place overlay by both SettingsActivity and ReaderSettingsActivity (neither
// can host a sub-activity cheaply, and the reader-settings path is daily-driver
// critical, so an overlay is safer than restructuring either activity).
//
// All rows render in the standard UI font: built-in families CAN be drawn in
// their own typeface for free, but installed custom .bin fonts are single-
// activation (one loadable at a time), so a uniform UI font keeps the list flat,
// fast and free of OOM/flicker risk.
//
// The picker owns only list state + drawing. The host activity drives open/nav/
// select and is responsible for applying the choice (applyFontFamilyByDisplayIndex)
// and persisting, so per-activity persist/feedback differences stay put.
namespace crosspoint::settings {

class FontFamilyPicker {
 public:
  // Build the row list (built-in families + installed custom fonts) and place
  // the cursor on the family currently selected in SETTINGS.
  void open();
  void close() { open_ = false; }
  bool isOpen() const { return open_; }

  void moveUp();
  void moveDown();

  // Display index of the highlighted row: [0, builtinCount) selects a built-in
  // family (via displayIndexToFontFamily); [builtinCount, ...) selects the Nth
  // installed custom family. Feed straight into applyFontFamilyByDisplayIndex().
  std::size_t selectedDisplayIndex() const { return static_cast<std::size_t>(selected_); }
  std::size_t builtinCount() const { return builtinCount_; }
  bool empty() const { return labels_.empty(); }

  // Draw the modal over the already-rendered frame. The caller draws the base
  // screen first and calls displayBuffer() afterwards.
  void render(GfxRenderer& renderer, int pageWidth, int pageHeight) const;

 private:
  bool open_ = false;
  int selected_ = 0;
  std::size_t builtinCount_ = 0;
  std::vector<std::string> labels_;
};

}  // namespace crosspoint::settings
