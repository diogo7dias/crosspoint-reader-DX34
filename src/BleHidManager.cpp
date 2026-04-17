#include "BleHidManager.h"

#include <Logging.h>
#include <NimBLEDevice.h>

#include "CrossPointSettings.h"

// Forward declaration for cleanup in deinit()
class BleHidScanCallbacks;
static BleHidScanCallbacks* scanCallbacksInstance = nullptr;

// HID Service and Characteristic UUIDs
static const NimBLEUUID kHidServiceUuid((uint16_t)0x1812);
static const NimBLEUUID kHidReportCharUuid((uint16_t)0x2A4D);
static const NimBLEUUID kHidBootKbInputCharUuid((uint16_t)0x2A22);
static const NimBLEUUID kHidProtocolModeCharUuid((uint16_t)0x2A4E);

BleHidManager& BleHidManager::getInstance() {
  static BleHidManager instance;
  return instance;
}

// --- Lifecycle ---

bool BleHidManager::init() {
  if (state != State::Uninitialized) return true;

  LOG_INF("BLE", "Initializing NimBLE stack");
  NimBLEDevice::init("CrossPoint");
  NimBLEDevice::setPower(ESP_PWR_LVL_P3);  // +3 dBm

  state = State::Idle;
  scanComplete = false;
  LOG_INF("BLE", "BLE initialized, free heap: %u", (unsigned)esp_get_free_heap_size());
  return true;
}

void BleHidManager::deinit() {
  if (state == State::Uninitialized) return;

  disconnect();
  NimBLEDevice::deinit(true);
  state = State::Uninitialized;
  scanResults.clear();
  client = nullptr;  // deinit frees all clients

  // Free scan callbacks singleton allocated in startScan()
  delete scanCallbacksInstance;
  scanCallbacksInstance = nullptr;

  LOG_INF("BLE", "BLE deinitialized, free heap: %u", (unsigned)esp_get_free_heap_size());
}

// --- Scanning ---

class BleHidScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
 public:
  explicit BleHidScanCallbacks(BleHidManager& mgr) : mgr(mgr) {}

  void onResult(NimBLEAdvertisedDevice* device) override {
    if (!device->isAdvertisingService(kHidServiceUuid)) return;

    auto& results = mgr.scanResults;
    if (results.size() >= 10) return;

    const std::string addr = device->getAddress().toString();
    for (const auto& d : results) {
      if (d.address == addr) return;
    }

    BleHidManager::ScannedDevice sd;
    sd.address = addr;
    sd.name = device->getName();
    if (sd.name.empty()) sd.name = addr;
    sd.rssi = device->getRSSI();
    results.push_back(sd);
    LOG_DBG("BLE", "Found HID device: %s (%s) RSSI=%d", sd.name.c_str(), addr.c_str(), sd.rssi);
  }

 private:
  BleHidManager& mgr;
};

// NimBLE 1.x signals scan completion via a plain C callback passed to
// NimBLEScan::start(), not via a method on the advertised-device callback
// class. Route the signal back to the singleton so isScanComplete() flips.
static void scanCompleteCallback(NimBLEScanResults results) {
  BleHidManager::getInstance().markScanComplete((int)results.getCount());
}

void BleHidManager::startScan(uint32_t durationSec) {
  if (state == State::Uninitialized) return;

  scanResults.clear();
  scanComplete = false;
  state = State::Scanning;

  NimBLEScan* scan = NimBLEDevice::getScan();
  if (!scanCallbacksInstance) {
    scanCallbacksInstance = new BleHidScanCallbacks(*this);
  }
  scan->setAdvertisedDeviceCallbacks(scanCallbacksInstance, /*wantDuplicates=*/false);
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
  scan->clearResults();
  // Non-blocking: passing scanCompleteCallback returns immediately and
  // invokes the callback when the scan duration elapses.
  scan->start(durationSec, scanCompleteCallback, false);
  LOG_INF("BLE", "Scan started for %u seconds", durationSec);
}

