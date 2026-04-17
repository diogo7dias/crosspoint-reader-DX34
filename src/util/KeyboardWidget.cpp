#include "KeyboardWidget.h"

#include <I18n.h>

#include "MappedInputManager.h"
#include "fontIds.h"

// ─── Layout data ────────────────────────────────────────────────
const char* const KeyboardWidget::keyboard[NUM_ROWS] = {
    "`1234567890-=", "qwertyuiop[]\\", "asdfghjkl;'", "zxcvbnm,./",
    "^  _____<OK"  // ^ = shift, _ = space, < = backspace, OK = done
};

const char* const KeyboardWidget::keyboardShift[NUM_ROWS] = {"~!@#$%^&*()_+", "QWERTYUIOP{}|", "ASDFGHJKL:\"",
                                                             "ZXCVBNM<>?", "SPECIAL ROW"};

// ─── Helpers ────────────────────────────────────────────────────
int KeyboardWidget::getRowLength(const int row) const {
  switch (row) {
    case 0:
      return 13;
    case 1:
      return 13;
    case 2:
      return 11;
    case 3:
      return 10;
    case 4:
      return 10;
    default:
      return 0;
  }
}

char KeyboardWidget::getSelectedChar() const {
  const char* const* layout = shiftState ? keyboardShift : keyboard;
  if (selectedRow < 0 || selectedRow >= NUM_ROWS) return '\0';
  if (selectedCol < 0 || selectedCol >= getRowLength(selectedRow)) return '\0';
  return layout[selectedRow][selectedCol];
}

int KeyboardWidget::getTotalHeight(const GfxRenderer& renderer) const {
  const int hintHeight = renderer.getLineHeight(SMALL_FONT_ID) + 4;
  return hintHeight + NUM_ROWS * (KEY_HEIGHT + KEY_SPACING) - KEY_SPACING;
}

// ─── Navigation ─────────────────────────────────────────────────
bool KeyboardWidget::handleNavigation(ButtonNavigator& nav, MappedInputManager& input) {
  bool changed = false;

  nav.onPressAndContinuous({MappedInputManager::Button::Up}, [&] {
    selectedRow = ButtonNavigator::previousIndex(selectedRow, NUM_ROWS);
    const int maxCol = getRowLength(selectedRow) - 1;
    if (selectedCol > maxCol) selectedCol = maxCol;
    lastWasDoubleTappable = false;
    changed = true;
  });

  nav.onPressAndContinuous({MappedInputManager::Button::Down}, [&] {
    selectedRow = ButtonNavigator::nextIndex(selectedRow, NUM_ROWS);
    const int maxCol = getRowLength(selectedRow) - 1;
    if (selectedCol > maxCol) selectedCol = maxCol;
    lastWasDoubleTappable = false;
    changed = true;
  });

  nav.onPressAndContinuous({MappedInputManager::Button::Left}, [&] {
    const int maxCol = getRowLength(selectedRow) - 1;
    if (selectedRow == SPECIAL_ROW) {
      if (selectedCol >= SHIFT_COL && selectedCol < SPACE_COL)
        selectedCol = maxCol;
      else if (selectedCol >= SPACE_COL && selectedCol < BACKSPACE_COL)
        selectedCol = SHIFT_COL;
      else if (selectedCol >= BACKSPACE_COL && selectedCol < DONE_COL)
        selectedCol = SPACE_COL;
      else if (selectedCol >= DONE_COL)
        selectedCol = BACKSPACE_COL;
    } else {
      selectedCol = ButtonNavigator::previousIndex(selectedCol, maxCol + 1);
    }
    lastWasDoubleTappable = false;
    changed = true;
  });

  nav.onPressAndContinuous({MappedInputManager::Button::Right}, [&] {
    const int maxCol = getRowLength(selectedRow) - 1;
    if (selectedRow == SPECIAL_ROW) {
      if (selectedCol >= SHIFT_COL && selectedCol < SPACE_COL)
        selectedCol = SPACE_COL;
      else if (selectedCol >= SPACE_COL && selectedCol < BACKSPACE_COL)
        selectedCol = BACKSPACE_COL;
      else if (selectedCol >= BACKSPACE_COL && selectedCol < DONE_COL)
        selectedCol = DONE_COL;
      else if (selectedCol >= DONE_COL)
        selectedCol = SHIFT_COL;
    } else {
      selectedCol = ButtonNavigator::nextIndex(selectedCol, maxCol + 1);
    }
    lastWasDoubleTappable = false;
    changed = true;
  });

  return changed;
}

