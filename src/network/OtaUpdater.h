/**
 * @file OtaUpdater.h
 * @brief Over-the-air firmware update checker and installer.
 *
 * Queries the GitHub Releases API for the latest firmware, compares version
 * strings, and performs ESP-IDF HTTPS OTA if a newer version is found.
 * Uses the device's secondary OTA partition (app1) for safe rollback.
 */
#pragma once

#include <functional>
#include <string>

class OtaUpdater {
  bool updateAvailable = false;
  std::string latestVersion;
  std::string otaUrl;
  size_t otaSize = 0;
  size_t processedSize = 0;
  size_t totalSize = 0;
  bool render = false;
  // esp_err_to_name() returns a static C string from rodata, so a non-owning
  // pointer is safe to expose for on-screen rendering on failure paths.
  const char* lastEspErrName = nullptr;
  // Pre-flight diagnostic line (DNS resolve + heap free). Set at the start of
  // checkForUpdate so the failure screen can show whether DNS works and how
  // much heap is available — narrows network vs TLS vs OOM root cause without
  // a serial console. Non-owning pointer to a static buffer.
  const char* preflightDiag = nullptr;

 public:
  enum OtaUpdaterError {
    OK = 0,
    NO_UPDATE,
    HTTP_ERROR,
    JSON_PARSE_ERROR,
    UPDATE_OLDER_ERROR,
    INTERNAL_UPDATE_ERROR,
    OOM_ERROR,
    RATE_LIMITED,
    // Install-path failures, split out from INTERNAL_UPDATE_ERROR so the
    // failure screen can pinpoint which step broke without a serial console.
    OTA_BEGIN_ERROR,       // esp_https_ota_begin failed (TLS/redirect/DNS)
    OTA_INCOMPLETE_ERROR,  // download finished but Content-Length not satisfied
    OTA_FINISH_ERROR,      // image hash / signature / partition validate failed
  };

  size_t getOtaSize() const { return otaSize; }

  size_t getProcessedSize() const { return processedSize; }

  size_t getTotalSize() const { return totalSize; }

  bool getRender() const { return render; }

  const char* getLastEspErrName() const { return lastEspErrName; }

  const char* getPreflightDiag() const { return preflightDiag; }

  OtaUpdater() = default;
  bool isUpdateNewer() const;
  const std::string& getLatestVersion() const;
  OtaUpdaterError checkForUpdate();
  OtaUpdaterError installUpdate(std::function<void()> onProgress = nullptr);
};