void BleHidManager::markScanComplete(int foundCount) {
  LOG_DBG("BLE", "Scan complete, %d HID devices found", (int)scanResults.size());
  (void)foundCount;
  scanComplete = true;
}

void BleHidManager::stopScan() {
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->stop();
  scanComplete = true;
  if (state == State::Scanning) state = State::Idle;
}

// --- Connection ---

class BleHidClientCallbacks : public NimBLEClientCallbacks {
 public:
  void onDisconnect(NimBLEClient*) override {
    LOG_INF("BLE", "BLE device disconnected");
    // State transition handled in updateButtonState() via isConnected() check
  }
};

static BleHidClientCallbacks clientCallbacks;

// Background FreeRTOS task for non-blocking connect
void BleHidManager::connectTask(void* param) {
  auto* mgr = static_cast<BleHidManager*>(param);
  const std::string addr = mgr->pendingConnectAddr;

  bool ok = mgr->connectToDeviceBlocking(addr);
  if (!ok) {
    mgr->state = State::Idle;
  }

  vTaskDelete(nullptr);  // Self-delete
}

void BleHidManager::connectToDeviceAsync(const std::string& address) {
  if (state == State::Uninitialized) return;
  if (state == State::Scanning) stopScan();

  state = State::Connecting;
  pendingConnectAddr = address;

  // Launch blocking connect on a background task (4KB stack is enough)
  xTaskCreate(connectTask, "ble_conn", 4096, this, 1, nullptr);
}

bool BleHidManager::connectToDeviceBlocking(const std::string& address) {
  LOG_INF("BLE", "Connecting to %s", address.c_str());

  // Reuse existing client or create new one (fix #4: client leak)
  if (!client) {
    client = NimBLEDevice::createClient();
    // false = don't let NimBLE delete the static callbacks instance.
    client->setClientCallbacks(&clientCallbacks, /*deleteCallbacks=*/false);
  } else if (client->isConnected()) {
    client->disconnect();
  }

  client->setConnectTimeout(kConnectTimeoutSec);

  NimBLEAddress addr(address, 0);  // 0 = public address type
  if (!client->connect(addr)) {
    LOG_ERR("BLE", "Connection failed to %s", address.c_str());
    return false;
  }

  LOG_INF("BLE", "Connected to %s", address.c_str());

  if (!subscribeToHid()) {
    LOG_ERR("BLE", "Failed to subscribe to HID reports");
    client->disconnect();
    return false;
  }

  // Resolve connected device name from scan results
  connectedName = address;
  for (const auto& sd : scanResults) {
    if (sd.address == address) {
      connectedName = sd.name;
      break;
    }
  }

  state = State::Connected;
  reconnectAttempts = 0;
  LOG_INF("BLE", "HID subscription active for %s", connectedName.c_str());
  return true;
}

bool BleHidManager::subscribeToHid() {
  if (!client || !client->isConnected()) return false;

  NimBLERemoteService* hidService = client->getService(kHidServiceUuid);
  if (!hidService) {
    LOG_ERR("BLE", "HID service not found");
    return false;
  }

  // Try Boot Keyboard Input first (works for most keyboards/remotes)
  NimBLERemoteCharacteristic* bootKbChar = hidService->getCharacteristic(kHidBootKbInputCharUuid);
  if (bootKbChar && bootKbChar->canNotify()) {
    NimBLERemoteCharacteristic* protoMode = hidService->getCharacteristic(kHidProtocolModeCharUuid);
    if (protoMode && protoMode->canWrite()) {
      uint8_t bootMode = 0;  // 0 = Boot Protocol
      protoMode->writeValue(&bootMode, 1);
    }

    bootKbChar->subscribe(
        true, [this](NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) { onHidReport(data, len); });
    LOG_INF("BLE", "Subscribed to Boot Keyboard Input");
    return true;
  }

  // Fall back to HID Report characteristics (for gamepads and non-boot devices).
  // NimBLE 1.x returns a pointer-to-vector (nullable), unlike the const-ref in 2.x.
  bool subscribed = false;
  const auto* chars = hidService->getCharacteristics(true);
  if (chars) {
    for (auto* chr : *chars) {
      if (chr->getUUID() == kHidReportCharUuid && chr->canNotify()) {
        chr->subscribe(
            true, [this](NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) { onHidReport(data, len); });
        subscribed = true;
        LOG_INF("BLE", "Subscribed to HID Report characteristic");
      }
    }
  }

  return subscribed;
}