// ─── Confirm / double-tap ───────────────────────────────────────
KeyboardWidget::KeyResult KeyboardWidget::handleConfirm(const size_t textLength) {
  const unsigned long now = millis();

  // Double-tap check: same key, within window, on regular char row, text non-empty
  if (lastWasDoubleTappable && selectedRow == lastConfirmRow && selectedCol == lastConfirmCol &&
      (now - lastConfirmTime) < DOUBLE_TAP_MS && selectedRow != SPECIAL_ROW && textLength > 0) {
    // Get the OTHER variant
    const char* const* altLayout = shiftState ? keyboard : keyboardShift;
    const char altChar = altLayout[selectedRow][selectedCol];
    lastWasDoubleTappable = false;  // prevent triple-tap
    // Caller must: pop last char, then push altChar
    return {KeyAction::Char, altChar, true};
  }

  // Normal press
  if (selectedRow == SPECIAL_ROW) {
    lastWasDoubleTappable = false;

    if (selectedCol >= SHIFT_COL && selectedCol < SPACE_COL) {
      shiftState = (shiftState + 1) % 3;
      return {KeyAction::ShiftToggle, '\0'};
    }
    if (selectedCol >= SPACE_COL && selectedCol < BACKSPACE_COL) {
      return {KeyAction::Space, ' '};
    }
    if (selectedCol >= BACKSPACE_COL && selectedCol < DONE_COL) {
      return {KeyAction::Backspace, '\0'};
    }
    if (selectedCol >= DONE_COL) {
      return {KeyAction::Done, '\0'};
    }
    return {KeyAction::None, '\0'};
  }

  // Regular character
  const char c = getSelectedChar();
  if (c == '\0') return {KeyAction::None, '\0'};

  // Auto-disable single-shift after typing
  if (shiftState == 1) shiftState = 0;

  lastWasDoubleTappable = true;
  lastConfirmTime = now;
  lastConfirmRow = selectedRow;
  lastConfirmCol = selectedCol;

  return {KeyAction::Char, c};
}

