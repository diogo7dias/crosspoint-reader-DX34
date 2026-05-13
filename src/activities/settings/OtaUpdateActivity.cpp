#include "OtaUpdateActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include "esp_heap_caps.h"

#include <cstdio>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"
#include "network/OtaUpdater.h"
#include "network/WifiDiagReport.h"

namespace {
// Unpack NetPreflight.resolvedIpV4 (network-byte-packed uint32) into "a.b.c.d"
// for the failure screen. Returns chars written, ≤ 16 incl. NUL.
size_t formatIpV4(uint32_t ip, char* buf, size_t cap) {
  return std::snprintf(buf, cap, "%u.%u.%u.%u", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
}
}  // namespace

void OtaUpdateActivity::onWifiSelectionComplete(const bool success) {
  exitActivity();

  if (!success) {
    LOG_ERR("OTA", "WiFi connection failed, exiting");
    goBack();
    return;
  }

  LOG_DBG("OTA", "WiFi connected, checking for update");

  {
    RenderLock lock(*this);
    state = CHECKING_FOR_UPDATE;
  }
  requestUpdateAndWait();

  // TLS handshake needs ~10 KB contiguous for mbedTLS session buffers.
  // On the C3's tight heap (~27 KB free) the font cache often fragments
  // memory enough to cause a -1 (HTTPC_ERROR_CONNECTION_REFUSED) during
  // the handshake. Drop reclaimable caches before attempting.
  const size_t heapBefore = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->clearCache();
    const size_t heapAfter = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (heapAfter > heapBefore) {
      LOG_INF("OTA", "Heap recovered via cache evict: %u -> %u", static_cast<unsigned>(heapBefore),
              static_cast<unsigned>(heapAfter));
    }
  }

  CheckOutcome res;
  for (int attempt = 0; attempt <= OTA_CHECK_MAX_RETRIES; attempt++) {
    if (attempt > 0) {
      LOG_INF("OTA", "Retry attempt %d/%d", attempt, OTA_CHECK_MAX_RETRIES);
      delay(2000 * attempt);
    }
    res = updater.checkForUpdate();
    if (res.tag == CheckOutcome::Tag::UpdateAvailable || res.tag == CheckOutcome::Tag::AlreadyLatest ||
        res.tag == CheckOutcome::Tag::NoFirmwareAsset) {
      break;
    }
    if (res.tag == CheckOutcome::Tag::RateLimited && attempt < OTA_CHECK_MAX_RETRIES) {
      LOG_INF("OTA", "Rate limited, waiting before retry...");
      delay(5000);
      continue;
    }
  }

  switch (res.tag) {
    case CheckOutcome::Tag::UpdateAvailable: {
      RenderLock lock(*this);
      lastCheck = std::move(res);
      state = WAITING_CONFIRMATION;
      break;
    }
    case CheckOutcome::Tag::AlreadyLatest:
    case CheckOutcome::Tag::NoFirmwareAsset: {
      RenderLock lock(*this);
      lastCheck = std::move(res);
      state = NO_UPDATE;
      break;
    }
    default: {
      LOG_DBG("OTA", "Update check failed: tag=%d", static_cast<int>(res.tag));
      RenderLock lock(*this);
      lastCheck = std::move(res);
      state = FAILED;
      WifiDiagReport::writeReportOnFailure(WifiDiagReport::FailureKind::OtaCheckFailed);
      break;
    }
  }
  requestUpdate();
}

void OtaUpdateActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  // Turn on WiFi immediately
  LOG_DBG("OTA", "Turning on WiFi...");
  WiFi.mode(WIFI_STA);

  // Launch WiFi selection subactivity
  LOG_DBG("OTA", "Launching WifiSelectionActivity...");
  enterNewActivity(new WifiSelectionActivity(renderer, mappedInput,
                                             [this](const bool connected) { onWifiSelectionComplete(connected); }));
}

void OtaUpdateActivity::onExit() {
  ActivityWithSubactivity::onExit();

  // Turn off wifi
  WiFi.disconnect(false);  // false = don't erase credentials, send disconnect frame
  delay(100);              // Allow disconnect frame to be sent
  WiFi.mode(WIFI_OFF);
  delay(100);  // Allow WiFi hardware to fully power down
}

