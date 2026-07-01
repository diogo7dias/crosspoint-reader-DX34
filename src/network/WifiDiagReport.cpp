#include "WifiDiagReport.h"

#include <Arduino.h>
#include <CrashInfo.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#ifndef CROSSPOINT_VERSION
#define CROSSPOINT_VERSION "unknown"
#endif

namespace WifiDiagReport {

namespace {

constexpr size_t TIMELINE_CAPACITY = 32;

enum class EntryKind : uint8_t {
  Status,
  Event,
};

struct TimelineEntry {
  uint32_t msOffset;
  EntryKind kind;
  uint16_t code;  // wl_status_t for Status, arduino_event_id_t for Event
  int32_t aux;    // disconnect reason / channel / 0
};

// All state in BSS — no heap usage. Module is single-attempt: each
// noteAttemptStart resets the buffer.
TimelineEntry s_timeline[TIMELINE_CAPACITY];
size_t s_timelineCount = 0;
uint32_t s_attemptStartMs = 0;
bool s_attemptInProgress = false;

size_t s_targetSsidLen = 0;
size_t s_savedCount = 0;
bool s_targetRequiresPassword = false;
bool s_targetUsedSavedPassword = false;
bool s_targetAutoConnecting = false;

bool s_scanRecorded = false;
size_t s_scanTotal = 0;
bool s_scanTargetFound = false;
int32_t s_scanTargetRssi = 0;
int32_t s_scanTargetChannel = 0;
uint8_t s_scanTargetAuthMode = 0xFF;

int32_t s_lastDisconnectReason = -1;
int32_t s_assocChannel = -1;
uint8_t s_assocAuthMode = 0xFF;

wl_status_t s_lastStatus = WL_NO_SHIELD;

uint32_t s_minFreeHeapSeen = UINT32_MAX;

bool s_eventHandlerRegistered = false;
bool s_countryCodeApplied = false;

// ---- OTA breadcrumb state (RFC #146 stage 1) ----
// All in BSS; reset by noteOtaAttemptStart().
bool s_otaPreflightRecorded = false;
bool s_otaPreflightDnsOk = false;
bool s_otaPreflightTcpOk = false;
uint32_t s_otaPreflightIpV4 = 0;
uint32_t s_otaPreflightFreeHeap = 0;

bool s_otaCheckRecorded = false;
uint8_t s_otaCheckTag = 0;
int s_otaCheckHttpcCode = 0;
int s_otaCheckHttpStatus = 0;

bool s_otaInstallRecorded = false;
uint8_t s_otaInstallTag = 0;
int32_t s_otaInstallEspErr = 0;
const char* s_otaInstallEspErrName = nullptr;
uint32_t s_otaInstallBytesProcessed = 0;
uint32_t s_otaInstallBytesExpected = 0;

void recordTimeline(EntryKind kind, uint16_t code, int32_t aux) {
  if (s_timelineCount >= TIMELINE_CAPACITY) {
    return;  // drop — first frames are most informative
  }
  const uint32_t now = millis();
  TimelineEntry& e = s_timeline[s_timelineCount++];
  e.msOffset = s_attemptInProgress ? (now - s_attemptStartMs) : 0;
  e.kind = kind;
  e.code = code;
  e.aux = aux;
}

void updateMinHeap() {
  const uint32_t free = ESP.getFreeHeap();
  if (free < s_minFreeHeapSeen) {
    s_minFreeHeapSeen = free;
  }
}

const char* statusName(wl_status_t s) {
  switch (s) {
    case WL_NO_SHIELD:
      return "WL_NO_SHIELD";
    case WL_IDLE_STATUS:
      return "WL_IDLE_STATUS";
    case WL_NO_SSID_AVAIL:
      return "WL_NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED:
      return "WL_SCAN_COMPLETED";
    case WL_CONNECTED:
      return "WL_CONNECTED";
    case WL_CONNECT_FAILED:
      return "WL_CONNECT_FAILED";
    case WL_CONNECTION_LOST:
      return "WL_CONNECTION_LOST";
    case WL_DISCONNECTED:
      return "WL_DISCONNECTED";
    default:
      return "WL_UNKNOWN";
  }
}

const char* eventName(arduino_event_id_t id) {
  switch (id) {
    case ARDUINO_EVENT_WIFI_STA_START:
      return "STA_START";
    case ARDUINO_EVENT_WIFI_STA_STOP:
      return "STA_STOP";
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      return "STA_CONNECTED";
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      return "STA_DISCONNECTED";
    case ARDUINO_EVENT_WIFI_STA_AUTHMODE_CHANGE:
      return "STA_AUTHMODE_CHANGE";
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      return "STA_GOT_IP";
    case ARDUINO_EVENT_WIFI_STA_LOST_IP:
      return "STA_LOST_IP";
    case ARDUINO_EVENT_WIFI_SCAN_DONE:
      return "SCAN_DONE";
    default:
      return "OTHER_EVENT";
  }
}

const char* authModeName(uint8_t m) {
  switch (m) {
    case WIFI_AUTH_OPEN:
      return "OPEN";
    case WIFI_AUTH_WEP:
      return "WEP";
    case WIFI_AUTH_WPA_PSK:
      return "WPA_PSK";
    case WIFI_AUTH_WPA2_PSK:
      return "WPA2_PSK";
    case WIFI_AUTH_WPA_WPA2_PSK:
      return "WPA_WPA2_PSK";
    case WIFI_AUTH_WPA2_ENTERPRISE:
      return "WPA2_ENTERPRISE";
    case WIFI_AUTH_WPA3_PSK:
      return "WPA3_PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK:
      return "WPA2_WPA3_PSK";
    case WIFI_AUTH_WAPI_PSK:
      return "WAPI_PSK";
    default:
      return "UNKNOWN";
  }
}

// Subset of wifi_err_reason_t — common cases readers care about. The numeric
// reason is always written; this just adds a human label when known.
const char* disconnectReasonName(int32_t r) {
  switch (r) {
    case 1:
      return "UNSPECIFIED";
    case 2:
      return "AUTH_EXPIRE";
    case 3:
      return "AUTH_LEAVE";
    case 4:
      return "ASSOC_EXPIRE";
    case 5:
      return "ASSOC_TOOMANY";
    case 6:
      return "NOT_AUTHED";
    case 7:
      return "NOT_ASSOCED";
    case 8:
      return "ASSOC_LEAVE";
    case 15:
      return "4WAY_HANDSHAKE_TIMEOUT";
    case 16:
      return "GROUP_KEY_UPDATE_TIMEOUT";
    case 17:
      return "IE_IN_4WAY_DIFFERS";
    case 18:
      return "GROUP_CIPHER_INVALID";
    case 19:
      return "PAIRWISE_CIPHER_INVALID";
    case 20:
      return "AKMP_INVALID";
    case 23:
      return "IEEE_802_1X_AUTH_FAILED";
    case 24:
      return "CIPHER_SUITE_REJECTED";
    case 200:
      return "BEACON_TIMEOUT";
    case 201:
      return "NO_AP_FOUND";
    case 202:
      return "AUTH_FAIL";
    case 203:
      return "ASSOC_FAIL";
    case 204:
      return "HANDSHAKE_TIMEOUT";
    case 205:
      return "CONNECTION_FAIL";
    case 206:
      return "AP_TSF_RESET";
    case 207:
      return "ROAMING";
    default:
      return "UNKNOWN";
  }
}

// Names for the OTA outcome tags. Values mirror the corresponding enum
// declared in OtaUpdater.h; intentionally untyped here so this TU does not
// pull HTTPClient + esp_https_ota transitively. If the enum reorders the
// names list must be updated in lockstep — guarded by inline static_assert
// below the noteOta* setters.
const char* otaCheckTagName(uint8_t t) {
  switch (t) {
    case 0:
      return "UpdateAvailable";
    case 1:
      return "AlreadyLatest";
    case 2:
      return "NoFirmwareAsset";
    case 3:
      return "HttpClientError";
    case 4:
      return "HttpStatusError";
    case 5:
      return "RateLimited";
    case 6:
      return "JsonParseError";
    case 7:
      return "InternalError";
    default:
      return "?";
  }
}

const char* otaInstallTagName(uint8_t t) {
  switch (t) {
    case 0:
      return "Success";
    case 1:
      return "NotNewer";
    case 2:
      return "BeginFailed";
    case 3:
      return "PerformFailed";
    case 4:
      return "Incomplete";
    case 5:
      return "FinishFailed";
    default:
      return "?";
  }
}

// Best-effort plain-language hint for the most common failure shapes. Used
// only for the final RESULT line; never replaces the numeric reason code.
const char* failureHint(FailureKind kind, int32_t reason, bool sawConnected) {
  if (kind == FailureKind::NoSsidAvail || reason == 201) {
    return "AP not found in scan — wrong SSID, AP off, out of range, or channel 12/13 with wrong country code.";
  }
  switch (reason) {
    case 15:
    case 204:
      return "Handshake timeout — almost always wrong password, or PMF/WPA3 mismatch.";
    case 202:
      return "Auth fail — wrong password.";
    case 17:
    case 18:
    case 19:
    case 20:
      return "Cipher/AKMP mismatch — likely WPA3-only AP; ESP32-C3 needs WPA2-compat enabled on router.";
    case 200:
      return "Beacon timeout — RSSI too weak or AP went away mid-connect.";
    case 8:
    case 4:
    case 5:
      return "AP disassociated us — could be MAC filter, client cap reached, or band-steer.";
    case 203:
    case 205:
      return "Association failed — radio or AP rejection; check channel/regulatory.";
    default:
      break;
  }
  if (kind == FailureKind::Timeout) {
    return sawConnected ? "Reached STA_CONNECTED but never got IP — DHCP issue or AP not forwarding."
                        : "Never reached STA_CONNECTED in 15 s — RSSI/auth/regulatory; check reason codes above.";
  }
  if (kind == FailureKind::OtaCheckFailed) {
    return "OTA check failed before download. See preflight + check tag above.";
  }
  if (kind == FailureKind::OtaInstallFailed) {
    return "OTA install failed. See install tag + esp_err name above.";
  }
  return "See reason code above.";
}

void onWifiEvent(arduino_event_id_t event, arduino_event_info_t info) {
  updateMinHeap();
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      s_assocChannel = info.wifi_sta_connected.channel;
      s_assocAuthMode = info.wifi_sta_connected.authmode;
      recordTimeline(EntryKind::Event, static_cast<uint16_t>(event), s_assocChannel);
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      s_lastDisconnectReason = info.wifi_sta_disconnected.reason;
      recordTimeline(EntryKind::Event, static_cast<uint16_t>(event), s_lastDisconnectReason);
      break;
    case ARDUINO_EVENT_WIFI_STA_START:
    case ARDUINO_EVENT_WIFI_STA_STOP:
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
    case ARDUINO_EVENT_WIFI_STA_LOST_IP:
    case ARDUINO_EVENT_WIFI_STA_AUTHMODE_CHANGE:
      recordTimeline(EntryKind::Event, static_cast<uint16_t>(event), 0);
      break;
    default:
      break;
  }
}

void appendf(HalFile& f, const char* fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n > 0) {
    f.write(buf, static_cast<size_t>(n < static_cast<int>(sizeof(buf)) ? n : static_cast<int>(sizeof(buf)) - 1));
  }
}

}  // namespace

