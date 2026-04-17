#pragma once

#include <freertos/portmacro.h>

#include <cstdint>
#include <string>
#include <vector>

// Forward declarations — avoid pulling NimBLE headers into every TU.
class NimBLEClient;
class NimBLEAddress;
class NimBLERemoteCharacteristic;
class NimBLEAdvertisedDevice;

/**
 * Singleton managing BLE HID host lifecycle: scan, connect, pair,
 * HID report parsing, key-mapping, and auto-reconnect.
 *
 * Designed for lazy init — BLE stack is only loaded when the user
 * enters BLE settings or has a saved paired device on boot.
 */
class BleHidManager {
 public:
  enum class State : uint8_t {
    Uninitialized,  // BLE stack not loaded
    Idle,           // Stack loaded, not scanning or connected
    Scanning,       // Actively scanning for HID devices
    Connecting,     // Attempting connection (async)
    Connected,      // Receiving HID reports
    Disconnected    // Was connected, lost connection
  };

  struct ScannedDevice {
    std::string address;
    std::string name;
    int rssi = 0;
  };

  static BleHidManager& getInstance();

  // --- Lifecycle ---
  bool init();
  void deinit();
  bool isInitialized() const { return state != State::Uninitialized; }

  // --- Scanning ---
  void startScan(uint32_t durationSec = 10);
  void stopScan();
  const std::vector<ScannedDevice>& getScanResults() const { return scanResults; }
  bool isScanComplete() const { return scanComplete; }
  // Invoked by the NimBLE scan-complete C callback; internal.
  void markScanComplete(int foundCount);

  // --- Connection (non-blocking) ---
  // Launches connection on a background FreeRTOS task.
  // Poll getState() for Connecting → Connected/Idle transition.
  void connectToDeviceAsync(const std::string& address);
  void disconnect();
  State getState() const { return state; }
  const std::string& getConnectedDeviceName() const { return connectedName; }

  // --- Auto-reconnect (called from main.cpp setup, blocking OK at boot) ---
  void tryAutoReconnect();

  // --- Button state polling (called from MappedInputManager) ---
  // Index: 0=Back, 1=Confirm, 2=Left, 3=Right, 4=Up, 5=Down
  static constexpr uint8_t kButtonCount = 6;
  bool wasPressed(uint8_t idx) const;
  bool wasReleased(uint8_t idx) const;
  bool isPressed(uint8_t idx) const;
  bool wasAnyPressed() const;

  // Must be called once per main loop tick to advance edge detection.
  void updateButtonState();

  // --- Raw keycode capture (used by BleRemapActivity) ---
  // When capture mode is on, BLE keycodes are NOT translated into button
  // state — they're only stored for captureRawKeycode() to pick up.
  void setCaptureMode(bool enabled);
  bool isCaptureMode() const { return captureMode; }
  uint16_t captureRawKeycode();
  void clearCapturedKeycode();

 private:
  BleHidManager() = default;

  State state = State::Uninitialized;
  std::vector<ScannedDevice> scanResults;
  volatile bool scanComplete = false;
  friend class BleHidScanCallbacks;
  NimBLEClient* client = nullptr;
  std::string connectedName;

  // Spinlock for cross-core access to button state arrays.
  // BLE notification callback runs on NimBLE task (core 1),
  // updateButtonState() / wasPressed() run on main loop (core 0).
  mutable portMUX_TYPE stateLock = portMUX_INITIALIZER_UNLOCKED;

  // HID report callback — translates raw reports into button state.
  void onHidReport(const uint8_t* data, size_t len);

  // Raw keycode for remap capture mode.
  volatile uint16_t lastRawKeycode = 0;
  bool captureMode = false;

  // Button state arrays (protected by stateLock).
  bool currentPressed[kButtonCount] = {};
  bool previousPressed[kButtonCount] = {};
  bool edgePressed[kButtonCount] = {};
  bool edgeReleased[kButtonCount] = {};

  // Reconnect tracking
  unsigned long lastReconnectAttempt = 0;
  uint8_t reconnectAttempts = 0;
  static constexpr unsigned long kReconnectIntervalMs = 5000;
  static constexpr uint8_t kConnectTimeoutSec = 5;
  static constexpr uint8_t kMaxReconnectAttempts = 5;

  // Max scan results
  static constexpr size_t kMaxScanResults = 10;

  // Background connect task
  std::string pendingConnectAddr;
  static void connectTask(void* param);

  // Internal helpers (blocking — only called from background task or boot)
  bool connectToDeviceBlocking(const std::string& address);
  bool subscribeToHid();
  void translateKeycode(uint16_t keycode, bool pressed);
};

#define BLE_HID BleHidManager::getInstance()
