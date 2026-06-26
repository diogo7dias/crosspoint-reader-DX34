#pragma once

#include <Arduino.h>
#include <InputManager.h>
#include <Logging.h>
#include <freertos/semphr.h>

#include <cassert>

#include "HalGPIO.h"

class HalPowerManager;
extern HalPowerManager powerManager;  // Singleton

class HalPowerManager {
  int normalFreq = 0;  // MHz
  bool isLowPower = false;

  enum LockMode { None, NormalSpeed };
  LockMode currentLockMode = None;
  SemaphoreHandle_t modeMutex = nullptr;  // Protect access to currentLockMode

  // EMA-smoothed battery percent, scaled x10 (0 = uninitialised)
  mutable int _batteryCachedPercent = 0;

  // X3 reads battery from the BQ27220 fuel gauge over I2C instead of the X4's
  // analog ADC. Set in begin() when an X3 is detected. The X3 read is cached
  // for BATTERY_POLL_MS so the status bar does not hammer the I2C bus.
  bool _batteryUseI2C = false;
  mutable unsigned long _batteryLastPollMs = 0;
  static constexpr unsigned long BATTERY_POLL_MS = 1500;  // ms

 public:
  // 80 MHz (was 10): at 10 MHz the first button press after the 3s idle window
  // crawled — ADC sampling + the idle-loop tick both ran at 1/24th clock, so a
  // page-turn after any reading pause felt unresponsive. 80 MHz keeps a real
  // power saving (1/3 of the 240 MHz normal freq) while restoring snappy wake.
  static constexpr int LOW_POWER_FREQ = 80;                    // MHz
  static constexpr unsigned long IDLE_POWER_SAVING_MS = 3000;  // ms

  void begin();

  // Control CPU frequency for power saving
  void setPowerSaving(bool enabled);

  // Setup wake up GPIO and enter deep sleep
  // Should be called inside main loop() to handle the currentLockMode
  void startDeepSleep(HalGPIO& gpio) const;

  // Get battery percentage (range 0-100)
  int getBatteryPercentage() const;

  // RAII helper class to manage power saving locks
  // Usage: create an instance of Lock in a scope to disable power saving, for example when running a task that needs
  // full performance. When the Lock instance is destroyed (goes out of scope), power saving will be re-enabled.
  class Lock {
    friend class HalPowerManager;
    bool valid = false;

   public:
    explicit Lock();
    ~Lock();

    // Non-copyable and non-movable
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    Lock(Lock&&) = delete;
    Lock& operator=(Lock&&) = delete;
  };
};