void BleHidManager::disconnect() {
  if (client && client->isConnected()) {
    client->disconnect();
  }
  state = (state == State::Uninitialized) ? State::Uninitialized : State::Idle;
  connectedName.clear();

  portENTER_CRITICAL(&stateLock);
  for (int i = 0; i < kButtonCount; i++) {
    currentPressed[i] = false;
    previousPressed[i] = false;
    edgePressed[i] = false;
    edgeReleased[i] = false;
  }
  portEXIT_CRITICAL(&stateLock);
}

// --- Auto-reconnect ---

void BleHidManager::tryAutoReconnect() {
  if (SETTINGS.bleDeviceAddr[0] == '\0' || !SETTINGS.bleEnabled) return;

  // Try up to kMaxReconnectAttempts at boot (blocking OK here).
  for (uint8_t attempt = 1; attempt <= kMaxReconnectAttempts; attempt++) {
    LOG_INF("BLE", "Auto-reconnect attempt %u/%u to %s", attempt, kMaxReconnectAttempts, SETTINGS.bleDeviceName);
    if (connectToDeviceBlocking(SETTINGS.bleDeviceAddr)) {
      connectedName = SETTINGS.bleDeviceName;
      reconnectAttempts = 0;
      return;
    }
    if (attempt < kMaxReconnectAttempts) delay(1000);
  }

  // All attempts failed — shut down BLE to save power.
  LOG_INF("BLE", "Boot reconnect failed after %u attempts, shutting down BLE", kMaxReconnectAttempts);
  deinit();
}

// --- HID Report Parsing ---

void BleHidManager::onHidReport(const uint8_t* data, size_t len) {
  if (len == 0) return;

  // Boot keyboard protocol: 8 bytes
  // [0] = modifier keys, [1] = reserved, [2..7] = up to 6 keycodes
  if (len >= 3 && len <= 8) {
    uint8_t modifiers = data[0];

    // Collect all active keycodes from this report
    uint16_t activeKeycodes[6] = {};
    int activeCount = 0;
    for (size_t i = 2; i < len; i++) {
      if (data[i] == 0) continue;
      uint16_t fullCode = data[i];
      if (modifiers) {
        fullCode = (uint16_t)((modifiers << 8) | data[i]);
      }
      activeKeycodes[activeCount++] = fullCode;
      lastRawKeycode = fullCode;
    }

    // Fix #6: In capture mode, don't translate — just store raw keycode
    if (captureMode) return;

    // Fix #7: Per-key release — compute which buttons SHOULD be pressed,
    // then set all others to false.
    portENTER_CRITICAL(&stateLock);
    bool shouldBePressed[kButtonCount] = {};
    for (int k = 0; k < activeCount; k++) {
      for (int b = 0; b < kButtonCount; b++) {
        if (SETTINGS.bleKeyMap[b] == activeKeycodes[k] && activeKeycodes[k] != 0) {
          shouldBePressed[b] = true;
        }
      }
    }
    for (int b = 0; b < kButtonCount; b++) {
      currentPressed[b] = shouldBePressed[b];
    }
    portEXIT_CRITICAL(&stateLock);
    return;
  }

  // Generic HID report (gamepads, custom remotes)
  if (len >= 1) {
    uint16_t rawCode = data[0];
    if (len >= 2) {
      rawCode = (uint16_t)((data[1] << 8) | data[0]);
    }

    if (rawCode != 0) {
      lastRawKeycode = rawCode;
    }

    if (captureMode) return;

    portENTER_CRITICAL(&stateLock);
    if (rawCode != 0) {
      // Set the matching button, clear others
      bool shouldBePressed[kButtonCount] = {};
      for (int b = 0; b < kButtonCount; b++) {
        if (SETTINGS.bleKeyMap[b] == rawCode && rawCode != 0) {
          shouldBePressed[b] = true;
        }
      }
      for (int b = 0; b < kButtonCount; b++) {
        currentPressed[b] = shouldBePressed[b];
      }
    } else {
      for (int b = 0; b < kButtonCount; b++) {
        currentPressed[b] = false;
      }
    }
    portEXIT_CRITICAL(&stateLock);
  }
}

