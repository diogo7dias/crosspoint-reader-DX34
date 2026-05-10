#include "OtaUpdater.h"

#include <HTTPClient.h>
#include <Logging.h>
#include <ReleaseJsonParser.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <cstdio>

#include "WifiDiagReport.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_wifi.h"

// Static contract between OtaUpdater's outcome tags and the literal-value
// switch tables in WifiDiagReport.cpp's report writer. If anyone reorders
// these enums the report will silently print wrong tag names — pin the
// numeric values here so the build fails instead.
static_assert(static_cast<uint8_t>(CheckOutcome::Tag::UpdateAvailable) == 0, "CheckOutcome::Tag values pinned");
static_assert(static_cast<uint8_t>(CheckOutcome::Tag::AlreadyLatest) == 1, "CheckOutcome::Tag values pinned");
static_assert(static_cast<uint8_t>(CheckOutcome::Tag::NoFirmwareAsset) == 2, "CheckOutcome::Tag values pinned");
static_assert(static_cast<uint8_t>(CheckOutcome::Tag::HttpClientError) == 3, "CheckOutcome::Tag values pinned");
static_assert(static_cast<uint8_t>(CheckOutcome::Tag::HttpStatusError) == 4, "CheckOutcome::Tag values pinned");
static_assert(static_cast<uint8_t>(CheckOutcome::Tag::RateLimited) == 5, "CheckOutcome::Tag values pinned");
static_assert(static_cast<uint8_t>(CheckOutcome::Tag::JsonParseError) == 6, "CheckOutcome::Tag values pinned");
static_assert(static_cast<uint8_t>(CheckOutcome::Tag::InternalError) == 7, "CheckOutcome::Tag values pinned");

static_assert(static_cast<uint8_t>(InstallOutcome::Tag::Success) == 0, "InstallOutcome::Tag values pinned");
static_assert(static_cast<uint8_t>(InstallOutcome::Tag::NotNewer) == 1, "InstallOutcome::Tag values pinned");
static_assert(static_cast<uint8_t>(InstallOutcome::Tag::BeginFailed) == 2, "InstallOutcome::Tag values pinned");
static_assert(static_cast<uint8_t>(InstallOutcome::Tag::PerformFailed) == 3, "InstallOutcome::Tag values pinned");
static_assert(static_cast<uint8_t>(InstallOutcome::Tag::Incomplete) == 4, "InstallOutcome::Tag values pinned");
static_assert(static_cast<uint8_t>(InstallOutcome::Tag::FinishFailed) == 5, "InstallOutcome::Tag values pinned");

