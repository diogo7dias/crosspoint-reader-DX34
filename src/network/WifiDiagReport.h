#pragma once

#include <WiFi.h>
#include <stdint.h>

// Wi-Fi connection diagnostic report.
//
// Mirrors the crash_report.txt pattern: on a failed connection attempt the
// device writes a single plain-text file to the SD root that the user can
// pull off and share for debugging. The report contains technical
// diagnostics ONLY — no SSID, BSSID, MAC, IP, or any neighbour-network
// names. PII is dropped or length-encoded.
namespace WifiDiagReport {

enum class FailureKind : uint8_t {
  StatusFailed,    // WL_CONNECT_FAILED
  NoSsidAvail,     // WL_NO_SSID_AVAIL
  Timeout,         // 15 s timeout in checkConnectionStatus
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

// Serialize the timeline + environment to /wifi_report.txt. Overwrites any
// previous report. Safe to call from any task.
void writeReportOnFailure(FailureKind kind);

}  // namespace WifiDiagReport
