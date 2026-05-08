#include "OtaUpdater.h"

#include <HTTPClient.h>
#include <Logging.h>
#include <ReleaseJsonParser.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <cstdio>
#include <memory>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_wifi.h"

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

esp_err_t http_client_set_header_cb(esp_http_client_handle_t http_client) {
  return esp_http_client_set_header(http_client, "User-Agent", "CrossPoint-Mod-DX34-ESP32-" CROSSPOINT_VERSION);
}

size_t totalBytesReceived = 0;
}  // namespace

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
  esp_err_t esp_err;
  ReleaseJsonParser releaseParser;

  // Diagnostic: pre-flight DNS + plain-TCP probe + heap snapshot. Surfaces on
  // the failure screen so the user can self-report without serial. The TCP
  // probe distinguishes "TCP blocked / unroutable" from "TLS handshake fail":
  //   - tcp=OK  → TCP connect works, failure is in TLS handshake
  //   - tcp=FAIL → blocked at TCP layer (firewall, port closed, captive portal)
  static char diagBuf[120];
  IPAddress resolved;
  bool dnsOk = WiFi.hostByName("api.github.com", resolved);
  bool tcpOk = false;
  if (dnsOk) {
    WiFiClient probe;
    probe.setTimeout(5);  // seconds
    tcpOk = probe.connect(resolved, 443);
    probe.stop();
  }
  size_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
  if (dnsOk) {
    std::snprintf(diagBuf, sizeof(diagBuf), "dns=%s tcp=%s heap=%u", resolved.toString().c_str(),
                  tcpOk ? "OK" : "FAIL", freeHeap);
  } else {
    std::snprintf(diagBuf, sizeof(diagBuf), "dns=FAIL heap=%u", freeHeap);
  }
  preflightDiag = diagBuf;
  LOG_INF("OTA", "Pre-flight: %s", diagBuf);

  // Use Arduino HTTPClient + WiFiClientSecure (with setInsecure) instead of
  // ESP-IDF's esp_http_client. This mirrors KOReaderSyncClient and is the
  // stack proven to handshake successfully against TLS hosts on the C3's
  // tight heap (~28 KB free). esp_http_client + mbedtls full handshake was
  // failing with ESP_ERR_HTTP_CONNECT after TCP connect succeeded, even with
  // no CA bundle attached — symptomatic of an OOM mid-handshake that the
  // ESP-IDF transport layer doesn't surface as ESP_ERR_NO_MEM.
  totalBytesReceived = 0;
  lastEspErrName = nullptr;
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
    return INTERNAL_UPDATE_ERROR;
  }

  const int statusCode = http.GET();
  if (statusCode < 0) {
    // Arduino HTTPClient negative codes describe the failure mode (e.g.
    // HTTPC_ERROR_CONNECTION_REFUSED, _SEND_HEADER_FAILED, _READ_TIMEOUT).
    // Stash a static-buffered description so the failure screen can show it.
    static char httpErrBuf[64];
    const String desc = HTTPClient::errorToString(statusCode);
    std::snprintf(httpErrBuf, sizeof(httpErrBuf), "HTTPC %d: %s", statusCode, desc.c_str());
    lastEspErrName = httpErrBuf;
    LOG_ERR("OTA", "%s", httpErrBuf);
    http.end();
    return HTTP_ERROR;
  }

  if (statusCode == 403 || statusCode == 429) {
    LOG_ERR("OTA", "GitHub API rate limited (HTTP %d)", statusCode);
    http.end();
    return RATE_LIMITED;
  }
  if (statusCode != 200) {
    LOG_ERR("OTA", "Unexpected HTTP status %d from GitHub API", statusCode);
    http.end();
    return HTTP_ERROR;
  }

  // Stream the response body in 1 KB chunks into the SAX-style release parser.
  // Avoids buffering the full ~30 KB JSON in heap.
  WiFiClient* stream = http.getStreamPtr();
  if (stream == nullptr) {
    LOG_ERR("OTA", "HTTPClient stream is null");
    http.end();
    return INTERNAL_UPDATE_ERROR;
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
    return JSON_PARSE_ERROR;
  }

  if (!releaseParser.foundFirmware()) {
    LOG_ERR("OTA", "No firmware.bin asset found");
    return NO_UPDATE;
  }

  latestVersion = releaseParser.getTagName();
  otaUrl = releaseParser.getFirmwareUrl();
  otaSize = releaseParser.getFirmwareSize();
  totalSize = otaSize;
  updateAvailable = true;

  LOG_DBG("OTA", "Found update: tag=%s size=%zu", latestVersion.c_str(), otaSize);
  return OK;
}