// ─── Rendering ──────────────────────────────────────────────────
void KeyboardWidget::render(const GfxRenderer& renderer, const int topY) const {
  const int pageWidth = renderer.getScreenWidth();
  constexpr int maxRowWidth = KEYS_PER_ROW * (KEY_WIDTH + KEY_SPACING);
  const int leftMargin = (pageWidth - maxRowWidth) / 2;

  // Draw double-tap hint centered above keyboard
  renderer.drawCenteredText(SMALL_FONT_ID, topY, I18N.get(StrId::STR_KBD_DOUBLE_TAP_HINT));
  const int keysTopY = topY + renderer.getLineHeight(SMALL_FONT_ID) + 4;

  for (int row = 0; row < NUM_ROWS; row++) {
    const int rowY = keysTopY + row * (KEY_HEIGHT + KEY_SPACING);
    const int startX = leftMargin;

    if (row == SPECIAL_ROW) {
      int currentX = startX;

      // SHIFT (2 cols)
      const bool shiftSel = (selectedRow == SPECIAL_ROW && selectedCol >= SHIFT_COL && selectedCol < SPACE_COL);
      static constexpr StrId shiftIds[3] = {StrId::STR_KBD_SHIFT, StrId::STR_KBD_SHIFT_CAPS, StrId::STR_KBD_LOCK};
      const int shiftW = 2 * (KEY_WIDTH + KEY_SPACING) - KEY_SPACING;
      renderSpecialKey(renderer, currentX, rowY, shiftW, KEY_HEIGHT, I18N.get(shiftIds[shiftState]), shiftSel);
      currentX += 2 * (KEY_WIDTH + KEY_SPACING);

      // SPACE (5 cols)
      const bool spaceSel = (selectedRow == SPECIAL_ROW && selectedCol >= SPACE_COL && selectedCol < BACKSPACE_COL);
      const int spaceW = 5 * (KEY_WIDTH + KEY_SPACING) - KEY_SPACING;
      renderSpecialKey(renderer, currentX, rowY, spaceW, KEY_HEIGHT, " ", spaceSel);
      currentX += 5 * (KEY_WIDTH + KEY_SPACING);

      // BACKSPACE (2 cols)
      const bool bsSel = (selectedRow == SPECIAL_ROW && selectedCol >= BACKSPACE_COL && selectedCol < DONE_COL);
      const int bsW = 2 * (KEY_WIDTH + KEY_SPACING) - KEY_SPACING;
      renderSpecialKey(renderer, currentX, rowY, bsW, KEY_HEIGHT, "<-", bsSel);
      currentX += 2 * (KEY_WIDTH + KEY_SPACING);

      // OK (2 cols)
      const bool okSel = (selectedRow == SPECIAL_ROW && selectedCol >= DONE_COL);
      const int okW = 2 * (KEY_WIDTH + KEY_SPACING) - KEY_SPACING;
      renderSpecialKey(renderer, currentX, rowY, okW, KEY_HEIGHT, I18N.get(StrId::STR_OK_BUTTON), okSel);
    } else {
      for (int col = 0; col < getRowLength(row); col++) {
        const char mainChar = keyboard[row][col];
        const char shiftChar = keyboardShift[row][col];
        const std::string mainLabel(1, mainChar);
        const std::string shiftLabel(1, shiftChar);

        const int keyX = startX + col * (KEY_WIDTH + KEY_SPACING);
        const bool isSelected = row == selectedRow && col == selectedCol;
        renderKey(renderer, keyX, rowY, KEY_WIDTH, KEY_HEIGHT, mainLabel.c_str(), shiftLabel.c_str(), isSelected);
      }
    }
  }
}

void KeyboardWidget::renderKey(const GfxRenderer& renderer, const int x, const int y, const int w, const int h,
                               const char* mainLabel, const char* shiftLabel, const bool isSelected) const {
  if (isSelected) {
    renderer.fillRect(x, y, w, h, true);
    const int shiftW = renderer.getTextWidth(SMALL_FONT_ID, shiftLabel);
    renderer.drawText(SMALL_FONT_ID, x + w - shiftW - 3, y + 2, shiftLabel, false);
    const int mainW = renderer.getTextWidth(UI_10_FONT_ID, mainLabel);
    const int mainX = x + (w - mainW) / 2;
    const int mainY = y + h - renderer.getLineHeight(UI_10_FONT_ID) - 2;
    renderer.drawText(UI_10_FONT_ID, mainX, mainY, mainLabel, false);
  } else {
    renderer.drawRect(x, y, w, h);
    const int shiftW = renderer.getTextWidth(SMALL_FONT_ID, shiftLabel);
    renderer.drawText(SMALL_FONT_ID, x + w - shiftW - 3, y + 2, shiftLabel);
    const int mainW = renderer.getTextWidth(UI_10_FONT_ID, mainLabel);
    const int mainX = x + (w - mainW) / 2;
    const int mainY = y + h - renderer.getLineHeight(UI_10_FONT_ID) - 2;
    renderer.drawText(UI_10_FONT_ID, mainX, mainY, mainLabel);
  }
}

void KeyboardWidget::renderSpecialKey(const GfxRenderer& renderer, const int x, const int y, const int w, const int h,
                                      const char* label, const bool isSelected) const {
  if (isSelected) {
    renderer.fillRect(x, y, w, h, true);
    const int textW = renderer.getTextWidth(UI_10_FONT_ID, label);
    const int textX = x + (w - textW) / 2;
    const int textY = y + (h - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
    renderer.drawText(UI_10_FONT_ID, textX, textY, label, false);
  } else {
    renderer.drawRect(x, y, w, h);
    const int textW = renderer.getTextWidth(UI_10_FONT_ID, label);
    const int textX = x + (w - textW) / 2;
    const int textY = y + (h - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
    renderer.drawText(UI_10_FONT_ID, textX, textY, label);
  }
}
