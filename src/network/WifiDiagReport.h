#pragma once

#include <WiFi.h>
#include <stdint.h>

// Crosspoint diagnostic report (RFC #146 stage 1).
//
// On a failed Wi-Fi connection OR OTA update attempt the device writes a
// single plain-text file to the SD root (/diag_report.txt) that the user
// can share for debugging. PII-free by construction: contains reason
// codes, HTTP/esp_err codes, channel, auth mode, RSSI, heap, and a
// connect/OTA breadcrumb timeline. Contains NO SSID, BSSID, MAC,
// neighbour-network names, or full URLs (the GitHub releases endpoint is
// hardcoded firmware-side and is not user-secret).
//
// Naming: namespace stays WifiDiagReport for now to minimise rename churn.
// The module's responsibilities span Wi-Fi connect + OTA update; full
// rename to a generic Diag namespace is a follow-up RFC #146 stage.
namespace WifiDiagReport {

enum class FailureKind : uint8_t {
  StatusFailed,      // WL_CONNECT_FAILED
  NoSsidAvail,       // WL_NO_SSID_AVAIL
  Timeout,           // 15 s timeout in checkConnectionStatus
  OtaCheckFailed,    // OtaUpdater::checkForUpdate returned a failure tag
  OtaInstallFailed,  // OtaUpdater::installUpdate returned a failure tag
};

// Register the WiFi event handler. Call once from setup(). Idempotent.
void begin();

// Apply country code "01" (worldwide / 802.11d auto). Must be called after
// the Wi-Fi driver is initialized (i.e. after a WiFi.mode() call). Idempotent
// — only applied once per boot. Without this, ESP-IDF defaults to a region
// that hides channels 12/13, which can make some APs invisible to scans.
void ensureCountryCodeApplied();

// Mark the start of a connect attempt. Resets the per-attempt timeline.
// targetSsidLen — length of the target SSID (value never stored)
// savedCount    — number of saved credentials in WIFI_STORE
// requiresPassword — true if the target network is encrypted
// usedSavedPassword — true if we used a previously-saved credential
// autoConnecting    — true if this is the boot-time auto-connect path
void noteAttemptStart(size_t targetSsidLen, size_t savedCount, bool requiresPassword, bool usedSavedPassword,
                      bool autoConnecting);

// Record a wl_status_t transition observed by the polling loop.
void noteStatus(wl_status_t status);

// Record scan summary for the most recent attempt.
// totalNetworks   — number of unique SSIDs seen
// targetFound     — was a network with the target's SSID-length present
// targetRssi/ch/auth — only set when targetFound
void noteScanSummary(size_t totalNetworks, bool targetFound, int32_t targetRssi, int32_t targetChannel,
                     uint8_t targetAuthMode);

// Serialize the timeline + environment to /diag_report.txt. Overwrites any
// previous report. Safe to call from any task.
void writeReportOnFailure(FailureKind kind);

// ---- OTA breadcrumbs (RFC #146 stage 1) ----
// Reset OTA section state at the start of an attempt. Idempotent.
void noteOtaAttemptStart();

// Pre-flight network probe outcome from OtaUpdater::runPreflight().
// resolvedIpV4 is the packed network-byte-order uint32 (0 if dnsOk == false).
void noteOtaPreflight(bool dnsOk, bool tcpOk, uint32_t resolvedIpV4, uint32_t freeHeapBytes);

// Check-phase outcome. tagAsByte is CheckOutcome::Tag cast to uint8_t (kept
// untyped here so this header does not pull OtaUpdater.h's HTTPClient
// transitive include into every translation unit). httpcCode is the
// negative HTTPClient transport code on Tag::HttpClientError; httpStatus
// is the 4xx/5xx code on HttpStatusError or RateLimited; both 0 otherwise.
void noteOtaCheckResult(uint8_t tagAsByte, int httpcCode, int httpStatus);

// Install-phase outcome. tagAsByte is InstallOutcome::Tag cast to uint8_t.
// espErrName may be nullptr (rodata via esp_err_to_name; safe to capture).
void noteOtaInstallResult(uint8_t tagAsByte, int32_t espErr, const char* espErrName, uint32_t bytesProcessed,
                          uint32_t bytesExpected);

}  // namespace WifiDiagReport