void begin() {
  if (s_eventHandlerRegistered) {
    return;
  }
  WiFi.onEvent(onWifiEvent);
  s_eventHandlerRegistered = true;
  s_minFreeHeapSeen = ESP.getFreeHeap();
  LOG_INF("WIFI_DIAG", "Wi-Fi diagnostic reporter armed");
}

void ensureCountryCodeApplied() {
  if (s_countryCodeApplied) {
    return;
  }
  // "01" = worldwide / generic, with 802.11d enabled so we adopt the AP's
  // advertised regulatory domain. ietf=true makes channels 12/13 usable when
  // the AP advertises a country IE permitting them. Safe default.
  const esp_err_t err = esp_wifi_set_country_code("01", true);
  if (err == ESP_OK) {
    s_countryCodeApplied = true;
    LOG_INF("WIFI_DIAG", "Country code applied: 01 (worldwide, 802.11d)");
  } else {
    LOG_ERR("WIFI_DIAG", "esp_wifi_set_country_code failed: %s", esp_err_to_name(err));
  }
}

void noteAttemptStart(size_t targetSsidLen, size_t savedCount, bool requiresPassword, bool usedSavedPassword,
                      bool autoConnecting) {
  s_timelineCount = 0;
  s_attemptStartMs = millis();
  s_attemptInProgress = true;
  s_targetSsidLen = targetSsidLen;
  s_savedCount = savedCount;
  s_targetRequiresPassword = requiresPassword;
  s_targetUsedSavedPassword = usedSavedPassword;
  s_targetAutoConnecting = autoConnecting;
  s_lastDisconnectReason = -1;
  s_assocChannel = -1;
  s_assocAuthMode = 0xFF;
  s_lastStatus = WL_NO_SHIELD;
  updateMinHeap();
}