void BleHidManager::translateKeycode(uint16_t keycode, bool pressed) {
  for (int i = 0; i < kButtonCount; i++) {
    if (SETTINGS.bleKeyMap[i] == keycode && keycode != 0) {
      currentPressed[i] = pressed;
      return;
    }
  }
}

// --- Button State Polling ---

void BleHidManager::updateButtonState() {
  if (state == State::Uninitialized) return;

  // Check for disconnection
  if (state == State::Connected && client && !client->isConnected()) {
    state = State::Disconnected;
    reconnectAttempts = 0;
    LOG_INF("BLE", "Detected BLE disconnection");
  }

  // Non-blocking auto-reconnect with retry limit
  if (state == State::Disconnected && SETTINGS.bleEnabled) {
    if (reconnectAttempts >= kMaxReconnectAttempts) {
      LOG_INF("BLE", "Reconnect failed after %u attempts, shutting down BLE", kMaxReconnectAttempts);
      deinit();
      return;
    }
    unsigned long now = millis();
    if (now - lastReconnectAttempt >= kReconnectIntervalMs) {
      lastReconnectAttempt = now;
      reconnectAttempts++;
      LOG_DBG("BLE", "Auto-reconnect attempt %u/%u", reconnectAttempts, kMaxReconnectAttempts);
      connectToDeviceAsync(SETTINGS.bleDeviceAddr);
    }
  }

  // Edge detection (under spinlock for consistent snapshot)
  portENTER_CRITICAL(&stateLock);
  for (int i = 0; i < kButtonCount; i++) {
    bool curr = currentPressed[i];
    bool prev = previousPressed[i];
    edgePressed[i] = (curr && !prev);
    edgeReleased[i] = (!curr && prev);
    previousPressed[i] = curr;
  }
  portEXIT_CRITICAL(&stateLock);
}

bool BleHidManager::wasPressed(uint8_t idx) const { return idx < kButtonCount && edgePressed[idx]; }

bool BleHidManager::wasReleased(uint8_t idx) const { return idx < kButtonCount && edgeReleased[idx]; }

bool BleHidManager::isPressed(uint8_t idx) const { return idx < kButtonCount && currentPressed[idx]; }

bool BleHidManager::wasAnyPressed() const {
  for (int i = 0; i < kButtonCount; i++) {
    if (edgePressed[i]) return true;
  }
  return false;
}

// --- Capture Mode ---

void BleHidManager::setCaptureMode(bool enabled) {
  captureMode = enabled;
  if (enabled) {
    lastRawKeycode = 0;
    // Clear button state so stale presses don't leak
    portENTER_CRITICAL(&stateLock);
    for (int i = 0; i < kButtonCount; i++) {
      currentPressed[i] = false;
    }
    portEXIT_CRITICAL(&stateLock);
  }
}

uint16_t BleHidManager::captureRawKeycode() {
  uint16_t code = lastRawKeycode;
  lastRawKeycode = 0;
  return code;
}

void BleHidManager::clearCapturedKeycode() { lastRawKeycode = 0; }
