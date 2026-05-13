/**
 * @file OtaUpdater.h
 * @brief Over-the-air firmware update checker and installer (RFC #146 typed outcomes).
 *
 * Queries the GitHub Releases API for the latest firmware, compares version
 * strings, and performs ESP-IDF HTTPS OTA if a newer version is found. Uses
 * the device's secondary OTA partition (app1) for safe rollback.
 *
 * Two transports stay separate (Arduino HTTPClient for the metadata fetch,
 * esp_https_ota for the install) — both proven on the C3's tight heap.
 * Failure surfaces are typed sum types per phase, so the UI switches on a
 * tag with structured per-case fields and never parses const char* strings
 * that point into static buffers.
 */
#pragma once

#include <esp_err.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

// ---- Pre-flight network probe (cheap, runs once at start of checkForUpdate) ----
// Plain POD. Captured by value; lives on the caller's stack / activity.
struct NetPreflight {
  enum class Dns : uint8_t { Ok, Failed };
  enum class Tcp : uint8_t { Ok, Failed, Skipped };  // Skipped if Dns::Failed

  Dns dns = Dns::Failed;
  Tcp tcp = Tcp::Skipped;
  uint32_t resolvedIpV4 = 0;  // 0 if Dns::Failed
  uint32_t freeHeapBytes = 0;
  // Reserved for the diag::Log unification (RFC #146 follow-up). Today: 0.
  uint16_t lastWifiReason = 0;
};

// ---- Outcome of checkForUpdate() — Arduino HTTPClient transport ----
struct CheckOutcome {
  enum class Tag : uint8_t {
    UpdateAvailable,  // newer release found, ready for installUpdate()
    AlreadyLatest,    // server reachable; tag matches current or is older
    NoFirmwareAsset,  // tag present but no firmware.bin attachment
    HttpClientError,  // HTTPClient negative code (transport-level)
    HttpStatusError,  // server returned non-2xx (and not 403/429)
    RateLimited,      // 403 / 429
    JsonParseError,   // SAX parser found no tag_name
    InternalError,    // begin() / null stream / etc.
  };

  Tag tag = Tag::InternalError;
  NetPreflight preflight;

  // Per-tag payload. Read only the field matching `tag`.
  union U {
    uint32_t latestSize;  // updateAvailable
    int httpcCode;        // httpClientError (negative HTTPC_*)
    int httpStatus;       // httpStatusError or rateLimited (4xx/5xx)
    U() : latestSize(0) {}
  } u;

  // Populated on UpdateAvailable / AlreadyLatest. Empty otherwise.
  std::string latestVersion;
};

// ---- Streaming install progress (passed to ProgressFn per perform iteration) ----
struct InstallProgress {
  uint32_t bytesProcessed = 0;
  uint32_t bytesExpected = 0;
};

// ---- Outcome of installUpdate() — esp_https_ota transport ----
struct InstallOutcome {
  enum class Tag : uint8_t {
    Success,
    NotNewer,       // installUpdate called without prior UpdateAvailable
    BeginFailed,    // esp_https_ota_begin (TLS / DNS / redirect)
    PerformFailed,  // esp_https_ota_perform mid-stream
    Incomplete,     // exited OK but Content-Length not satisfied
    FinishFailed,   // hash / signature / partition validate
  };

  Tag tag = Tag::NotNewer;
  uint32_t bytesProcessed = 0;
  uint32_t bytesExpected = 0;
  esp_err_t espErr = ESP_OK;
  const char* espErrName = nullptr;  // rodata via esp_err_to_name; null if espErr == ESP_OK
};

class OtaUpdater {
 public:
  using ProgressFn = std::function<void(const InstallProgress&)>;

  OtaUpdater() = default;

  CheckOutcome checkForUpdate();
  InstallOutcome installUpdate(ProgressFn onProgress = {});

  // Held between checkForUpdate() and installUpdate(). Caller may also read
  // these from CheckOutcome.latestVersion / .u.updateAvailable.latestSize
  // — exposed here so legacy render paths that already grab the version
  // for the confirmation screen do not need to thread the outcome through.
  bool hasPendingUpdate() const { return pending_.has; }
  const std::string& latestVersion() const { return pending_.version; }
  uint32_t latestSize() const { return pending_.size; }

 private:
  struct Pending {
    bool has = false;
    std::string version;
    std::string url;
    uint32_t size = 0;
  } pending_;

  static NetPreflight runPreflight();
};
