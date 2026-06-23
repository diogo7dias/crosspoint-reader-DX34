#include <HalGPIO.h>
#include <SPI.h>

void HalGPIO::begin() {
  inputMgr.begin();
  SPI.begin(EPD_SCLK, SPI_MISO, EPD_MOSI, EPD_CS);
  pinMode(UART0_RXD, INPUT);
}

void HalGPIO::update() { inputMgr.update(); }

bool HalGPIO::isPressed(uint8_t buttonIndex) const { return inputMgr.isPressed(buttonIndex); }

bool HalGPIO::isAnyPressed() const {
  // The SDK's InputManager exposes per-button isPressed() plus edge events, but
  // no single "is any button currently held" query, so OR isPressed() across
  // every button. This reproduces the level-triggered any-button-held check
  // main.cpp uses to keep the CPU at full clock while a button is depressed.
  // isPressed() returns false during input suppression, so this stays
  // suppression-aware too.
  for (uint8_t i = BTN_BACK; i <= BTN_POWER; ++i) {
    if (inputMgr.isPressed(i)) return true;
  }
  return false;
}

bool HalGPIO::wasPressed(uint8_t buttonIndex) const { return inputMgr.wasPressed(buttonIndex); }

bool HalGPIO::wasAnyPressed() const { return inputMgr.wasAnyPressed(); }

bool HalGPIO::wasReleased(uint8_t buttonIndex) const { return inputMgr.wasReleased(buttonIndex); }

bool HalGPIO::wasAnyReleased() const { return inputMgr.wasAnyReleased(); }

unsigned long HalGPIO::getHeldTime() const { return inputMgr.getHeldTime(); }

void HalGPIO::suppressUntilAllReleased() { inputMgr.suppressUntilAllReleased(); }

bool HalGPIO::isUsbConnected() const {
  // U0RXD/GPIO20 reads HIGH when USB is connected
  return digitalRead(UART0_RXD) == HIGH;
}

HalGPIO::WakeupReason HalGPIO::getWakeupReason() const {
  const bool usbConnected = isUsbConnected();
  const auto wakeupCause = esp_sleep_get_wakeup_cause();
  const auto resetReason = esp_reset_reason();

  // If we woke up from deep sleep due to a GPIO trigger, it's the power button.
  // Also include the cold-boot case (!usbConnected && POWERON) if we want to treat it as a button press.
  if ((wakeupCause == ESP_SLEEP_WAKEUP_GPIO && resetReason == ESP_RST_DEEPSLEEP) ||
      (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && !usbConnected)) {
    return WakeupReason::PowerButton;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_UNKNOWN && usbConnected) {
    return WakeupReason::AfterFlash;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && usbConnected) {
    return WakeupReason::AfterUSBPower;
  }
  return WakeupReason::Other;
}