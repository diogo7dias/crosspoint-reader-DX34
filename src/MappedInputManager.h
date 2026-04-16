#pragma once

#include <HalGPIO.h>

class MappedInputManager {
 public:
  enum class Button { Back, Confirm, Left, Right, Up, Down, Power, PageBack, PageForward };

  struct Labels {
    const char* btn1;
    const char* btn2;
    const char* btn3;
    const char* btn4;
  };

  explicit MappedInputManager(HalGPIO& gpio) : gpio(gpio) {}

  void update() const { gpio.update(); }
  // Suppress all press/release events until every physical button is released.
  // Use during activity transitions to prevent the button event that triggered
  // the transition from leaking into the next screen.
  void suppressUntilAllReleased() { gpio.suppressUntilAllReleased(); }
  bool wasPressed(Button button) const;
  bool wasReleased(Button button) const;
  bool isPressed(Button button) const;
  bool wasAnyPressed() const;
  bool wasAnyReleased() const;
  unsigned long getHeldTime() const;
  Labels mapLabels(const char* back, const char* confirm, const char* previous, const char* next) const;
  // Returns the raw front button index that was pressed this frame (or -1 if none).
  int getPressedFrontButton() const;

 private:
  HalGPIO& gpio;

  // Zone-based dynamic button mapping.
  // When fewer than 4 front buttons are active (have labels), physical positions
  // are divided into equal zones mapped to the active buttons in display order.
  // Each entry holds the hardware button index whose action should fire when that
  // physical position is pressed.  Default: identity (1:1 mapping).
  static constexpr uint8_t kFrontButtons[4] = {HalGPIO::BTN_BACK, HalGPIO::BTN_CONFIRM,
                                                HalGPIO::BTN_LEFT, HalGPIO::BTN_RIGHT};
  mutable uint8_t zoneOwner[4] = {HalGPIO::BTN_BACK, HalGPIO::BTN_CONFIRM,
                                  HalGPIO::BTN_LEFT, HalGPIO::BTN_RIGHT};

  bool mapButton(Button button, bool (HalGPIO::*fn)(uint8_t) const) const;
  // Check the target hardware button plus any zone-mapped buttons that delegate to it.
  bool checkWithZones(uint8_t targetHw, bool (HalGPIO::*fn)(uint8_t) const) const;
};