namespace {
constexpr char latestReleaseUrl[] =
    "https://api.github.com/repos/diogo7dias/crosspoint-reader-DX34/releases/"
    "latest";

/* Find the start of a semver-like "N.N.N" substring, skipping noise like "DX34".
 * Scans for a digit whose run is immediately followed by '.digit', which
 * distinguishes "34-" (no dot) from "0.0.11" (real version). */
const char* findSemverStart(const char* str) {
  for (const char* p = str; *p; ++p) {
    if (isdigit(static_cast<unsigned char>(*p))) {
      const char* q = p;
      while (isdigit(static_cast<unsigned char>(*q))) q++;
      if (*q == '.' && isdigit(static_cast<unsigned char>(*(q + 1)))) return p;
      p = q;  // skip past this digit group
    }
  }
  return str;
}

// Pure helper. Returns true iff `latest` is strictly newer than `current`.
// RC builds (-rc suffix on current) treat equal-segment latest as newer so
// the device upgrades from a prerelease to its corresponding final.
bool isLatestNewer(const char* current, const char* latest) {
  if (latest == nullptr || latest[0] == '\0' || std::string(latest) == current) return false;

  int cM = 0, cm = 0, cp = 0, lM = 0, lm = 0, lp = 0;
  sscanf(findSemverStart(current), "%d.%d.%d", &cM, &cm, &cp);
  sscanf(findSemverStart(latest), "%d.%d.%d", &lM, &lm, &lp);
  if (lM != cM) return lM > cM;
  if (lm != cm) return lm > cm;
  if (lp != cp) return lp > cp;
  return strstr(current, "-rc") != nullptr;
}

esp_err_t http_client_set_header_cb(esp_http_client_handle_t http_client) {
  return esp_http_client_set_header(http_client, "User-Agent", "CrossPoint-Mod-DX34-ESP32-" CROSSPOINT_VERSION);
}

// RAII: on scope exit, push the final outcome into the diag report's OTA
// section. Lets us add breadcrumb recording without touching every early
// return inside checkForUpdate / installUpdate.
struct CheckExitGuard {
  const CheckOutcome& out;
  ~CheckExitGuard() {
    const int httpc = (out.tag == CheckOutcome::Tag::HttpClientError) ? out.u.httpcCode : 0;
    const int httpStatus =
        (out.tag == CheckOutcome::Tag::HttpStatusError || out.tag == CheckOutcome::Tag::RateLimited) ? out.u.httpStatus
                                                                                                     : 0;
    WifiDiagReport::noteOtaCheckResult(static_cast<uint8_t>(out.tag), httpc, httpStatus);
  }
};

struct InstallExitGuard {
  const InstallOutcome& out;
  ~InstallExitGuard() {
    WifiDiagReport::noteOtaInstallResult(static_cast<uint8_t>(out.tag), static_cast<int32_t>(out.espErr),
                                         out.espErrName, out.bytesProcessed, out.bytesExpected);
  }
};
}  // namespace

NetPreflight OtaUpdater::runPreflight() {
  NetPreflight pf;
  IPAddress resolved;
  const bool dnsOk = WiFi.hostByName("api.github.com", resolved);
  pf.dns = dnsOk ? NetPreflight::Dns::Ok : NetPreflight::Dns::Failed;
  if (dnsOk) {
    // Pack 4 octets into a uint32 (network byte order). Renderer can
    // unpack via byte shifts; storing as integer keeps NetPreflight POD.
    pf.resolvedIpV4 = (static_cast<uint32_t>(resolved[0]) << 24) | (static_cast<uint32_t>(resolved[1]) << 16) |
                      (static_cast<uint32_t>(resolved[2]) << 8) | static_cast<uint32_t>(resolved[3]);
    WiFiClient probe;
    probe.setTimeout(5);  // seconds
    pf.tcp = probe.connect(resolved, 443) ? NetPreflight::Tcp::Ok : NetPreflight::Tcp::Failed;
    probe.stop();
  }
  pf.freeHeapBytes = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
  return pf;
}