bool OtaUpdater::isUpdateNewer() const {
  if (!updateAvailable || latestVersion.empty() || latestVersion == CROSSPOINT_VERSION) {
    return false;
  }

  int currentMajor = 0, currentMinor = 0, currentPatch = 0;
  int latestMajor = 0, latestMinor = 0, latestPatch = 0;

  const auto currentVersion = CROSSPOINT_VERSION;

  const char* latestDigits = findSemverStart(latestVersion.c_str());
  const char* currentDigits = findSemverStart(currentVersion);

  sscanf(latestDigits, "%d.%d.%d", &latestMajor, &latestMinor, &latestPatch);
  sscanf(currentDigits, "%d.%d.%d", &currentMajor, &currentMinor, &currentPatch);

  /*
   * Compare major versions.
   * If they differ, return true if latest major version greater than current
   * major version otherwise return false.
   */
  if (latestMajor != currentMajor) return latestMajor > currentMajor;

  /*
   * Compare minor versions.
   * If they differ, return true if latest minor version greater than current
   * minor version otherwise return false.
   */
  if (latestMinor != currentMinor) return latestMinor > currentMinor;

  /*
   * Check patch versions.
   */
  if (latestPatch != currentPatch) return latestPatch > currentPatch;

  // If we reach here, it means all segments are equal.
  // One final check, if we're on an RC build (contains "-rc"), we should
  // consider the latest version as newer even if the segments are equal, since
  // RC builds are pre-release versions.
  if (strstr(currentVersion, "-rc") != nullptr) {
    return true;
  }

  return false;
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(std::function<void()> onProgress) {
  if (!isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }

  esp_https_ota_handle_t ota_handle = NULL;
  esp_err_t esp_err;
  /* Signal for OtaUpdateActivity */
  render = false;

  // No CA bundle attached: same stance as checkForUpdate above. The OTA
  // download URL redirects (releases/download → objects.githubusercontent.com)
  // so RX buffer stays at 8 KB to fit redirect headers cleanly.
  esp_http_client_config_t client_config = {
      .url = otaUrl.c_str(),
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

  lastEspErrName = nullptr;
  esp_err = esp_https_ota_begin(&ota_config, &ota_handle);
  if (esp_err != ESP_OK) {
    lastEspErrName = esp_err_to_name(esp_err);
    LOG_ERR("OTA", "HTTP OTA Begin Failed: %s", lastEspErrName);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    return OTA_BEGIN_ERROR;
  }

  do {
    esp_err = esp_https_ota_perform(ota_handle);
    processedSize = esp_https_ota_get_image_len_read(ota_handle);
    /* Sent signal to  OtaUpdateActivity */
    render = true;
    if (onProgress) onProgress();
    delay(100);  // Yield time for the UI render loop between OTA chunks
  } while (esp_err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

  /* Return back to default power saving for WiFi in case of failing */
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  if (esp_err != ESP_OK) {
    lastEspErrName = esp_err_to_name(esp_err);
    LOG_ERR("OTA", "esp_https_ota_perform Failed: %s", lastEspErrName);
    esp_https_ota_finish(ota_handle);
    return HTTP_ERROR;
  }

  if (!esp_https_ota_is_complete_data_received(ota_handle)) {
    // The perform-loop exited with ESP_OK, so esp_err here is not a meaningful
    // signal. Surface processed/total bytes instead — that's the actual gap.
    LOG_ERR("OTA", "Download incomplete: %zu / %zu bytes", processedSize, totalSize);
    esp_https_ota_finish(ota_handle);
    return OTA_INCOMPLETE_ERROR;
  }

  esp_err = esp_https_ota_finish(ota_handle);
  if (esp_err != ESP_OK) {
    lastEspErrName = esp_err_to_name(esp_err);
    LOG_ERR("OTA", "esp_https_ota_finish Failed: %s", lastEspErrName);
    return OTA_FINISH_ERROR;
  }

  LOG_INF("OTA", "Update completed");
  return OK;
}
