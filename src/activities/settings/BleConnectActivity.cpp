#include "BleConnectActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "BleHidManager.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"

void BleConnectActivity::onEnter() {
  Activity::onEnter();
  uiState = BleConnectState::Initializing;
  selectedIndex = 0;
  stateEnteredAt = millis();
  requestUpdate();
}

void BleConnectActivity::onExit() { Activity::onExit(); }

void BleConnectActivity::loop() {
  switch (uiState) {
    case BleConnectState::Initializing: {
      if (!BLE_HID.isInitialized()) {
        BLE_HID.init();
      }
      BLE_HID.startScan(10);
      uiState = BleConnectState::Scanning;
      stateEnteredAt = millis();
      requestUpdate();
      break;
    }

    case BleConnectState::Scanning: {
      if (BLE_HID.isScanComplete()) {
        uiState = BleConnectState::DeviceList;
        selectedIndex = 0;
        requestUpdate();
      } else if (millis() - stateEnteredAt > 500) {
        requestUpdate();
        stateEnteredAt = millis();
      }

      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        BLE_HID.stopScan();
        onComplete(false);
        return;
      }
      break;
    }

    case BleConnectState::DeviceList: {
      const auto& devices = BLE_HID.getScanResults();
      const int count = static_cast<int>(devices.size());

      // Side Up: rescan
      if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
        BLE_HID.startScan(10);
        uiState = BleConnectState::Scanning;
        stateEnteredAt = millis();
        requestUpdate();
        return;
      }

      // Side Down: unpair if paired, otherwise cancel
      if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
        if (SETTINGS.bleDeviceAddr[0] != '\0') {
          uiState = BleConnectState::UnpairConfirm;
          requestUpdate();
        } else {
          onComplete(false);
        }
        return;
      }

      // Back: cancel
      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        onComplete(false);
        return;
      }

      if (count == 0) break;

      // Navigate list
      if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
        selectedIndex = buttonNavigator.nextIndex(selectedIndex, count);
        requestUpdate();
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
        selectedIndex = buttonNavigator.previousIndex(selectedIndex, count);
        requestUpdate();
      }

      // Confirm: initiate async connect to selected device
      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        if (selectedIndex >= 0 && selectedIndex < count) {
          BLE_HID.connectToDeviceAsync(devices[selectedIndex].address);
          uiState = BleConnectState::Connecting;
          stateEnteredAt = millis();
          requestUpdate();
        }
      }
      break;
    }

    case BleConnectState::Connecting: {
      // Poll for async connection result
      const auto bleState = BLE_HID.getState();
      if (bleState == BleHidManager::State::Connected) {
        // Save pairing info
        const auto& devices = BLE_HID.getScanResults();
        if (selectedIndex >= 0 && selectedIndex < static_cast<int>(devices.size())) {
          const auto& device = devices[selectedIndex];
          strncpy(SETTINGS.bleDeviceAddr, device.address.c_str(), sizeof(SETTINGS.bleDeviceAddr) - 1);
          SETTINGS.bleDeviceAddr[sizeof(SETTINGS.bleDeviceAddr) - 1] = '\0';
          strncpy(SETTINGS.bleDeviceName, device.name.c_str(), sizeof(SETTINGS.bleDeviceName) - 1);
          SETTINGS.bleDeviceName[sizeof(SETTINGS.bleDeviceName) - 1] = '\0';
        }
        SETTINGS.bleEnabled = 1;
        SETTINGS.saveToFile();

        uiState = BleConnectState::Connected;
        stateEnteredAt = millis();
        requestUpdate();
      } else if (bleState == BleHidManager::State::Idle) {
        // Connection failed (connectTask set state back to Idle)
        uiState = BleConnectState::Failed;
        stateEnteredAt = millis();
        requestUpdate();
      }
      // else still Connecting — keep polling
      break;
    }

    case BleConnectState::Connected: {
      if (millis() - stateEnteredAt > 1500 || mappedInput.wasAnyPressed()) {
        onComplete(true);
        return;
      }
      break;
    }

    case BleConnectState::Failed: {
      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        onComplete(false);
        return;
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        BLE_HID.startScan(10);
        uiState = BleConnectState::Scanning;
        stateEnteredAt = millis();
        requestUpdate();
      }
      break;
    }

    case BleConnectState::UnpairConfirm: {
      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        SETTINGS.bleDeviceAddr[0] = '\0';
        SETTINGS.bleDeviceName[0] = '\0';
        SETTINGS.bleEnabled = 0;
        for (int i = 0; i < CrossPointSettings::BLE_KEY_MAP_SIZE; i++) {
          SETTINGS.bleKeyMap[i] = 0;
        }
        BLE_HID.disconnect();
        SETTINGS.saveToFile();
        onComplete(false);
        return;
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        uiState = BleConnectState::DeviceList;
        requestUpdate();
      }
      break;
    }
  }
}

