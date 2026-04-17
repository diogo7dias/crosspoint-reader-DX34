#pragma once

#include <Arduino.h>
#include <GfxRenderer.h>

#include "ButtonNavigator.h"

class MappedInputManager;

/**
 * Shared on-screen keyboard widget with dual-label keys and double-tap support.
 *
 * Each key shows both primary (bottom, large) and shift (top-right, small) characters.
 * Double-tap Select on a key toggles between primary and shift char.
 *
 * This is NOT an Activity — it's a reusable rendering/input component.
 * The owning Activity controls text buffer, positioning, and display refresh.
 */
class KeyboardWidget {
 public:
  // Result of a confirm/select press
  enum class KeyAction { None, Char, ShiftToggle, Space, Backspace, Done };

  struct KeyResult {
    KeyAction action = KeyAction::None;
    char character = '\0';
    bool isDoubleTap = false;  // true if this Char replaces the previously typed char
  };

  KeyboardWidget() = default;

  /**
   * Handle D-pad navigation. Call each frame from activity's loop().
   * @return true if selection changed (caller should requestUpdate)
   */
  bool handleNavigation(ButtonNavigator& nav, MappedInputManager& input);

  /**
   * Handle confirm/select press with double-tap support.
   * Call when wasPressed(Confirm) is true.
   * @param textLength current length of text buffer (needed for double-tap backspace check)
   * @return KeyResult describing what happened
   */
  KeyResult handleConfirm(size_t textLength);

  /**
   * Render keyboard at the given Y position. Draws all rows centered on screen.
   * @param renderer the GfxRenderer to draw with
   * @param topY Y pixel position for the first keyboard row
   */
  void render(const GfxRenderer& renderer, int topY) const;

  /** Total pixel height of the keyboard block (hint line + all rows + spacing). */
  int getTotalHeight(const GfxRenderer& renderer) const;

  /** Width of the widest row (for centering calculations). */
  static constexpr int getMaxRowWidth() { return KEYS_PER_ROW * (KEY_WIDTH + KEY_SPACING); }

  // Layout constants (public for callers that need positioning math)
  static constexpr int NUM_ROWS = 5;
  static constexpr int KEYS_PER_ROW = 13;
  static constexpr int KEY_WIDTH = 34;
  static constexpr int KEY_HEIGHT = 38;
  static constexpr int KEY_SPACING = 3;

 private:
  // Selection state
  int selectedRow = 0;
  int selectedCol = 0;
  int shiftState = 0;  // 0=lower, 1=upper, 2=lock

  // Double-tap state
  unsigned long lastConfirmTime = 0;
  int lastConfirmRow = -1;
  int lastConfirmCol = -1;
  bool lastWasDoubleTappable = false;
  static constexpr unsigned long DOUBLE_TAP_MS = 350;

  // Keyboard layout
  static const char* const keyboard[NUM_ROWS];
  static const char* const keyboardShift[NUM_ROWS];

  // Special key positions (bottom row)
  static constexpr int SPECIAL_ROW = 4;
  static constexpr int SHIFT_COL = 0;
  static constexpr int SPACE_COL = 2;
  static constexpr int BACKSPACE_COL = 7;
  static constexpr int DONE_COL = 9;

  int getRowLength(int row) const;
  char getSelectedChar() const;

  void renderKey(const GfxRenderer& renderer, int x, int y, int w, int h, const char* mainLabel, const char* shiftLabel,
                 bool isSelected) const;
  void renderSpecialKey(const GfxRenderer& renderer, int x, int y, int w, int h, const char* label,
                        bool isSelected) const;
};