void noteStatus(wl_status_t status) {
  updateMinHeap();
  if (status == s_lastStatus) {
    return;
  }
  s_lastStatus = status;
  recordTimeline(EntryKind::Status, static_cast<uint16_t>(status), 0);
}

void noteScanSummary(size_t totalNetworks, bool targetFound, int32_t targetRssi, int32_t targetChannel,
                     uint8_t targetAuthMode) {
  s_scanRecorded = true;
  s_scanTotal = totalNetworks;
  s_scanTargetFound = targetFound;
  s_scanTargetRssi = targetRssi;
  s_scanTargetChannel = targetChannel;
  s_scanTargetAuthMode = targetAuthMode;
}

void noteOtaAttemptStart() {
  s_otaPreflightRecorded = false;
  s_otaCheckRecorded = false;
  s_otaInstallRecorded = false;
  s_otaInstallEspErrName = nullptr;
  s_otaInstallEspErr = 0;
  s_otaInstallBytesProcessed = 0;
  s_otaInstallBytesExpected = 0;
  s_otaCheckHttpcCode = 0;
  s_otaCheckHttpStatus = 0;
  updateMinHeap();
}

void noteOtaPreflight(bool dnsOk, bool tcpOk, uint32_t resolvedIpV4, uint32_t freeHeapBytes) {
  s_otaPreflightRecorded = true;
  s_otaPreflightDnsOk = dnsOk;
  s_otaPreflightTcpOk = tcpOk;
  s_otaPreflightIpV4 = resolvedIpV4;
  s_otaPreflightFreeHeap = freeHeapBytes;
  updateMinHeap();
}

