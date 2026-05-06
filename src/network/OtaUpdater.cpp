#include "OtaUpdater.h"

#include <Logging.h>
#include <ReleaseJsonParser.h>

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

/*
 * When esp_crt_bundle.h included, it is pointing wrong header file
 * which is something under WifiClientSecure because of our framework based on
 * arduno platform. To manage this obstacle, don't include anything, just extern
 * and it will point correct one.
 */
extern "C" {
extern esp_err_t esp_crt_bundle_attach(void* conf);
}

esp_err_t http_client_set_header_cb(esp_http_client_handle_t http_client) {
  return esp_http_client_set_header(http_client, "User-Agent", "CrossPoint-Mod-DX34-ESP32-" CROSSPOINT_VERSION);
}

size_t totalBytesReceived = 0;

esp_err_t event_handler(esp_http_client_event_t* event) {
  if (event->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
  totalBytesReceived += event->data_len;
  auto* parser = static_cast<ReleaseJsonParser*>(event->user_data);
  parser->feed(static_cast<const char*>(event->data), event->data_len);
  return ESP_OK;
}
}  // namespace

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
  esp_err_t esp_err;
  ReleaseJsonParser releaseParser;

  esp_http_client_config_t client_config = {
      .url = latestReleaseUrl,
      .event_handler = event_handler,
      .buffer_size = 8192,
      .buffer_size_tx = 8192,
      .user_data = &releaseParser,
      .skip_cert_common_name_check = true,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .keep_alive_enable = true,
  };

  totalBytesReceived = 0;
  LOG_DBG("OTA", "Checking for update (current: %s)", CROSSPOINT_VERSION);

  esp_http_client_handle_t client_handle = esp_http_client_init(&client_config);
  if (!client_handle) {
    LOG_ERR("OTA", "HTTP Client Handle Failed");
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_http_client_set_header(client_handle, "User-Agent", "CrossPoint-Mod-DX34-ESP32-" CROSSPOINT_VERSION);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_http_client_set_header Failed : %s", esp_err_to_name(esp_err));
    esp_http_client_cleanup(client_handle);
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_http_client_perform(client_handle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_http_client_perform Failed : %s", esp_err_to_name(esp_err));
    esp_http_client_cleanup(client_handle);
    return HTTP_ERROR;
  }

  int statusCode = esp_http_client_get_status_code(client_handle);

  /* esp_http_client_close will be called inside cleanup as well*/
  esp_err = esp_http_client_cleanup(client_handle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_http_client_cleanup Failed : %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  if (statusCode == 403 || statusCode == 429) {
    LOG_ERR("OTA", "GitHub API rate limited (HTTP %d)", statusCode);
    return RATE_LIMITED;
  }
  if (statusCode != 200) {
    LOG_ERR("OTA", "Unexpected HTTP status %d from GitHub API", statusCode);
    return HTTP_ERROR;
  }

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
      .crt_bundle_attach = esp_crt_bundle_attach,
      .keep_alive_enable = true,
  };

  esp_https_ota_config_t ota_config = {
      .http_config = &client_config,
      .http_client_init_cb = http_client_set_header_cb,
  };

  /* For better timing and connectivity, we disable power saving for WiFi */
  esp_wifi_set_ps(WIFI_PS_NONE);

  esp_err = esp_https_ota_begin(&ota_config, &ota_handle);
  if (esp_err != ESP_OK) {
    LOG_DBG("OTA", "HTTP OTA Begin Failed: %s", esp_err_to_name(esp_err));
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    return INTERNAL_UPDATE_ERROR;
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
    LOG_ERR("OTA", "esp_https_ota_perform Failed: %s", esp_err_to_name(esp_err));
    esp_https_ota_finish(ota_handle);
    return HTTP_ERROR;
  }

  if (!esp_https_ota_is_complete_data_received(ota_handle)) {
    LOG_ERR("OTA", "esp_https_ota_is_complete_data_received Failed: %s", esp_err_to_name(esp_err));
    esp_https_ota_finish(ota_handle);
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_https_ota_finish(ota_handle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_https_ota_finish Failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  LOG_INF("OTA", "Update completed");
  return OK;
}
