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
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);  // +9 dBm — reduce range-based drops

  // Many BLE HID peripherals (the IINE Gamebrick among them) refuse to notify
  // input reports until the link is encrypted and keys are bonded — the CCCD
  // write on subscribe returns ATT Insufficient Authentication, and without
  // bonding enabled NimBLE cannot negotiate encryption, so the peer drops the
  // link a second or two after service discovery completes. JustWorks pairing
  // (bonding=true, MITM=false, SC=true) matches what upstream Crosspoint uses
  // and unblocks these devices.
  NimBLEDevice::setSecurityAuth(/*bonding=*/true, /*mitm=*/false, /*sc=*/true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

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

  // Launch blocking connect on a background task. 6KB stack leaves headroom
  // for the NimBLE-Arduino call chain (connect → secureConnection → subscribe
  // per characteristic); 4KB was tight once explicit pairing was added.
  xTaskCreate(connectTask, "ble_conn", 6144, this, 1, nullptr);
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

  // Explicit connection params. The 6s supervision timeout (600 × 10 ms) is
  // comfortably longer than the default on most stacks, giving gamepads that
  // notify sparsely (only on button edges) enough slack to avoid link loss
  // while the user sits on the Map BLE buttons screen waiting to press a key.
  client->setConnectionParams(/*minInterval=*/12, /*maxInterval=*/24,
                              /*latency=*/0, /*timeout=*/600,
                              /*scanInterval=*/60, /*scanWindow=*/30);

  NimBLEAddress addr(address, 0);  // 0 = public address type
  if (!client->connect(addr)) {
    LOG_ERR("BLE", "Connection failed to %s", address.c_str());
    return false;
  }

  LOG_INF("BLE", "Connected to %s", address.c_str());

  // Re-request the same params after link-up so the peripheral actually
  // adopts our supervision timeout; some devices ignore the scan-phase
  // params and use their own until we issue an L2CAP update.
  client->updateConnParams(12, 24, 0, 600);
  client->setDataLen(251);

  // Force pairing/encryption now rather than waiting for NimBLE to auto-retry
  // an ATT Insufficient Authentication on the first CCCD write. Explicit is
  // more reliable: on a first connect we pair and persist an LTK; on reconnect
  // with an existing bond NimBLE reuses the key and this returns quickly.
  // We tolerate failure — some devices (non-HID or open HID) don't require it,
  // and subscribe() below still has NimBLE's implicit retry as a backstop.
  if (!client->secureConnection()) {
    LOG_INF("BLE", "secureConnection() did not complete (continuing, subscribe may still work)");
  }

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

  // Name may be empty on reconnect (bonded device, no scan this session) —
  // fall back to the persisted device name from settings so the Gamebrick
  // detector can still match by name on reboot.
  if ((connectedName == address || connectedName.empty()) && SETTINGS.bleDeviceAddr[0] != '\0' &&
      address == SETTINGS.bleDeviceAddr) {
    connectedName = SETTINGS.bleDeviceName;
  }
  detectGamebrickFromAddressOrName(address, connectedName);

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

  // Ask the peer to use Report Protocol (the descriptor-driven mode). Previous
  // revisions forced Boot Protocol (0x00) whenever a Boot Keyboard Input char
  // was present, which broke gamepads that also expose the boot chars for
  // compatibility: the boot reports only ever carry keyboard bytes, so gamepad
  // input silently stops arriving. Writing 0x01 here is a no-op for devices
  // that already default to Report Protocol.
  NimBLERemoteCharacteristic* protoMode = hidService->getCharacteristic(kHidProtocolModeCharUuid);
  if (protoMode && (protoMode->canWrite() || protoMode->canWriteNoResponse())) {
    uint8_t reportMode = 0x01;  // 0x01 = Report Protocol
    protoMode->writeValue(&reportMode, 1, /*response=*/false);
  }

  // Subscribe to ALL input Report characteristics that can notify OR indicate.
  // Some BLE page turners expose indicate-only input reports; treating notify
  // as the only valid transport makes pairing appear to work but no usable
  // input ever arrives on the remap screen. NimBLE 1.x returns a
  // pointer-to-vector (nullable).
  bool subscribed = false;
  const auto* chars = hidService->getCharacteristics(true);
  if (chars) {
    for (auto* chr : *chars) {
      if (chr->getUUID() == kHidReportCharUuid && (chr->canNotify() || chr->canIndicate())) {
        // Clear stale CCCD state when reusing a client across reconnects —
        // NimBLE-Arduino caches subscription state on the characteristic, and
        // a previous connection's ghost value can make subscribe() think it
        // has nothing to do and return without writing CCCD.
        (void)chr->unsubscribe();
        const bool useNotify = chr->canNotify();
        const bool ok =
            chr->subscribe(useNotify, [this](NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
              onHidReport(data, len, /*isBootKeyboard=*/false);
            });
        if (ok) {
          subscribed = true;
          LOG_INF("BLE", "Subscribed to HID Report characteristic via %s", useNotify ? "notify" : "indicate");
        } else {
          LOG_INF("BLE", "HID Report subscribe failed (continuing)");
        }
      }
    }
  }

  // Also subscribe to Boot Keyboard Input if present — purely as a fallback
  // for devices that only notify on the boot char. We no longer switch the
  // peer into Boot Protocol, so a device that supports both will still emit
  // its Report-Protocol stream on the generic chars above.
  NimBLERemoteCharacteristic* bootKbChar = hidService->getCharacteristic(kHidBootKbInputCharUuid);
  if (bootKbChar && (bootKbChar->canNotify() || bootKbChar->canIndicate())) {
    (void)bootKbChar->unsubscribe();
    const bool useNotify = bootKbChar->canNotify();
    const bool ok =
        bootKbChar->subscribe(useNotify, [this](NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
          onHidReport(data, len, /*isBootKeyboard=*/true);
        });
    if (ok) {
      subscribed = true;
      LOG_INF("BLE", "Subscribed to Boot Keyboard Input via %s", useNotify ? "notify" : "indicate");
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
  gamebrickMode = false;

  portENTER_CRITICAL(&stateLock);
  for (int i = 0; i < kButtonCount; i++) {
    currentPressed[i] = false;
    previousPressed[i] = false;
    edgePressed[i] = false;
    edgeReleased[i] = false;
  }
  portEXIT_CRITICAL(&stateLock);

  resetReportBaseline();
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

// Case-insensitive substring match — names vary in casing across firmware revs
// of the Gamebrick family, so "IINE Game Brick" and "iine gamebrick" should
// both trigger the device-specific decoder.
static bool containsIgnoreCase(const std::string& haystack, const char* needle) {
  const size_t needleLen = strlen(needle);
  if (needleLen == 0 || haystack.size() < needleLen) return false;
  for (size_t i = 0; i + needleLen <= haystack.size(); i++) {
    size_t j = 0;
    for (; j < needleLen; j++) {
      char a = haystack[i + j];
      char b = needle[j];
      if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
      if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
      if (a != b) break;
    }
    if (j == needleLen) return true;
  }
  return false;
}

void BleHidManager::detectGamebrickFromAddressOrName(const std::string& address, const std::string& name) {
  // Drunkpenguin device table lists MAC prefix 60:4d:ec as a known Gamebrick
  // identity; match either that prefix or the device name. Address string
  // from NimBLE is lowercase hex, e.g. "60:4d:ec:12:34:56".
  const bool macMatch = address.size() >= 8 &&
                        (address.rfind("60:4d:ec", 0) == 0 || address.rfind("60:4D:EC", 0) == 0);
  const bool nameMatch = containsIgnoreCase(name, "IINE") || containsIgnoreCase(name, "Game Brick") ||
                         containsIgnoreCase(name, "Gamebrick");
  gamebrickMode = macMatch || nameMatch;
  if (gamebrickMode) {
    LOG_INF("BLE", "Gamebrick device detected (addr=%s name=%s) — using custom decoder",
            address.c_str(), name.c_str());
  }
}

// Minimal-viable Gamebrick V2 report decoder. Ported (subset) from
// thedrunkpenguin/crosspoint-reader-ble crosspoint-ble-1.2. Intentionally
// omits counter-freeze (0x07D0) disambiguation, stale-hold reset, center-
// press-frame LEFT heuristic, and A/B distinction — those are follow-ups
// once a volunteer confirms basic D-pad registers on the remap screen.
//
// Report layout (5 bytes):
//   byte[0] : frame status, bit 0 = active/press, clear = release tail
//   byte[1..2] : 16-bit cycling counter (ignored here)
//   byte[3] : joystick X, center = 0x98
//   byte[4] : D-pad / vertical: 0x07=UP, 0x09=DOWN, 0x08=idle/horizontal
//
// Emitted keycodes (standard HID Keyboard/Keypad Usage Page):
//   0x07 UP, 0x09 DOWN, 0x50 LEFT_ARROW, 0x4F RIGHT_ARROW
void BleHidManager::onGamebrickReport(const uint8_t* data, size_t len) {
  if (len < 5) return;

  const bool pressed = (data[0] & 0x01) != 0;
  const uint8_t b4 = data[4];
  uint16_t keycode = 0;

  if (b4 == 0x07) {
    keycode = 0x07;  // UP
  } else if (b4 == 0x09) {
    keycode = 0x09;  // DOWN
  } else if (b4 == 0x08) {
    const int dx = (int)data[3] - 0x98;
    if (dx < -2) {
      keycode = 0x4F;  // RIGHT
    } else if (dx > 0) {
      keycode = 0x50;  // LEFT
    }
  }

  if (keycode != 0) {
    lastRawKeycode = keycode;
  }

  if (captureMode) return;

  portENTER_CRITICAL(&stateLock);
  // Clear all current button state for this device — Gamebrick only ever
  // reports one logical input at a time (D-pad is mutually exclusive with
  // joystick-horizontal), so we do not need to OR with previous state.
  for (int b = 0; b < kButtonCount; b++) {
    currentPressed[b] = false;
  }
  if (pressed && keycode != 0) {
    for (int b = 0; b < kButtonCount; b++) {
      if (SETTINGS.bleKeyMap[b] == keycode) currentPressed[b] = true;
    }
  }
  portEXIT_CRITICAL(&stateLock);
}

void BleHidManager::onHidReport(const uint8_t* data, size_t len, bool isBootKeyboard) {
  if (len == 0) return;

  // Route Gamebrick-family devices through the dedicated decoder before the
  // generic parsers — the byte-diff path below produces garbage on these
  // devices because they encode D-pad as byte VALUES (0x07/0x09) rather than
  // bit flags. Only applies to non-boot-keyboard reports; if the Gamebrick
  // somehow emits a boot-keyboard frame we still want the standard path.
  if (gamebrickMode && !isBootKeyboard) {
    onGamebrickReport(data, len);
    return;
  }

  // Boot keyboard protocol: [0]=modifiers, [1]=reserved, [2..7]=up to 6 keycodes.
  // Only apply this interpretation to reports that actually came from the boot-
  // keyboard input characteristic — a gamepad's generic report may coincidentally
  // be 3–8 bytes long and would be garbled if parsed as boot keyboard.
  if (isBootKeyboard && len >= 3 && len <= 8) {
    uint8_t modifiers = data[0];

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

    if (captureMode) return;

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

  // Generic HID report (gamepads, custom remotes). We don't parse the HID
  // descriptor, so we can't know which bytes encode buttons vs analog axes.
  // Instead, snapshot the previous report and diff byte-by-byte: each bit that
  // transitions 0→1 is treated as a button-press event, 1→0 as release. The
  // synthesized keycode encodes the (byte, bit) position, which users bind in
  // the remap activity the same way they'd bind a keyboard key.
  const size_t n = (len < kMaxDiffBytes) ? len : kMaxDiffBytes;

  if (!reportBaselined) {
    memcpy(lastReport, data, n);
    for (size_t i = n; i < kMaxDiffBytes; i++) lastReport[i] = 0;
    lastReportLen = n;
    reportBaselined = true;
    return;
  }

  const size_t diffLen = (n > lastReportLen) ? n : lastReportLen;

  portENTER_CRITICAL(&stateLock);
  for (size_t i = 0; i < diffLen && i < kMaxDiffBytes; i++) {
    const uint8_t prev = (i < lastReportLen) ? lastReport[i] : 0;
    const uint8_t curr = (i < n) ? data[i] : 0;
    if (prev == curr) continue;

    const uint8_t rising = (uint8_t)(curr & ~prev);
    const uint8_t falling = (uint8_t)(prev & ~curr);

    for (uint8_t b = 0; b < 8; b++) {
      const uint8_t mask = (uint8_t)(1u << b);
      const uint16_t code = (uint16_t)(0xF000u | ((i & 0x1Fu) << 3) | (b & 0x07u));
      if (rising & mask) {
        if (captureMode) {
          lastRawKeycode = code;
        } else {
          for (int bt = 0; bt < kButtonCount; bt++) {
            if (SETTINGS.bleKeyMap[bt] == code) currentPressed[bt] = true;
          }
        }
      }
      if ((falling & mask) && !captureMode) {
        for (int bt = 0; bt < kButtonCount; bt++) {
          if (SETTINGS.bleKeyMap[bt] == code) currentPressed[bt] = false;
        }
      }
    }
  }
  portEXIT_CRITICAL(&stateLock);

  memcpy(lastReport, data, n);
  for (size_t i = n; i < kMaxDiffBytes; i++) lastReport[i] = 0;
  lastReportLen = n;
}

void BleHidManager::resetReportBaseline() {
  // Seed baseline with all-zeros and mark as baselined. Gamepads over BLE HID
  // typically only notify on state change, so if we wait for the first report
  // to establish the baseline the user's first button press is silently
  // consumed. Starting from a zeroed baseline makes the first real report
  // produce correct rising-edge events relative to the resting state.
  for (size_t i = 0; i < kMaxDiffBytes; i++) lastReport[i] = 0;
  lastReportLen = kMaxDiffBytes;
  reportBaselined = true;
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
    // Rebaseline the diff buffer so the next generic report establishes the
    // resting state — otherwise the first user-initiated edge might be missed
    // (or an old edge from before capture began would fire).
    resetReportBaseline();
  }
}

uint16_t BleHidManager::captureRawKeycode() {
  uint16_t code = lastRawKeycode;
  lastRawKeycode = 0;
  return code;
}

void BleHidManager::clearCapturedKeycode() { lastRawKeycode = 0; }