CheckOutcome OtaUpdater::checkForUpdate() {
  CheckOutcome out;
  CheckExitGuard guard{out};
  WifiDiagReport::noteOtaAttemptStart();
  out.preflight = runPreflight();
  WifiDiagReport::noteOtaPreflight(out.preflight.dns == NetPreflight::Dns::Ok,
                                   out.preflight.tcp == NetPreflight::Tcp::Ok, out.preflight.resolvedIpV4,
                                   out.preflight.freeHeapBytes);
  LOG_INF("OTA", "Pre-flight: dns=%s tcp=%s heap=%u",
          out.preflight.dns == NetPreflight::Dns::Ok ? "OK" : "FAIL",
          out.preflight.tcp == NetPreflight::Tcp::Ok    ? "OK"
          : out.preflight.tcp == NetPreflight::Tcp::Failed ? "FAIL"
                                                           : "SKIP",
          (unsigned)out.preflight.freeHeapBytes);

  // Use Arduino HTTPClient + WiFiClientSecure (with setInsecure) instead of
  // ESP-IDF's esp_http_client. This mirrors KOReaderSyncClient and is the
  // stack proven to handshake successfully against TLS hosts on the C3's
  // tight heap (~28 KB free). esp_http_client + mbedtls full handshake was
  // failing with ESP_ERR_HTTP_CONNECT after TCP connect succeeded, even with
  // no CA bundle attached — symptomatic of an OOM mid-handshake that the
  // ESP-IDF transport layer doesn't surface as ESP_ERR_NO_MEM.
  ReleaseJsonParser releaseParser;
  size_t totalBytesReceived = 0;
  LOG_DBG("OTA", "Checking for update (current: %s)", CROSSPOINT_VERSION);

  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient http;
  http.setUserAgent("CrossPoint-Mod-DX34-ESP32-" CROSSPOINT_VERSION);
  http.setTimeout(15000);
  // Allow GitHub's redirect path (api.github.com may issue 301/302 in some
  // configurations); FOLLOW_REDIRECTS_FORCED reuses the same secure client.
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

  if (!http.begin(secureClient, latestReleaseUrl)) {
    LOG_ERR("OTA", "HTTPClient::begin failed");
    out.tag = CheckOutcome::Tag::InternalError;
    return out;
  }

  const int statusCode = http.GET();
  if (statusCode < 0) {
    out.tag = CheckOutcome::Tag::HttpClientError;
    out.u.httpcCode = statusCode;
    LOG_ERR("OTA", "HTTPC %d: %s", statusCode, HTTPClient::errorToString(statusCode).c_str());
    http.end();
    return out;
  }

  if (statusCode == 403 || statusCode == 429) {
    LOG_ERR("OTA", "GitHub API rate limited (HTTP %d)", statusCode);
    out.tag = CheckOutcome::Tag::RateLimited;
    out.u.httpStatus = statusCode;
    http.end();
    return out;
  }
  if (statusCode != 200) {
    LOG_ERR("OTA", "Unexpected HTTP status %d from GitHub API", statusCode);
    out.tag = CheckOutcome::Tag::HttpStatusError;
    out.u.httpStatus = statusCode;
    http.end();
    return out;
  }

  // Stream the response body in 1 KB chunks into the SAX-style release parser.
  // Avoids buffering the full ~30 KB JSON in heap.
  WiFiClient* stream = http.getStreamPtr();
  if (stream == nullptr) {
    LOG_ERR("OTA", "HTTPClient stream is null");
    http.end();
    out.tag = CheckOutcome::Tag::InternalError;
    return out;
  }
  char chunk[1024];
  unsigned long lastDataMs = millis();
  while (http.connected() && (stream->available() > 0 || (millis() - lastDataMs) < 5000)) {
    int n = stream->read(reinterpret_cast<uint8_t*>(chunk), sizeof(chunk));
    if (n > 0) {
      releaseParser.feed(chunk, n);
      totalBytesReceived += n;
      lastDataMs = millis();
    } else {
      delay(10);
      if (stream->available() <= 0 && !http.connected()) break;
    }
  }
  http.end();

  LOG_DBG("OTA", "Response received: %zu bytes total", totalBytesReceived);

  if (!releaseParser.foundTag()) {
    LOG_ERR("OTA", "No tag_name in release JSON");
    out.tag = CheckOutcome::Tag::JsonParseError;
    return out;
  }

  if (!releaseParser.foundFirmware()) {
    LOG_ERR("OTA", "No firmware.bin asset found");
    out.tag = CheckOutcome::Tag::NoFirmwareAsset;
    out.latestVersion = releaseParser.getTagName();
    return out;
  }

  out.latestVersion = releaseParser.getTagName();
  if (!isLatestNewer(CROSSPOINT_VERSION, out.latestVersion.c_str())) {
    LOG_DBG("OTA", "Latest tag %s is not newer than current %s", out.latestVersion.c_str(), CROSSPOINT_VERSION);
    out.tag = CheckOutcome::Tag::AlreadyLatest;
    return out;
  }

  pending_.has = true;
  pending_.version = out.latestVersion;
  pending_.url = releaseParser.getFirmwareUrl();
  pending_.size = releaseParser.getFirmwareSize();

  out.tag = CheckOutcome::Tag::UpdateAvailable;
  out.u.latestSize = pending_.size;
  LOG_DBG("OTA", "Found update: tag=%s size=%u", out.latestVersion.c_str(), (unsigned)pending_.size);
  return out;
}

