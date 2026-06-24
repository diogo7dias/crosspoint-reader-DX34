#pragma once

#include <I18n.h>

#include <string>
#include <vector>

class GfxRenderer;

// Generic modal vertical-list picker for any ENUM setting with more than two
// options, embedded as an in-place overlay by SettingsActivity and
// ReaderSettingsActivity. Same rationale as FontFamilyPicker: neither activity
// hosts a sub-activity cheaply and the reader-settings path is daily-driver
// critical, so an overlay is safer than restructuring either activity.
//
// It replaces click-to-cycle for multi-option settings (e.g. a status-item
// position with six choices) so the value is picked in one shot instead of
// tapping through every option. The picker owns only list state + drawing; the
// host activity drives open/nav/select and applies + persists the result, so
// per-activity differences stay put. Two-option and toggle settings keep their
// single-click behaviour and never open this.
namespace crosspoint::settings {

class EnumOptionPicker {
 public:
  // Build the row list from already-resolved display labels and place the
  // cursor on currentIndex. title is shown at the top of the modal.
  void open(StrId title, std::vector<std::string> labels, int currentIndex);
  void close() { open_ = false; }
  bool isOpen() const { return open_; }

  void moveUp();
  void moveDown();

  int selectedIndex() const { return selected_; }
  bool empty() const { return labels_.empty(); }

  // Draw the modal over the already-rendered frame. The caller draws the base
  // screen first and calls displayBuffer() afterwards.
  void render(GfxRenderer& renderer, int pageWidth, int pageHeight) const;

 private:
  bool open_ = false;
  int selected_ = 0;
  StrId title_ = StrId::STR_NONE_OPT;
  std::vector<std::string> labels_;
};

}  // namespace crosspoint::settings