void OtaUpdateActivity::render(Activity::RenderLock&&) {
  if (subActivity) {
    // Subactivity handles its own rendering
    return;
  }

  float updaterProgress = 0;
  if (state == UPDATE_IN_PROGRESS) {
    LOG_DBG("OTA", "Update progress: %u / %u", (unsigned)progressBytesDone, (unsigned)progressBytesTotal);
    updaterProgress = static_cast<float>(progressBytesDone) / static_cast<float>(progressBytesTotal);
    // Only redraw every 5% to limit e-paper refresh cost during download
    const int currentPct = static_cast<int>(updaterProgress * 100);
    if (currentPct != 100 && currentPct - lastUpdaterPercentage < 5) {
      return;
    }
    lastUpdaterPercentage = currentPct;
  }

  const auto pageWidth = renderer.getScreenWidth();

  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_UPDATE), true, EpdFontFamily::REGULAR);

  if (state == CHECKING_FOR_UPDATE) {
    renderer.drawCenteredText(UI_10_FONT_ID, 300, tr(STR_CHECKING_UPDATE), true, EpdFontFamily::REGULAR);
    renderer.displayBuffer();
    return;
  }

  if (state == WAITING_CONFIRMATION) {
    renderer.drawCenteredText(UI_10_FONT_ID, 200, tr(STR_NEW_UPDATE), true, EpdFontFamily::REGULAR);
    renderer.drawText(UI_10_FONT_ID, 20, 250, (std::string(tr(STR_CURRENT_VERSION)) + CROSSPOINT_VERSION).c_str());
    renderer.drawText(UI_10_FONT_ID, 20, 270, (std::string(tr(STR_NEW_VERSION)) + updater.latestVersion()).c_str());

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_UPDATE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == UPDATE_IN_PROGRESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, 310, tr(STR_UPDATING), true, EpdFontFamily::REGULAR);
    renderer.drawRect(20, 350, pageWidth - 40, 50);
    renderer.fillRect(24, 354, static_cast<int>(updaterProgress * static_cast<float>(pageWidth - 44)), 42);
    renderer.drawCenteredText(UI_10_FONT_ID, 420,
                              (std::to_string(static_cast<int>(updaterProgress * 100)) + "%").c_str());
    renderer.drawCenteredText(UI_10_FONT_ID, 440,
                              (std::to_string(progressBytesDone) + " / " + std::to_string(progressBytesTotal)).c_str());
    renderer.displayBuffer();
    return;
  }

  if (state == NO_UPDATE) {
    renderer.drawCenteredText(UI_10_FONT_ID, 300, tr(STR_NO_UPDATE), true, EpdFontFamily::REGULAR);
    renderer.displayBuffer();
    return;
  }

  if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, 300, tr(STR_UPDATE_FAILED), true, EpdFontFamily::REGULAR);
    char hintBuf[64];
    char detailBuf[80];
    const char* hint = nullptr;
    const char* detail = nullptr;

    if (lastInstallPresent) {
      switch (lastInstall.tag) {
        case InstallOutcome::Tag::BeginFailed:
          hint = "download start (TLS/redirect)";
          detail = lastInstall.espErrName;
          break;
        case InstallOutcome::Tag::PerformFailed:
          hint = "download interrupted";
          std::snprintf(detailBuf, sizeof(detailBuf), "%s @ %u / %u",
                        lastInstall.espErrName ? lastInstall.espErrName : "esp_err",
                        (unsigned)lastInstall.bytesProcessed, (unsigned)lastInstall.bytesExpected);
          detail = detailBuf;
          break;
        case InstallOutcome::Tag::Incomplete:
          hint = "download truncated";
          std::snprintf(detailBuf, sizeof(detailBuf), "%u / %u bytes", (unsigned)lastInstall.bytesProcessed,
                        (unsigned)lastInstall.bytesExpected);
          detail = detailBuf;
          break;
        case InstallOutcome::Tag::FinishFailed:
          hint = "image validate/partition";
          detail = lastInstall.espErrName;
          break;
        case InstallOutcome::Tag::NotNewer:
          hint = "latest is older";
          break;
        case InstallOutcome::Tag::Success:
          break;  // not a failure path
      }
    } else {
      switch (lastCheck.tag) {
        case CheckOutcome::Tag::HttpClientError:
          hint = "network/TLS to GitHub";
          std::snprintf(detailBuf, sizeof(detailBuf), "HTTPC %d", lastCheck.u.httpcCode);
          detail = detailBuf;
          break;
        case CheckOutcome::Tag::HttpStatusError:
          hint = "server error";
          std::snprintf(detailBuf, sizeof(detailBuf), "HTTP %d", lastCheck.u.httpStatus);
          detail = detailBuf;
          break;
        case CheckOutcome::Tag::RateLimited:
          hint = "Try again in a few minutes";
          break;
        case CheckOutcome::Tag::JsonParseError:
          hint = "release JSON parse";
          break;
        case CheckOutcome::Tag::NoFirmwareAsset:
          hint = "no firmware.bin in latest tag";
          break;
        case CheckOutcome::Tag::InternalError:
          hint = "check setup";
          break;
        case CheckOutcome::Tag::UpdateAvailable:
        case CheckOutcome::Tag::AlreadyLatest:
          break;  // not a failure path
      }
    }

    if (hint != nullptr) {
      std::snprintf(hintBuf, sizeof(hintBuf), "%s", hint);
      renderer.drawCenteredText(UI_10_FONT_ID, 340, hintBuf);
    }
    if (detail != nullptr && detail[0] != '\0') {
      renderer.drawCenteredText(UI_10_FONT_ID, 360, detail);
    }

    // Pre-flight diagnostic line — DNS resolve outcome + heap snapshot.
    // Differentiates network reachability vs TLS handshake vs OOM as the
    // underlying cause. Rendered for any check-phase failure.
    const auto& pf = lastCheck.preflight;
    char preflightBuf[80];
    if (pf.dns == NetPreflight::Dns::Ok) {
      char ipBuf[16];
      formatIpV4(pf.resolvedIpV4, ipBuf, sizeof(ipBuf));
      std::snprintf(preflightBuf, sizeof(preflightBuf), "dns=%s tcp=%s heap=%u", ipBuf,
                    pf.tcp == NetPreflight::Tcp::Ok ? "OK" : "FAIL", (unsigned)pf.freeHeapBytes);
    } else if (pf.freeHeapBytes != 0) {
      std::snprintf(preflightBuf, sizeof(preflightBuf), "dns=FAIL heap=%u", (unsigned)pf.freeHeapBytes);
    } else {
      preflightBuf[0] = '\0';
    }
    if (preflightBuf[0] != '\0') {
      renderer.drawCenteredText(UI_10_FONT_ID, 380, preflightBuf);
    }
    renderer.displayBuffer();
    return;
  }

  if (state == FINISHED) {
    renderer.drawCenteredText(UI_10_FONT_ID, 300, tr(STR_UPDATE_COMPLETE), true, EpdFontFamily::REGULAR);
    renderer.drawCenteredText(UI_10_FONT_ID, 350, tr(STR_POWER_ON_HINT));
    renderer.displayBuffer();
    state = SHUTTING_DOWN;
    return;
  }
}

void OtaUpdateActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (state == WAITING_CONFIRMATION) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      LOG_DBG("OTA", "New update available, starting download...");
      {
        RenderLock lock(*this);
        state = UPDATE_IN_PROGRESS;
        progressBytesDone = 0;
        progressBytesTotal = updater.latestSize() ? updater.latestSize() : 1;
      }
      requestUpdate();
      requestUpdateAndWait();
      // ProgressFn fires per esp_https_ota_perform iteration with typed
      // bytes-processed/expected — replaces the old getRender() polled flag.
      const auto res = updater.installUpdate([this](const InstallProgress& p) {
        progressBytesDone = p.bytesProcessed;
        if (p.bytesExpected > 0) progressBytesTotal = p.bytesExpected;
        requestUpdate();
      });

      if (res.tag != InstallOutcome::Tag::Success) {
        LOG_DBG("OTA", "Update failed: tag=%d", static_cast<int>(res.tag));
        {
          RenderLock lock(*this);
          lastInstall = res;
          lastInstallPresent = true;
          state = FAILED;
        }
        WifiDiagReport::writeReportOnFailure(WifiDiagReport::FailureKind::OtaInstallFailed);
        requestUpdate();
        return;
      }

      {
        RenderLock lock(*this);
        state = FINISHED;
      }
      requestUpdate();
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
    }

    return;
  }

  if (state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }

  if (state == NO_UPDATE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }

  if (state == SHUTTING_DOWN) {
    ESP.restart();
  }
}
