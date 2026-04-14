#include "OtaUpdater.h"

#include <ArduinoJson.h>
#include <Logging.h>

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
const char *findSemverStart(const char *str) {
  for (const char *p = str; *p; ++p) {
    if (isdigit(static_cast<unsigned char>(*p))) {
      const char *q = p;
      while (isdigit(static_cast<unsigned char>(*q)))
        q++;
      if (*q == '.' && isdigit(static_cast<unsigned char>(*(q + 1))))
        return p;
      p = q;  // skip past this digit group
    }
  }
  return str;
}

/* Buffer and length tracker for incremental HTTP response from latestReleaseUrl.
 * Static storage is zero-initialized by C++, but explicit init makes intent clear
 * and guards against future refactors that might move these to non-static scope. */
char *local_buf = nullptr;
int output_len = 0;

/*
 * When esp_crt_bundle.h included, it is pointing wrong header file
 * which is something under WifiClientSecure because of our framework based on
 * arduno platform. To manage this obstacle, don't include anything, just extern
 * and it will point correct one.
 */
extern "C" {
extern esp_err_t esp_crt_bundle_attach(void *conf);
}

esp_err_t http_client_set_header_cb(esp_http_client_handle_t http_client) {
  return esp_http_client_set_header(
      http_client, "User-Agent",
      "CrossPoint-Mod-DX34-ESP32-" CROSSPOINT_VERSION);
}

esp_err_t event_handler(esp_http_client_event_t *event) {
  /* We do interested in only HTTP_EVENT_ON_DATA event only */
  if (event->event_id != HTTP_EVENT_ON_DATA)
    return ESP_OK;

  if (!esp_http_client_is_chunked_response(event->client)) {
    int content_len = esp_http_client_get_content_length(event->client);
    int copy_len = 0;

    if (local_buf == NULL) {
      /* Guard against bogus or malicious content-length values */
      if (content_len <= 0 || content_len > 32768) {
        LOG_ERR("OTA", "Rejecting content_len %d (max 32768)", content_len);
        return ESP_ERR_NO_MEM;
      }
      /* local_buf life span is tracked by caller checkForUpdate */
      local_buf = static_cast<char *>(calloc(content_len + 1, sizeof(char)));
      output_len = 0;
      if (local_buf == NULL) {
        LOG_ERR("OTA", "HTTP Client Out of Memory Failed, Allocation %d",
                content_len);
        return ESP_ERR_NO_MEM;
      }
    }
    copy_len = min(event->data_len, (content_len - output_len));
    if (copy_len) {
      memcpy(local_buf + output_len, event->data, copy_len);
    }
    output_len += copy_len;
  } else {
    /* Code might be hits here, It happened once (for version checking) but I
     * need more logs to handle that */
    int chunked_len;
    esp_http_client_get_chunk_length(event->client, &chunked_len);
    LOG_DBG("OTA",
            "esp_http_client_is_chunked_response failed, chunked_len: %d",
            chunked_len);
  }

  return ESP_OK;
} /* event_handler */
} /* namespace */

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
  JsonDocument filter;
  esp_err_t esp_err;
  JsonDocument doc;

  esp_http_client_config_t client_config = {
      .url = latestReleaseUrl,
      .event_handler = event_handler,
      /* Default HTTP client buffer size 512 byte only */
      .buffer_size = 8192,
      .buffer_size_tx = 8192,
      .skip_cert_common_name_check = true,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .keep_alive_enable = true,
  };

  /* To track life time of local_buf, dtor will be called on exit from that
   * function */
  struct localBufCleaner {
    char **bufPtr;
    ~localBufCleaner() {
      if (*bufPtr) {
        free(*bufPtr);
        *bufPtr = NULL;
      }
    }
  } localBufCleaner = {&local_buf};

  esp_http_client_handle_t client_handle = esp_http_client_init(&client_config);
  if (!client_handle) {
    LOG_ERR("OTA", "HTTP Client Handle Failed");
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_http_client_set_header(
      client_handle, "User-Agent",
      "CrossPoint-Mod-DX34-ESP32-" CROSSPOINT_VERSION);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_http_client_set_header Failed : %s",
            esp_err_to_name(esp_err));
    esp_http_client_cleanup(client_handle);
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_http_client_perform(client_handle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_http_client_perform Failed : %s",
            esp_err_to_name(esp_err));
    esp_http_client_cleanup(client_handle);
    return HTTP_ERROR;
  }

  int statusCode = esp_http_client_get_status_code(client_handle);

  /* esp_http_client_close will be called inside cleanup as well*/
  esp_err = esp_http_client_cleanup(client_handle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_http_client_cleanup Failed : %s",
            esp_err_to_name(esp_err));
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

  filter["tag_name"] = true;
  filter["assets"][0]["name"] = true;
  filter["assets"][0]["browser_download_url"] = true;
  filter["assets"][0]["size"] = true;
  const DeserializationError error =
      deserializeJson(doc, local_buf, DeserializationOption::Filter(filter));
  if (error) {
    LOG_ERR("OTA", "JSON parse failed: %s", error.c_str());
    return JSON_PARSE_ERROR;
  }

  if (!doc["tag_name"].is<std::string>()) {
    LOG_ERR("OTA", "No tag_name found");
    return JSON_PARSE_ERROR;
  }

  if (!doc["assets"].is<JsonArray>()) {
    LOG_ERR("OTA", "No assets found");
    return JSON_PARSE_ERROR;
  }

  latestVersion = doc["tag_name"].as<std::string>();

  for (int i = 0; i < doc["assets"].size(); i++) {
    if (doc["assets"][i]["name"] == "firmware.bin") {
      if (!doc["assets"][i]["browser_download_url"].is<std::string>() ||
          !doc["assets"][i]["size"].is<size_t>()) {
        LOG_ERR("OTA", "firmware.bin asset missing url or size fields");
        return JSON_PARSE_ERROR;
      }
      otaUrl = doc["assets"][i]["browser_download_url"].as<std::string>();
      otaSize = doc["assets"][i]["size"].as<size_t>();
      totalSize = otaSize;
      updateAvailable = true;
      break;
    }
  }

  if (!updateAvailable) {
    LOG_ERR("OTA", "No firmware.bin asset found");
    return NO_UPDATE;
  }

  LOG_DBG("OTA", "Found update: %s", latestVersion.c_str());
  return OK;
}