void noteOtaCheckResult(uint8_t tagAsByte, int httpcCode, int httpStatus) {
  s_otaCheckRecorded = true;
  s_otaCheckTag = tagAsByte;
  s_otaCheckHttpcCode = httpcCode;
  s_otaCheckHttpStatus = httpStatus;
  updateMinHeap();
}

void noteOtaInstallResult(uint8_t tagAsByte, int32_t espErr, const char* espErrName, uint32_t bytesProcessed,
                          uint32_t bytesExpected) {
  s_otaInstallRecorded = true;
  s_otaInstallTag = tagAsByte;
  s_otaInstallEspErr = espErr;
  s_otaInstallEspErrName = espErrName;
  s_otaInstallBytesProcessed = bytesProcessed;
  s_otaInstallBytesExpected = bytesExpected;
  updateMinHeap();
}

const char* shortFailureMessage(FailureKind kind) {
  const int32_t r = s_lastDisconnectReason;
  if (kind == FailureKind::NoSsidAvail || r == 201 /*NO_AP_FOUND*/) {
    return "Network not found — check it is on, in range, and 2.4GHz (not 5GHz-only).";
  }
  switch (r) {
    case 15:   // 4WAY_HANDSHAKE_TIMEOUT
    case 204:  // HANDSHAKE_TIMEOUT
    case 202:  // AUTH_FAIL
      return "Password rejected, or the router uses WPA3/PMF. Re-check the password; set the "
             "router to WPA2 or PMF-optional.";
    case 17:  // IE_IN_4WAY_DIFFERS
    case 18:  // GROUP_CIPHER_INVALID
    case 19:  // PAIRWISE_CIPHER_INVALID
    case 20:  // AKMP_INVALID
    case 24:  // CIPHER_SUITE_REJECTED
      return "Security mismatch (likely WPA3-only) — enable WPA2 compatibility on the router.";
    case 200:  // BEACON_TIMEOUT
      return "Signal too weak — the router dropped mid-connect. Move closer.";
    case 4:  // ASSOC_EXPIRE
    case 5:  // ASSOC_TOOMANY
    case 8:  // ASSOC_LEAVE
      return "The router disconnected the device — MAC filter, client limit, or band-steering.";
    default:
      break;
  }
  if (kind == FailureKind::Timeout) {
    return "No response from the router (weak signal, or connected but got no IP). See CRASH_INFO.TXT.";
  }
  return "Connection failed — see the WIFI section of CRASH_INFO.TXT for the reason code.";
}