void BleConnectActivity::render(Activity::RenderLock&&) {
  renderer.clearScreen();

  switch (uiState) {
    case BleConnectState::Initializing:  renderInitializing();  break;
    case BleConnectState::Scanning:      renderScanning();      break;
    case BleConnectState::DeviceList:    renderDeviceList();    break;
    case BleConnectState::Connecting:    renderConnecting();    break;
    case BleConnectState::Connected:     renderConnected();     break;
    case BleConnectState::Failed:        renderFailed();        break;
    case BleConnectState::UnpairConfirm: renderUnpairConfirm(); break;
  }

  renderer.displayBuffer();
}

void BleConnectActivity::renderInitializing() const {
  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_BLUETOOTH_HID), true, EpdFontFamily::REGULAR);
  renderer.drawCenteredText(UI_10_FONT_ID, 150, tr(STR_BLE_INITIALIZING));
}

void BleConnectActivity::renderScanning() const {
  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_BLUETOOTH_HID), true, EpdFontFamily::REGULAR);
  renderer.drawCenteredText(UI_10_FONT_ID, 150, tr(STR_BLE_SCANNING));

  const auto& devices = BLE_HID.getScanResults();
  if (!devices.empty()) {
    char buf[32];
    snprintf(buf, sizeof(buf), "Found: %d", (int)devices.size());
    renderer.drawCenteredText(UI_10_FONT_ID, 180, buf);
  }
}

void BleConnectActivity::renderDeviceList() const {
  const auto pageWidth = renderer.getScreenWidth();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_BLUETOOTH_HID), true, EpdFontFamily::REGULAR);

  if (SETTINGS.bleDeviceAddr[0] != '\0') {
    char pairedBuf[64];
    snprintf(pairedBuf, sizeof(pairedBuf), "%s%s", tr(STR_BLE_PAIRED_PREFIX), SETTINGS.bleDeviceName);
    renderer.drawCenteredText(SMALL_FONT_ID, 38, pairedBuf);
  }

  const auto& devices = BLE_HID.getScanResults();
  if (devices.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, 150, tr(STR_BLE_NO_DEVICES));
  } else {
    const int startY = 55;
    const int rowHeight = 28;
    const int maxVisible = 6;

    for (int i = 0; i < static_cast<int>(devices.size()) && i < maxVisible; i++) {
      const int y = startY + i * rowHeight;
      const bool isSelected = (i == selectedIndex);

      if (isSelected) {
        renderer.fillRect(0, y - 2, pageWidth - 1, rowHeight);
      }

      renderer.drawText(UI_10_FONT_ID, 10, y, devices[i].name.c_str(), !isSelected);

      const char* signal = signalIndicator(devices[i].rssi);
      const auto sigWidth = renderer.getTextWidth(SMALL_FONT_ID, signal);
      renderer.drawText(SMALL_FONT_ID, pageWidth - 10 - sigWidth, y + 2, signal, !isSelected);
    }
  }

  renderer.drawCenteredText(SMALL_FONT_ID, 250, tr(STR_BLE_SCAN_HINT), true);
  // Dynamic hint: show unpair option when paired, otherwise just rescan/back
  if (SETTINGS.bleDeviceAddr[0] != '\0') {
    renderer.drawCenteredText(SMALL_FONT_ID, 270, "Side Up: Rescan  Side Down: Unpair", true);
  } else {
    renderer.drawCenteredText(SMALL_FONT_ID, 270, tr(STR_BLE_RESCAN), true);
  }
}

void BleConnectActivity::renderConnecting() const {
  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_BLUETOOTH_HID), true, EpdFontFamily::REGULAR);
  renderer.drawCenteredText(UI_10_FONT_ID, 150, tr(STR_BLE_CONNECTING));

  const auto& devices = BLE_HID.getScanResults();
  if (selectedIndex >= 0 && selectedIndex < static_cast<int>(devices.size())) {
    renderer.drawCenteredText(UI_10_FONT_ID, 180, devices[selectedIndex].name.c_str());
  }
}

void BleConnectActivity::renderConnected() const {
  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_BLUETOOTH_HID), true, EpdFontFamily::REGULAR);
  renderer.drawCenteredText(UI_10_FONT_ID, 140, tr(STR_BLE_CONNECTED));
  renderer.drawCenteredText(UI_10_FONT_ID, 170, BLE_HID.getConnectedDeviceName().c_str());
}

void BleConnectActivity::renderFailed() const {
  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_BLUETOOTH_HID), true, EpdFontFamily::REGULAR);
  renderer.drawCenteredText(UI_10_FONT_ID, 150, tr(STR_BLE_CONNECT_FAILED));
  renderer.drawCenteredText(SMALL_FONT_ID, 250, tr(STR_BLE_SCAN_HINT), true);
}

void BleConnectActivity::renderUnpairConfirm() const {
  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_BLUETOOTH_HID), true, EpdFontFamily::REGULAR);
  renderer.drawCenteredText(UI_10_FONT_ID, 140, tr(STR_BLE_UNPAIR_CONFIRM));
  renderer.drawCenteredText(UI_10_FONT_ID, 170, SETTINGS.bleDeviceName);

  GUI.drawButtonHints(renderer, tr(STR_BACK), tr(STR_CONFIRM), "", "");
}

const char* BleConnectActivity::signalIndicator(int rssi) const {
  if (rssi >= -50) return "****";
  if (rssi >= -65) return "*** ";
  if (rssi >= -80) return "**  ";
  return "*   ";
}
