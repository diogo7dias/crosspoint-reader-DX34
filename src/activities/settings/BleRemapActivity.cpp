#include "BleRemapActivity.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>

#include "BleHidManager.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr unsigned long kErrorDisplayMs = 1500;
}

void BleRemapActivity::onEnter() {
  Activity::onEnter();
  currentStep = 0;
  for (int i = 0; i < kRoleCount; i++) tempMapping[i] = 0;
  errorMessage.clear();
  errorUntil = 0;

  // Enable capture mode so BLE keycodes are NOT translated into button
  // presses (which would leak into MappedInputManager side-button checks).
  BLE_HID.setCaptureMode(true);
  requestUpdate();
}

void BleRemapActivity::onExit() {
  BLE_HID.setCaptureMode(false);
  Activity::onExit();
}

void BleRemapActivity::loop() {
  // Clear error banner after timeout
  if (errorUntil > 0 && millis() > errorUntil) {
    errorMessage.clear();
    errorUntil = 0;
    requestUpdate();
    return;
  }

  // Use MappedInputManager for side buttons — safe because capture mode
  // prevents BLE keypresses from reaching button translation.
  // Side Up: reset to defaults and exit
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    for (int i = 0; i < CrossPointSettings::BLE_KEY_MAP_SIZE; i++) {
      SETTINGS.bleKeyMap[i] = 0;
    }
    SETTINGS.saveToFile();
    onBack();
    return;
  }

  // Side Down: cancel without saving
  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    onBack();
    return;
  }

  // Wait for BLE keypress
  uint16_t rawCode = BLE_HID.captureRawKeycode();
  if (rawCode == 0) return;

  // Validate not already assigned to another role
  if (!validateUnassigned(rawCode)) {
    requestUpdate();
    return;
  }

  tempMapping[currentStep] = rawCode;
  currentStep++;

  if (currentStep >= kRoleCount) {
    applyMapping();
    SETTINGS.saveToFile();
    onBack();
    return;
  }

  BLE_HID.clearCapturedKeycode();
  requestUpdate();
}

void BleRemapActivity::render(Activity::RenderLock&&) {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();

  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_BLE_REMAP_TITLE), true, EpdFontFamily::REGULAR);
  renderer.drawCenteredText(UI_10_FONT_ID, 40, tr(STR_BLE_REMAP_PROMPT));

  for (uint8_t i = 0; i < kRoleCount; i++) {
    const int y = 70 + i * 28;
    const bool isSelected = (i == currentStep);

    if (isSelected) {
      renderer.fillRect(0, y - 2, pageWidth - 1, 28);
    }

    renderer.drawText(UI_10_FONT_ID, 20, y, getRoleName(i), !isSelected);

    const char* assigned = (tempMapping[i] == 0) ? tr(STR_BLE_KEY_UNASSIGNED) : getKeycodeName(tempMapping[i]);
    const auto width = renderer.getTextWidth(UI_10_FONT_ID, assigned);
    renderer.drawText(UI_10_FONT_ID, pageWidth - 20 - width, y, assigned, !isSelected);
  }

  if (!errorMessage.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, 250, errorMessage.c_str(), true);
  }

  renderer.drawCenteredText(SMALL_FONT_ID, 270, tr(STR_BLE_REMAP_RESET_HINT), true);
  renderer.drawCenteredText(SMALL_FONT_ID, 290, tr(STR_BLE_REMAP_CANCEL_HINT), true);

  renderer.displayBuffer();
}

void BleRemapActivity::applyMapping() {
  for (int i = 0; i < kRoleCount; i++) {
    SETTINGS.bleKeyMap[i] = tempMapping[i];
  }
}

bool BleRemapActivity::validateUnassigned(uint16_t keycode) {
  for (uint8_t i = 0; i < kRoleCount; i++) {
    if (tempMapping[i] == keycode && i != currentStep && tempMapping[i] != 0) {
      errorMessage = tr(STR_ALREADY_ASSIGNED);
      errorUntil = millis() + kErrorDisplayMs;
      return false;
    }
  }
  return true;
}

const char* BleRemapActivity::getRoleName(uint8_t step) const {
  switch (step) {
    case 0: return tr(STR_BACK);
    case 1: return tr(STR_CONFIRM);
    case 2: return tr(STR_DIR_LEFT);
    case 3: return tr(STR_DIR_RIGHT);
    case 4: return tr(STR_DIR_UP);
    case 5: return tr(STR_DIR_DOWN);
    default: return "?";
  }
}

const char* BleRemapActivity::getKeycodeName(uint16_t keycode) {
  const uint8_t key = keycode & 0xFF;
  const uint8_t mod = (keycode >> 8) & 0xFF;

  if (key == 0 && mod != 0) {
    if (mod & 0x01) return "L-Ctrl";
    if (mod & 0x02) return "L-Shift";
    if (mod & 0x04) return "L-Alt";
    if (mod & 0x08) return "L-GUI";
    if (mod & 0x10) return "R-Ctrl";
    if (mod & 0x20) return "R-Shift";
    if (mod & 0x40) return "R-Alt";
    if (mod & 0x80) return "R-GUI";
  }

  if (mod == 0) {
    switch (key) {
      case 0x04: return "A";  case 0x05: return "B";  case 0x06: return "C";
      case 0x07: return "D";  case 0x08: return "E";  case 0x09: return "F";
      case 0x0A: return "G";  case 0x0B: return "H";  case 0x0C: return "I";
      case 0x0D: return "J";  case 0x0E: return "K";  case 0x0F: return "L";
      case 0x10: return "M";  case 0x11: return "N";  case 0x12: return "O";
      case 0x13: return "P";  case 0x14: return "Q";  case 0x15: return "R";
      case 0x16: return "S";  case 0x17: return "T";  case 0x18: return "U";
      case 0x19: return "V";  case 0x1A: return "W";  case 0x1B: return "X";
      case 0x1C: return "Y";  case 0x1D: return "Z";
      case 0x1E: return "1";  case 0x1F: return "2";  case 0x20: return "3";
      case 0x21: return "4";  case 0x22: return "5";  case 0x23: return "6";
      case 0x24: return "7";  case 0x25: return "8";  case 0x26: return "9";
      case 0x27: return "0";
      case 0x28: return "Enter";
      case 0x29: return "Escape";
      case 0x2A: return "Backspace";
      case 0x2B: return "Tab";
      case 0x2C: return "Space";
      case 0x39: return "CapsLock";
      case 0x4F: return "Right";
      case 0x50: return "Left";
      case 0x51: return "Down";
      case 0x52: return "Up";
      case 0x4B: return "PageUp";
      case 0x4E: return "PageDown";
      case 0x4A: return "Home";
      case 0x4D: return "End";
      case 0xE9: return "Vol+";
      case 0xEA: return "Vol-";
      case 0xE2: return "Mute";
      case 0xB5: return "Next";
      case 0xB6: return "Prev";
      case 0xCD: return "Play";
    }
  }

  static char hexBuf[12];
  if (mod != 0) {
    snprintf(hexBuf, sizeof(hexBuf), "0x%02X+0x%02X", mod, key);
  } else {
    snprintf(hexBuf, sizeof(hexBuf), "0x%02X", key);
  }
  return hexBuf;
}