bool OtaUpdater::isUpdateNewer() const {
  if (!updateAvailable || latestVersion.empty() ||
      latestVersion == CROSSPOINT_VERSION) {
    return false;
  }

  int currentMajor = 0, currentMinor = 0, currentPatch = 0;
  int latestMajor = 0, latestMinor = 0, latestPatch = 0;

  const auto currentVersion = CROSSPOINT_VERSION;

  const char *latestDigits = findSemverStart(latestVersion.c_str());
  const char *currentDigits = findSemverStart(currentVersion);

  sscanf(latestDigits, "%d.%d.%d", &latestMajor, &latestMinor, &latestPatch);
  sscanf(currentDigits, "%d.%d.%d", &currentMajor, &currentMinor,
         &currentPatch);

  /*
   * Compare major versions.
   * If they differ, return true if latest major version greater than current
   * major version otherwise return false.
   */
  if (latestMajor != currentMajor)
    return latestMajor > currentMajor;

  /*
   * Compare minor versions.
   * If they differ, return true if latest minor version greater than current
   * minor version otherwise return false.
   */
  if (latestMinor != currentMinor)
    return latestMinor > currentMinor;

  /*
   * Check patch versions.
   */
  if (latestPatch != currentPatch)
    return latestPatch > currentPatch;

  // If we reach here, it means all segments are equal.
  // One final check, if we're on an RC build (contains "-rc"), we should
  // consider the latest version as newer even if the segments are equal, since
  // RC builds are pre-release versions.
  if (strstr(currentVersion, "-rc") != nullptr) {
    return true;
  }

  return false;
}

const std::string &OtaUpdater::getLatestVersion() const {
  return latestVersion;
}

OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate() {
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
    return INTERNAL_UPDATE_ERROR;
  }

  do {
    esp_err = esp_https_ota_perform(ota_handle);
    processedSize = esp_https_ota_get_image_len_read(ota_handle);
    /* Sent signal to  OtaUpdateActivity */
    render = true;
    delay(100);  // Yield time for the UI render loop between OTA chunks
  } while (esp_err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

  /* Return back to default power saving for WiFi in case of failing */
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_https_ota_perform Failed: %s",
            esp_err_to_name(esp_err));
    esp_https_ota_finish(ota_handle);
    return HTTP_ERROR;
  }

  if (!esp_https_ota_is_complete_data_received(ota_handle)) {
    LOG_ERR("OTA", "esp_https_ota_is_complete_data_received Failed: %s",
            esp_err_to_name(esp_err));
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