void writeReportOnFailure(FailureKind kind) {
  s_attemptInProgress = false;
  const bool isOtaKind = (kind == FailureKind::OtaCheckFailed || kind == FailureKind::OtaInstallFailed);

  // Append a WIFI section to the consolidated /CRASH_INFO.TXT (was its own
  // overwriting /diag_report.txt). beginCrashSection writes the section header
  // and positions at end; endCrashSection closes + cap-trims.
  HalFile f;
  if (!crosspoint::diag::beginCrashSection("WIFI", f)) {
    LOG_ERR("WIFI_DIAG", "Failed to open crash-info for writing");
    return;
  }

  // Header
  appendf(f, "CrossPoint Diagnostic Report\n");
  appendf(f, "Firmware:        %s\n", CROSSPOINT_VERSION);
  appendf(f, "ESP-IDF:         %s\n", esp_get_idf_version());
  appendf(f, "Reset reason:    %d\n", static_cast<int>(esp_reset_reason()));
  appendf(f, "Free heap now:   %u bytes\n", static_cast<unsigned>(ESP.getFreeHeap()));
  appendf(f, "Min free heap:   %u bytes\n",
          static_cast<unsigned>(s_minFreeHeapSeen == UINT32_MAX ? 0 : s_minFreeHeapSeen));

  // Country code
  wifi_country_t country{};
  if (esp_wifi_get_country(&country) == ESP_OK) {
    char cc[4] = {0};
    cc[0] = country.cc[0] ? country.cc[0] : '?';
    cc[1] = country.cc[1] ? country.cc[1] : '?';
    cc[2] = country.cc[2] ? country.cc[2] : '\0';
    appendf(f, "Country code:    %s (ch %u..%u, policy=%d)\n", cc, country.schan,
            static_cast<unsigned>(country.schan + country.nchan - 1), static_cast<int>(country.policy));
  } else {
    appendf(f, "Country code:    <unavailable>\n");
  }

  appendf(f, "Saved networks:  %u\n", static_cast<unsigned>(s_savedCount));
  appendf(f, "\n");

  // Target (no SSID, length only)
  appendf(f, "Target network (sanitized):\n");
  appendf(f, "  SSID length:   %u    (value redacted)\n", static_cast<unsigned>(s_targetSsidLen));
  appendf(f, "  Encrypted:     %s\n", s_targetRequiresPassword ? "yes" : "no (open)");
  appendf(f, "  Saved cred:    %s\n", s_targetUsedSavedPassword ? "yes" : "no");
  appendf(f, "  Auto-connect:  %s\n", s_targetAutoConnecting ? "yes" : "no");

  if (s_scanRecorded) {
    appendf(f, "  Found in scan: %s\n", s_scanTargetFound ? "yes" : "no");
    if (s_scanTargetFound) {
      appendf(f, "  RSSI:          %d dBm\n", static_cast<int>(s_scanTargetRssi));
      appendf(f, "  Channel:       %d\n", static_cast<int>(s_scanTargetChannel));
      appendf(f, "  Auth mode:     %s (%u)\n", authModeName(s_scanTargetAuthMode),
              static_cast<unsigned>(s_scanTargetAuthMode));
    }
  } else {
    appendf(f, "  Scan not run this attempt (auto-connect path or skipped).\n");
  }
  appendf(f, "\n");

  if (s_scanRecorded) {
    appendf(f, "Scan summary:\n");
    appendf(f, "  Networks seen: %u\n", static_cast<unsigned>(s_scanTotal));
    appendf(f, "  (SSIDs and BSSIDs intentionally omitted)\n\n");
  }

  // Association detail captured from STA_CONNECTED event
  if (s_assocChannel >= 0) {
    appendf(f, "Association (from STA_CONNECTED):\n");
    appendf(f, "  Channel:       %d\n", static_cast<int>(s_assocChannel));
    appendf(f, "  Auth mode:     %s (%u)\n", authModeName(s_assocAuthMode), static_cast<unsigned>(s_assocAuthMode));
    appendf(f, "\n");
  }

  // Timeline
  appendf(f, "Connect timeline (ms since attempt start):\n");
  bool sawConnected = false;
  for (size_t i = 0; i < s_timelineCount; ++i) {
    const TimelineEntry& e = s_timeline[i];
    if (e.kind == EntryKind::Status) {
      appendf(f, "  %6u ms  STATUS  %s (%u)\n", static_cast<unsigned>(e.msOffset),
              statusName(static_cast<wl_status_t>(e.code)), static_cast<unsigned>(e.code));
    } else {
      const auto eventId = static_cast<arduino_event_id_t>(e.code);
      if (eventId == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
        sawConnected = true;
        appendf(f, "  %6u ms  EVENT   %s (channel=%d)\n", static_cast<unsigned>(e.msOffset), eventName(eventId),
                static_cast<int>(e.aux));
      } else if (eventId == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        appendf(f, "  %6u ms  EVENT   %s reason=%d (%s)\n", static_cast<unsigned>(e.msOffset), eventName(eventId),
                static_cast<int>(e.aux), disconnectReasonName(e.aux));
      } else {
        appendf(f, "  %6u ms  EVENT   %s\n", static_cast<unsigned>(e.msOffset), eventName(eventId));
      }
    }
  }
  if (s_timelineCount == 0) {
    appendf(f, "  (no timeline entries — attempt failed before any state change)\n");
  } else if (s_timelineCount >= TIMELINE_CAPACITY) {
    appendf(f, "  (timeline truncated at %u entries)\n", static_cast<unsigned>(TIMELINE_CAPACITY));
  }
  appendf(f, "\n");

  // ---- OTA section (only printed when an OTA attempt left breadcrumbs) ----
  if (s_otaPreflightRecorded || s_otaCheckRecorded || s_otaInstallRecorded) {
    appendf(f, "OTA breadcrumbs:\n");
    if (s_otaPreflightRecorded) {
      if (s_otaPreflightDnsOk) {
        appendf(f, "  Preflight:     dns=%u.%u.%u.%u tcp=%s heap=%u bytes\n", (s_otaPreflightIpV4 >> 24) & 0xFF,
                (s_otaPreflightIpV4 >> 16) & 0xFF, (s_otaPreflightIpV4 >> 8) & 0xFF, s_otaPreflightIpV4 & 0xFF,
                s_otaPreflightTcpOk ? "OK" : "FAIL", static_cast<unsigned>(s_otaPreflightFreeHeap));
      } else {
        appendf(f, "  Preflight:     dns=FAIL heap=%u bytes\n", static_cast<unsigned>(s_otaPreflightFreeHeap));
      }
    }
    if (s_otaCheckRecorded) {
      appendf(f, "  Check tag:     %u (%s)\n", static_cast<unsigned>(s_otaCheckTag), otaCheckTagName(s_otaCheckTag));
      if (s_otaCheckHttpcCode != 0) appendf(f, "  HTTPC code:    %d\n", s_otaCheckHttpcCode);
      if (s_otaCheckHttpStatus != 0) appendf(f, "  HTTP status:   %d\n", s_otaCheckHttpStatus);
    }
    if (s_otaInstallRecorded) {
      appendf(f, "  Install tag:   %u (%s)\n", static_cast<unsigned>(s_otaInstallTag),
              otaInstallTagName(s_otaInstallTag));
      if (s_otaInstallEspErrName != nullptr) {
        appendf(f, "  esp_err:       %s (%d)\n", s_otaInstallEspErrName, static_cast<int>(s_otaInstallEspErr));
      }
      if (s_otaInstallBytesExpected > 0) {
        appendf(f, "  Bytes:         %u / %u\n", static_cast<unsigned>(s_otaInstallBytesProcessed),
                static_cast<unsigned>(s_otaInstallBytesExpected));
      }
    }
    appendf(f, "\n");
  }

  // Result
  const char* kindName;
  switch (kind) {
    case FailureKind::Timeout:
      kindName = "TIMEOUT (15 s)";
      break;
    case FailureKind::NoSsidAvail:
      kindName = "NO_SSID_AVAIL";
      break;
    case FailureKind::OtaCheckFailed:
      kindName = "OTA_CHECK_FAILED";
      break;
    case FailureKind::OtaInstallFailed:
      kindName = "OTA_INSTALL_FAILED";
      break;
    case FailureKind::SilentRestart:
      kindName = "SILENT_RESTART (timeline capture)";
      break;
    case FailureKind::StatusFailed:
    default:
      kindName = "CONNECT_FAILED";
      break;
  }
  appendf(f, "Result:          %s\n", kindName);
  if (!isOtaKind) {
    appendf(f, "Last reason:     %d (%s)\n", static_cast<int>(s_lastDisconnectReason),
            disconnectReasonName(s_lastDisconnectReason));
  }
  appendf(f, "Hint:            %s\n", failureHint(kind, s_lastDisconnectReason, sawConnected));
  appendf(f, "End of report.\n");

  f.flush();
  crosspoint::diag::endCrashSection(f);
  LOG_INF("WIFI_DIAG", "Appended WIFI section (kind=%s, reason=%d)", kindName,
          static_cast<int>(s_lastDisconnectReason));
}

void captureForReboot() {
  // Only capture if a WiFi attempt left a timeline this boot. A non-WiFi silent
  // restart (e.g. reader OOM) must NOT clobber an earlier real failure report
  // with an empty one — so skip when nothing was recorded.
  if (s_timelineCount == 0) {
    return;
  }
  writeReportOnFailure(FailureKind::SilentRestart);
}

}  // namespace WifiDiagReport