InstallOutcome OtaUpdater::installUpdate(ProgressFn onProgress) {
  InstallOutcome out;
  InstallExitGuard guard{out};
  if (!pending_.has) {
    out.tag = InstallOutcome::Tag::NotNewer;
    return out;
  }
  out.bytesExpected = pending_.size;

  esp_https_ota_handle_t ota_handle = nullptr;

  // No CA bundle attached: same stance as checkForUpdate above. The OTA
  // download URL redirects (releases/download → objects.githubusercontent.com)
  // so RX buffer stays at 8 KB to fit redirect headers cleanly.
  esp_http_client_config_t client_config = {
      .url = pending_.url.c_str(),
      .timeout_ms = 15000,
      /* Default HTTP client buffer size 512 byte only
       * not sufficent to handle URL redirection cases or
       * parsing of large HTTP headers.
       */
      .buffer_size = 8192,
      .buffer_size_tx = 8192,
      .skip_cert_common_name_check = true,
      .keep_alive_enable = true,
  };

  esp_https_ota_config_t ota_config = {
      .http_config = &client_config,
      .http_client_init_cb = http_client_set_header_cb,
  };

  /* For better timing and connectivity, we disable power saving for WiFi */
  esp_wifi_set_ps(WIFI_PS_NONE);

  esp_err_t esp_err = esp_https_ota_begin(&ota_config, &ota_handle);
  if (esp_err != ESP_OK) {
    out.tag = InstallOutcome::Tag::BeginFailed;
    out.espErr = esp_err;
    out.espErrName = esp_err_to_name(esp_err);
    LOG_ERR("OTA", "HTTP OTA Begin Failed: %s", out.espErrName);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    return out;
  }

  do {
    esp_err = esp_https_ota_perform(ota_handle);
    out.bytesProcessed = esp_https_ota_get_image_len_read(ota_handle);
    if (onProgress) onProgress(InstallProgress{out.bytesProcessed, out.bytesExpected});
    delay(100);  // Yield time for the UI render loop between OTA chunks
  } while (esp_err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

  /* Return back to default power saving for WiFi in case of failing */
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  if (esp_err != ESP_OK) {
    out.tag = InstallOutcome::Tag::PerformFailed;
    out.espErr = esp_err;
    out.espErrName = esp_err_to_name(esp_err);
    LOG_ERR("OTA", "esp_https_ota_perform Failed: %s", out.espErrName);
    esp_https_ota_finish(ota_handle);
    return out;
  }

  if (!esp_https_ota_is_complete_data_received(ota_handle)) {
    // The perform-loop exited with ESP_OK, so esp_err is not a meaningful
    // signal here. Surface processed/expected bytes — that's the actual gap.
    LOG_ERR("OTA", "Download incomplete: %u / %u bytes", (unsigned)out.bytesProcessed, (unsigned)out.bytesExpected);
    out.tag = InstallOutcome::Tag::Incomplete;
    esp_https_ota_finish(ota_handle);
    return out;
  }

  esp_err = esp_https_ota_finish(ota_handle);
  if (esp_err != ESP_OK) {
    out.tag = InstallOutcome::Tag::FinishFailed;
    out.espErr = esp_err;
    out.espErrName = esp_err_to_name(esp_err);
    LOG_ERR("OTA", "esp_https_ota_finish Failed: %s", out.espErrName);
    return out;
  }

  LOG_INF("OTA", "Update completed");
  out.tag = InstallOutcome::Tag::Success;
  return out;
}
