#include "HttpDownloader.h"

#include <HTTPClient.h>
#include <Logging.h>
#include <StreamString.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <base64.h>

#include <cstring>
#include <memory>
#include <new>

#include "CrossPointSettings.h"
#include "util/UrlUtils.h"

std::unique_ptr<WiFiClient> HttpDownloader::createClient(const std::string& url, HTTPClient& http) {
  // Use WiFiClientSecure for HTTPS, regular WiFiClient for HTTP.
  // Bare `new` aborts under -fno-exceptions (bad_alloc -> std::terminate ->
  // abort). The explicit nothrow placement form is the only safe way to
  // recover from OOM here.
  std::unique_ptr<WiFiClient> client;
  if (UrlUtils::isHttpsUrl(url)) {
    auto* secureClient = new (std::nothrow) WiFiClientSecure();
    if (!secureClient) {
      LOG_ERR("HTTP", "OOM: failed to allocate WiFiClientSecure");
      return nullptr;
    }
    secureClient->setInsecure();
    client.reset(secureClient);
  } else {
    auto* plainClient = new (std::nothrow) WiFiClient();
    if (!plainClient) {
      LOG_ERR("HTTP", "OOM: failed to allocate WiFiClient");
      return nullptr;
    }
    client.reset(plainClient);
  }

  http.begin(*client, url.c_str());
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", "CrossPoint-Mod-DX34-ESP32-" CROSSPOINT_VERSION);

  // Add Basic HTTP auth if credentials are configured
  if (strlen(SETTINGS.opdsUsername) > 0 && strlen(SETTINGS.opdsPassword) > 0) {
    std::string credentials = std::string(SETTINGS.opdsUsername) + ":" + SETTINGS.opdsPassword;
    String encoded = base64::encode(credentials.c_str());
    http.addHeader("Authorization", "Basic " + encoded);
  }

  return client;
}

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent) {
  HTTPClient http;
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());

  auto client = createClient(url, http);
  if (!client) return false;

  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    LOG_ERR("HTTP", "Fetch failed: %d", httpCode);
    http.end();
    return false;
  }

  http.writeToStream(&outContent);

  http.end();

  LOG_DBG("HTTP", "Fetch success");
  return true;
}

bool HttpDownloader::fetchUrl(const std::string& url, std::string& outContent) {
  StreamString stream;
  if (!fetchUrl(url, stream)) {
    return false;
  }
  outContent = stream.c_str();
  return true;
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress) {
  HTTPClient http;
  LOG_DBG("HTTP", "Downloading: %s", url.c_str());
  LOG_DBG("HTTP", "Destination: %s", destPath.c_str());

  auto client = createClient(url, http);
  if (!client) return DownloadError::OutOfMemory;

  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    LOG_ERR("HTTP", "Download failed: %d", httpCode);
    http.end();
    return HTTP_ERROR;
  }

  const size_t contentLength = http.getSize();
  LOG_DBG("HTTP", "Content-Length: %zu", contentLength);

  // Reject absurdly large downloads that would exhaust SD or run forever.
  // 100 MB is generous for any supported file type (epub, bmp, xtc).
  constexpr size_t kMaxDownloadSize = 100 * 1024 * 1024;
  if (contentLength > kMaxDownloadSize) {
    LOG_ERR("HTTP", "Content-Length %zu exceeds max %zu", contentLength, kMaxDownloadSize);
    http.end();
    return HTTP_ERROR;
  }

  // Remove existing file if present
  if (Storage.exists(destPath.c_str())) {
    Storage.remove(destPath.c_str());
  }

  // Open file for writing
  FsFile file;
  if (!Storage.openFileForWrite("HTTP", destPath.c_str(), file)) {
    LOG_ERR("HTTP", "Failed to open file for writing");
    http.end();
    return FILE_ERROR;
  }

  // Get the stream for chunked reading
  WiFiClient* stream = http.getStreamPtr();
  if (!stream) {
    LOG_ERR("HTTP", "Failed to get stream");
    file.close();
    Storage.remove(destPath.c_str());
    http.end();
    return HTTP_ERROR;
  }

  // Download in chunks
  uint8_t buffer[DOWNLOAD_CHUNK_SIZE];
  size_t downloaded = 0;
  const size_t total = contentLength > 0 ? contentLength : 0;
  constexpr unsigned long STREAM_TIMEOUT_MS = 30000;
  unsigned long lastDataTime = millis();

  while (http.connected() && (contentLength == 0 || downloaded < contentLength)) {
    const size_t available = stream->available();
    if (available == 0) {
      if (millis() - lastDataTime > STREAM_TIMEOUT_MS) {
        LOG_ERR("HTTP", "Stream timeout after %zu bytes", downloaded);
        break;
      }
      delay(1);
      continue;
    }
    lastDataTime = millis();

    const size_t toRead = available < DOWNLOAD_CHUNK_SIZE ? available : DOWNLOAD_CHUNK_SIZE;
    const size_t bytesRead = stream->readBytes(buffer, toRead);

    if (bytesRead == 0) {
      break;
    }

    const size_t written = file.write(buffer, bytesRead);
    if (written != bytesRead) {
      LOG_ERR("HTTP", "Write failed: wrote %zu of %zu bytes", written, bytesRead);
      file.close();
      Storage.remove(destPath.c_str());
      http.end();
      return FILE_ERROR;
    }

    downloaded += bytesRead;

    if (progress && total > 0) {
      progress(downloaded, total);
    }
  }

  file.close();
  http.end();

  LOG_DBG("HTTP", "Downloaded %zu bytes", downloaded);

  // Verify download size if known, or reject empty downloads
  if (contentLength > 0 && downloaded != contentLength) {
    LOG_ERR("HTTP", "Size mismatch: got %zu, expected %zu", downloaded, contentLength);
    Storage.remove(destPath.c_str());
    return HTTP_ERROR;
  }
  if (downloaded == 0) {
    LOG_ERR("HTTP", "Empty download (0 bytes received)");
    Storage.remove(destPath.c_str());
    return HTTP_ERROR;
  }

  return OK;
}
