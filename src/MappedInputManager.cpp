#include "MappedInputManager.h"

#include <algorithm>

#include "BleHidManager.h"
#include "CrossPointSettings.h"

namespace {
using ButtonIndex = uint8_t;

struct SideLayoutMap {
  ButtonIndex pageBack;
  ButtonIndex pageForward;
};

// Order matches CrossPointSettings::SIDE_BUTTON_LAYOUT.
constexpr SideLayoutMap kSideLayouts[] = {
    {HalGPIO::BTN_UP, HalGPIO::BTN_DOWN},
    {HalGPIO::BTN_DOWN, HalGPIO::BTN_UP},
};
}  // namespace

constexpr uint8_t MappedInputManager::kFrontButtons[4];

void MappedInputManager::update() const {
  gpio.update();
  BLE_HID.updateButtonState();
}

namespace {
// Map MappedInputManager::Button to BleHidManager button index.
// Returns -1 for buttons that don't map (Power, PageBack, PageForward).
int bleIndexForButton(MappedInputManager::Button button) {
  switch (button) {
    case MappedInputManager::Button::Back:        return 0;
    case MappedInputManager::Button::Confirm:     return 1;
    case MappedInputManager::Button::Left:        return 2;
    case MappedInputManager::Button::Right:       return 3;
    case MappedInputManager::Button::Up:          return 4;
    case MappedInputManager::Button::Down:        return 5;
    // PageBack/PageForward map to the same BLE buttons as Up/Down
    // so BLE users can turn pages in the reader.
    case MappedInputManager::Button::PageBack:    return 4;
    case MappedInputManager::Button::PageForward: return 5;
    default: return -1;
  }
}
}  // namespace

bool MappedInputManager::checkWithZones(const uint8_t targetHw, bool (HalGPIO::*fn)(uint8_t) const) const {
  // Only check physical positions whose zone is assigned to this action.
  // This ensures a remapped button doesn't also fire its original action.
  for (int i = 0; i < 4; i++) {
    if (zoneOwner[i] == targetHw) {
      if ((gpio.*fn)(kFrontButtons[i])) return true;
    }
  }
  return false;
}

bool MappedInputManager::mapButton(const Button button, bool (HalGPIO::*fn)(uint8_t) const) const {
  const auto sideLayout = static_cast<CrossPointSettings::SIDE_BUTTON_LAYOUT>(SETTINGS.sideButtonLayout);
  const auto& side = kSideLayouts[sideLayout];

  switch (button) {
    case Button::Back:
      return checkWithZones(SETTINGS.frontButtonBack, fn);
    case Button::Confirm:
      return checkWithZones(SETTINGS.frontButtonConfirm, fn);
    case Button::Left:
      return checkWithZones(SETTINGS.frontButtonLeft, fn);
    case Button::Right:
      return checkWithZones(SETTINGS.frontButtonRight, fn);
    case Button::Up:
      return (gpio.*fn)(HalGPIO::BTN_UP);
    case Button::Down:
      return (gpio.*fn)(HalGPIO::BTN_DOWN);
    case Button::Power:
      return (gpio.*fn)(HalGPIO::BTN_POWER);
    case Button::PageBack:
      return (gpio.*fn)(side.pageBack);
    case Button::PageForward:
      return (gpio.*fn)(side.pageForward);
  }

  return false;
}

bool MappedInputManager::wasPressed(const Button button) const {
  if (mapButton(button, &HalGPIO::wasPressed)) return true;
  if (SETTINGS.bleEnabled && BLE_HID.isInitialized()) {
    const int idx = bleIndexForButton(button);
    if (idx >= 0 && BLE_HID.wasPressed(static_cast<uint8_t>(idx))) return true;
  }
  return false;
}

bool MappedInputManager::wasReleased(const Button button) const {
  if (mapButton(button, &HalGPIO::wasReleased)) return true;
  if (SETTINGS.bleEnabled && BLE_HID.isInitialized()) {
    const int idx = bleIndexForButton(button);
    if (idx >= 0 && BLE_HID.wasReleased(static_cast<uint8_t>(idx))) return true;
  }
  return false;
}

bool MappedInputManager::isPressed(const Button button) const {
  if (mapButton(button, &HalGPIO::isPressed)) return true;
  if (SETTINGS.bleEnabled && BLE_HID.isInitialized()) {
    const int idx = bleIndexForButton(button);
    if (idx >= 0 && BLE_HID.isPressed(static_cast<uint8_t>(idx))) return true;
  }
  return false;
}

bool MappedInputManager::wasAnyPressed() const {
  if (gpio.wasAnyPressed()) return true;
  if (SETTINGS.bleEnabled && BLE_HID.isInitialized() && BLE_HID.wasAnyPressed()) return true;
  return false;
}

bool MappedInputManager::wasAnyReleased() const { return gpio.wasAnyReleased(); }

unsigned long MappedInputManager::getHeldTime() const { return gpio.getHeldTime(); }

MappedInputManager::Labels MappedInputManager::mapLabels(const char* back, const char* confirm, const char* previous,
                                                         const char* next) const {
  // Build the label order based on the configured hardware mapping.
  auto labelForHardware = [&](uint8_t hw) -> const char* {
    // Compare against configured logical roles and return the matching label.
    if (hw == SETTINGS.frontButtonBack) {
      return back;
    }
    if (hw == SETTINGS.frontButtonConfirm) {
      return confirm;
    }
    if (hw == SETTINGS.frontButtonLeft) {
      return previous;
    }
    if (hw == SETTINGS.frontButtonRight) {
      return next;
    }
    return "";
  };

  const Labels result = {labelForHardware(HalGPIO::BTN_BACK), labelForHardware(HalGPIO::BTN_CONFIRM),
                         labelForHardware(HalGPIO::BTN_LEFT), labelForHardware(HalGPIO::BTN_RIGHT)};

  // Compute zone owners for dynamic button mapping.
  // When fewer than 4 buttons have labels, physical positions are divided into
  // equal zones assigned to active buttons in display order so that pressing
  // any physical button under an expanded visual button triggers that action.
  const char* hwLabels[] = {result.btn1, result.btn2, result.btn3, result.btn4};
  int activePositions[4];
  int activeCount = 0;
  for (int i = 0; i < 4; i++) {
    if (hwLabels[i] != nullptr && hwLabels[i][0] != '\0') {
      activePositions[activeCount++] = i;
    }
  }

  if (activeCount == 0 || activeCount == 4) {
    // No active or all active: 1:1 identity mapping.
    for (int i = 0; i < 4; i++) zoneOwner[i] = kFrontButtons[i];
  } else {
    // Divide 4 physical positions into activeCount equal zones using the
    // midpoint of each position to determine which zone it falls into.
    for (int i = 0; i < 4; i++) {
      const int zone = std::min((2 * i + 1) * activeCount / 8, activeCount - 1);
      zoneOwner[i] = kFrontButtons[activePositions[zone]];
    }
  }

  return result;
}

int MappedInputManager::getPressedFrontButton() const {
  // Scan the raw front buttons in hardware order.
  // This bypasses remapping so the remap activity can capture physical presses.
  if (gpio.wasPressed(HalGPIO::BTN_BACK)) {
    return HalGPIO::BTN_BACK;
  }
  if (gpio.wasPressed(HalGPIO::BTN_CONFIRM)) {
    return HalGPIO::BTN_CONFIRM;
  }
  if (gpio.wasPressed(HalGPIO::BTN_LEFT)) {
    return HalGPIO::BTN_LEFT;
  }
  if (gpio.wasPressed(HalGPIO::BTN_RIGHT)) {
    return HalGPIO::BTN_RIGHT;
  }
  return -1;
}