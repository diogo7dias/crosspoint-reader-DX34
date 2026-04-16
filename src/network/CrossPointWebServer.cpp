#include "CrossPointWebServer.h"

#include <ArduinoJson.h>
#include <BookFingerprint.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Txt.h>
#include <WiFi.h>
#include <Xtc.h>
#include <esp_task_wdt.h>

#include <algorithm>
#include <functional>
#include <memory>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "Paths.h"
#include "RecentBooksStore.h"
#include "SettingsList.h"
#if __has_include("WebDAVHandler.h")
#include "WebDAVHandler.h"
#define CROSSPOINT_HAS_WEBDAV 1
#endif
#include "html/FilesPageHtml.generated.h"
#include "html/HomePageHtml.generated.h"
#include "html/SettingsPageHtml.generated.h"
#include "html/js/jszip_minJs.generated.h"
#include "util/StringUtils.h"

namespace {
// Folders/files to hide from the web interface file browser
// Note: Items starting with "." are automatically hidden
const char* HIDDEN_ITEMS[] = {"System Volume Information", "XTCache"};
constexpr size_t HIDDEN_ITEMS_COUNT = sizeof(HIDDEN_ITEMS) / sizeof(HIDDEN_ITEMS[0]);
constexpr uint16_t UDP_PORTS[] = {54982, 48123, 39001, 44044, 59678};
constexpr uint16_t LOCAL_UDP_PORT = 8134;

// Static pointer for WebSocket callback (WebSocketsServer requires C-style callback)
CrossPointWebServer* wsInstance = nullptr;

// WebSocket upload state
FsFile wsUploadFile;
String wsUploadFileName;
String wsUploadPath;
size_t wsUploadSize = 0;
size_t wsUploadReceived = 0;
unsigned long wsUploadStartTime = 0;
bool wsUploadInProgress = false;
uint8_t wsUploadClientNum = 255;  // 255 = no active upload client
size_t wsUploadLastProgressSent = 0;

// WebSocket download state
FsFile wsDownloadFile;
size_t wsDownloadSize = 0;
size_t wsDownloadSent = 0;
bool wsDownloadInProgress = false;
uint8_t wsDownloadClientNum = 0;
String wsLastCompleteName;
size_t wsLastCompleteSize = 0;
unsigned long wsLastCompleteAt = 0;

// Compute the cache directory path for a book file (returns empty if not a book).
// Uses content-based fingerprint so the path is stable across file moves.
std::string bookCachePath(const std::string& filePath) {
  if (StringUtils::checkFileExtension(filePath, ".epub")) {
    return BookFingerprint::cacheDirName("epub", filePath, Paths::kDataDir);
  }
  if (StringUtils::checkFileExtension(filePath, ".xtc") ||
      StringUtils::checkFileExtension(filePath, ".xtch")) {
    return BookFingerprint::cacheDirName("xtc", filePath, Paths::kDataDir);
  }
  if (StringUtils::checkFileExtension(filePath, ".txt") ||
      StringUtils::checkFileExtension(filePath, ".md")) {
    return BookFingerprint::cacheDirName("txt", filePath, Paths::kDataDir);
  }
  return {};
}

// Clear book cache for a file (all book types, not just epub)
void clearBookCacheIfNeeded(const String& filePath) {
  if (StringUtils::checkFileExtension(filePath, ".epub")) {
    Epub(filePath.c_str(), Paths::kDataDir).clearCache();
    LOG_DBG("WEB", "Cleared epub cache for: %s", filePath.c_str());
  } else {
    const std::string cache = bookCachePath(filePath.c_str());
    if (!cache.empty() && Storage.exists(cache.c_str())) {
      Storage.removeDir(cache.c_str());
      LOG_DBG("WEB", "Cleared book cache: %s", cache.c_str());
    }
  }
}

// Migrate book metadata when a file is renamed or moved.
// Cache directory is content-keyed so it doesn't change on move — only
// recent.json and state.json paths need updating.
void migrateBookData(const String& oldPath, const String& newPath) {
  const std::string oldPathStr(oldPath.c_str());
  const std::string newPathStr(newPath.c_str());

  // Update recent books list
  auto& recentStore = RecentBooksStore::getInstance();
  const auto& books = recentStore.getBooks();
  for (const auto& book : books) {
    // Case-insensitive compare (paths are normalized lowercase in the store)
    std::string bookKey = book.path;
    std::transform(bookKey.begin(), bookKey.end(), bookKey.begin(),
                   [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    std::string oldKey = oldPathStr;
    std::transform(oldKey.begin(), oldKey.end(), oldKey.begin(),
                   [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    if (bookKey == oldKey) {
      // Re-derive metadata from the new path (title, author, cover at new cache location)
      RecentBook newData = recentStore.getDataFromBook(newPathStr);
      recentStore.removeBook(oldPathStr);
      recentStore.addBook(newPathStr, newData.title, newData.author, newData.coverBmpPath);
      LOG_DBG("WEB", "Updated recent book: %s -> %s", oldPathStr.c_str(), newPathStr.c_str());
      break;
    }
  }

  // 3. Update state.json openEpubPath if it references the old path
  std::string statePath = APP_STATE.openEpubPath;
  std::transform(statePath.begin(), statePath.end(), statePath.begin(),
                 [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
  std::string oldLower = oldPathStr;
  std::transform(oldLower.begin(), oldLower.end(), oldLower.begin(),
                 [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
  if (statePath == oldLower) {
    APP_STATE.openEpubPath = newPathStr;
    APP_STATE.saveToFile();
    LOG_DBG("WEB", "Updated openEpubPath: %s -> %s", oldPathStr.c_str(), newPathStr.c_str());
  }
}

String normalizeWebPath(const String& inputPath) {
  if (inputPath.isEmpty() || inputPath == "/") {
    return "/";
  }
  std::string normalized = FsHelpers::normalisePath(inputPath.c_str());
  String result = normalized.c_str();
  if (result.isEmpty()) {
    return "/";
  }
  if (!result.startsWith("/")) {
    result = "/" + result;
  }
  if (result.length() > 1 && result.endsWith("/")) {
    result = result.substring(0, result.length() - 1);
  }
  return result;
}

bool isProtectedItemName(const String& name) {
  if (name.startsWith(".")) {
    return true;
  }
  for (size_t i = 0; i < HIDDEN_ITEMS_COUNT; i++) {
    if (name.equals(HIDDEN_ITEMS[i])) {
      return true;
    }
  }
  return false;
}
}  // namespace

// File listing page template - now using generated headers:
// - HomePageHtml (from html/HomePage.html)
// - FilesPageHeaderHtml (from html/FilesPageHeader.html)
// - FilesPageFooterHtml (from html/FilesPageFooter.html)
CrossPointWebServer::CrossPointWebServer() {}

CrossPointWebServer::~CrossPointWebServer() { stop(); }

void CrossPointWebServer::begin() {
  if (running) {
    LOG_DBG("WEB", "Web server already running");
    return;
  }

  // Check if we have a valid network connection (either STA connected or AP mode)
  const wifi_mode_t wifiMode = WiFi.getMode();
  const bool isStaConnected = (wifiMode & WIFI_MODE_STA) && (WiFi.status() == WL_CONNECTED);
  const bool isInApMode = (wifiMode & WIFI_MODE_AP) && (WiFi.softAPgetStationNum() >= 0);  // AP is running

  if (!isStaConnected && !isInApMode) {
    LOG_DBG("WEB", "Cannot start webserver - no valid network (mode=%d, status=%d)", wifiMode, WiFi.status());
    return;
  }

  // Store AP mode flag for later use (e.g., in handleStatus)
  apMode = isInApMode;

  LOG_DBG("WEB", "[MEM] Free heap before begin: %d bytes", ESP.getFreeHeap());
  LOG_DBG("WEB", "Network mode: %s", apMode ? "AP" : "STA");

  LOG_DBG("WEB", "Creating web server on port %d...", port);
  server.reset(new WebServer(port));

  // Disable WiFi sleep to improve responsiveness and prevent 'unreachable' errors.
  // This is critical for reliable web server operation on ESP32.
  WiFi.setSleep(false);

  // Note: WebServer class doesn't have setNoDelay() in the standard ESP32 library.
  // We rely on disabling WiFi sleep for responsiveness.

  LOG_DBG("WEB", "[MEM] Free heap after WebServer allocation: %d bytes", ESP.getFreeHeap());

  if (!server) {
    LOG_ERR("WEB", "Failed to create WebServer!");
    return;
  }

  // Setup routes
  LOG_DBG("WEB", "Setting up routes...");
  server->on("/", HTTP_GET, [this] { handleRoot(); });
  server->on("/files", HTTP_GET, [this] { handleFileList(); });

  server->on("/api/status", HTTP_GET, [this] { handleStatus(); });
  server->on("/api/files", HTTP_GET, [this] { handleFileListData(); });
  server->on("/download", HTTP_GET, [this] { handleDownload(); });
  server->on("/preview", HTTP_GET, [this] { handlePreview(); });

  // Upload endpoint with special handling for multipart form data
  server->on("/upload", HTTP_POST, [this] { handleUploadPost(upload); }, [this] { handleUpload(upload); });

  // Create folder endpoint
  server->on("/mkdir", HTTP_POST, [this] { handleCreateFolder(); });

  // Rename file endpoint
  server->on("/rename", HTTP_POST, [this] { handleRename(); });

  // Move file endpoint
  server->on("/move", HTTP_POST, [this] { handleMove(); });

  // Delete file/folder endpoint
  server->on("/delete", HTTP_POST, [this] { handleDelete(); });

  // JSZip library for EPUB optimizer
  server->on("/js/jszip.min.js", HTTP_GET, [this] { handleJszip(); });

  // Settings endpoints
  server->on("/settings", HTTP_GET, [this] { handleSettingsPage(); });
  server->on("/api/settings", HTTP_GET, [this] { handleGetSettings(); });
  server->on("/api/settings", HTTP_POST, [this] { handlePostSettings(); });

  server->onNotFound([this] { handleNotFound(); });
  LOG_DBG("WEB", "[MEM] Free heap after route setup: %d bytes", ESP.getFreeHeap());

#if defined(CROSSPOINT_HAS_WEBDAV)
  // Collect WebDAV headers and register handler
  const char* davHeaders[] = {"Depth", "Destination", "Overwrite", "If", "Lock-Token", "Timeout"};
  server->collectHeaders(davHeaders, 6);
  server->addHandler(new WebDAVHandler());  // WebDAVHandler is deleted by WebServer when server stops
  LOG_DBG("WEB", "WebDAV handler initialized");
#endif
  server->begin();

  // Start WebSocket server for fast binary uploads
  LOG_DBG("WEB", "Starting WebSocket server on port %d...", wsPort);
  wsServer.reset(new WebSocketsServer(wsPort));
  wsInstance = const_cast<CrossPointWebServer*>(this);
  wsServer->begin();
  wsServer->onEvent(wsEventCallback);
  LOG_DBG("WEB", "WebSocket server started");

  udpActive = udp.begin(LOCAL_UDP_PORT);
  LOG_DBG("WEB", "Discovery UDP %s on port %d", udpActive ? "enabled" : "failed", LOCAL_UDP_PORT);

  running = true;

  LOG_DBG("WEB", "Web server started on port %d", port);
  // Show the correct IP based on network mode
  const String ipAddr = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  LOG_DBG("WEB", "Access at http://%s/", ipAddr.c_str());
  LOG_DBG("WEB", "WebSocket at ws://%s:%d/", ipAddr.c_str(), wsPort);
  LOG_DBG("WEB", "[MEM] Free heap after server.begin(): %d bytes", ESP.getFreeHeap());
}

void CrossPointWebServer::abortWsUpload(const char* tag) {
  wsUploadFile.close();
  String filePath = wsUploadPath;
  if (!filePath.endsWith("/")) filePath += "/";
  filePath += wsUploadFileName;
  if (Storage.remove(filePath.c_str())) {
    LOG_DBG(tag, "Deleted incomplete upload: %s", filePath.c_str());
  } else {
    LOG_DBG(tag, "Failed to delete incomplete upload: %s", filePath.c_str());
  }
  wsUploadInProgress = false;
  wsUploadClientNum = 255;
  wsUploadLastProgressSent = 0;
}

void CrossPointWebServer::pumpWsDownload() {
  if (!wsDownloadInProgress || !wsServer || !wsDownloadFile) {
    return;
  }

  constexpr size_t WS_DL_BUF_SIZE = 4096;
  auto dlBuf = std::make_unique<uint8_t[]>(WS_DL_BUF_SIZE);
  if (!dlBuf) {
    LOG_ERR("WS", "Download OOM: cannot allocate buffer");
    wsDownloadFile.close();
    wsDownloadInProgress = false;
    if (wsServer) {
      wsServer->sendTXT(wsDownloadClientNum, "ERROR:Out of memory");
    }
    return;
  }

  // Send up to 2 chunks per loop iteration
  for (int i = 0; i < 2 && wsDownloadSent < wsDownloadSize; i++) {
    esp_task_wdt_reset();
    const size_t remaining = wsDownloadSize - wsDownloadSent;
    const size_t toRead = remaining < WS_DL_BUF_SIZE ? remaining : WS_DL_BUF_SIZE;
    const int bytesRead = wsDownloadFile.read(dlBuf.get(), toRead);
    if (bytesRead <= 0) break;

    if (!wsServer->sendBIN(wsDownloadClientNum, dlBuf.get(), bytesRead)) {
      LOG_ERR("WS", "Download send failed at %d/%d", wsDownloadSent, wsDownloadSize);
      wsDownloadFile.close();
      wsDownloadInProgress = false;
      wsServer->sendTXT(wsDownloadClientNum, "ERROR:Send failed");
      return;
    }
    wsDownloadSent += bytesRead;
  }

  // Check if download complete
  if (wsDownloadSent >= wsDownloadSize) {
    wsDownloadFile.close();
    wsDownloadInProgress = false;
    wsServer->sendTXT(wsDownloadClientNum, "DLDONE");
    LOG_DBG("WS", "Download complete: %d bytes sent", wsDownloadSent);
  }
}

void CrossPointWebServer::handleJszip() const {
  server->sendHeader("Content-Encoding", "gzip");
  server->send_P(200, "application/javascript", jszip_minJs, jszip_minJsCompressedSize);
  LOG_DBG("WEB", "Served jszip.min.js");
}

void CrossPointWebServer::stop() {
  if (!running || !server) {
    LOG_DBG("WEB", "stop() called but already stopped (running=%d, server=%p)", running, server.get());
    return;
  }

  LOG_DBG("WEB", "STOP INITIATED - setting running=false first");
  running = false;  // Set this FIRST to prevent handleClient from using server

  LOG_DBG("WEB", "[MEM] Free heap before stop: %d bytes", ESP.getFreeHeap());

  // Close any in-progress WebSocket upload and remove partial file
  if (wsUploadInProgress && wsUploadFile) {
    abortWsUpload("WEB");
  }
  if (wsDownloadInProgress && wsDownloadFile) {
    wsDownloadFile.close();
    wsDownloadInProgress = false;
  }

  // Stop WebSocket server
  if (wsServer) {
    LOG_DBG("WEB", "Stopping WebSocket server...");
    wsServer->close();
    wsServer.reset();
    wsInstance = nullptr;
    LOG_DBG("WEB", "WebSocket server stopped");
  }

  if (udpActive) {
    udp.stop();
    udpActive = false;
  }

  // Brief delay to allow any in-flight handleClient() calls to complete
  delay(20);

  server->stop();
  LOG_DBG("WEB", "[MEM] Free heap after server->stop(): %d bytes", ESP.getFreeHeap());

  // Brief delay before deletion
  delay(10);

  server.reset();
  LOG_DBG("WEB", "Web server stopped and deleted");
  LOG_DBG("WEB", "[MEM] Free heap after delete server: %d bytes", ESP.getFreeHeap());

  // Note: Static upload variables (uploadFileName, uploadPath, uploadError) are declared
  // later in the file and will be cleared when they go out of scope or on next upload
  LOG_DBG("WEB", "[MEM] Free heap final: %d bytes", ESP.getFreeHeap());
}

void CrossPointWebServer::handleClient() {
  static unsigned long lastDebugPrint = 0;

  // Check running flag FIRST before accessing server
  if (!running) {
    return;
  }

  // Double-check server pointer is valid
  if (!server) {
    LOG_DBG("WEB", "WARNING: handleClient called with null server!");
    return;
  }

  // Print debug every 10 seconds to confirm handleClient is being called
  if (millis() - lastDebugPrint > 10000) {
    LOG_DBG("WEB", "handleClient active, server running on port %d", port);
    lastDebugPrint = millis();
  }

  server->handleClient();

  // Handle WebSocket events
  if (wsServer) {
    wsServer->loop();
  }

  pumpWsDownload();

  // Respond to discovery broadcasts
  if (udpActive) {
    int packetSize = udp.parsePacket();
    if (packetSize > 0) {
      char buffer[16];
      int len = udp.read(buffer, sizeof(buffer) - 1);
      if (len > 0) {
        buffer[len] = '\0';
        if (strcmp(buffer, "hello") == 0) {
          String hostname = WiFi.getHostname();
          if (hostname.isEmpty()) {
            hostname = "crosspoint";
          }
          String message = "crosspoint (on " + hostname + ");" + String(wsPort);
          udp.beginPacket(udp.remoteIP(), udp.remotePort());
          udp.write(reinterpret_cast<const uint8_t*>(message.c_str()), message.length());
          udp.endPacket();
        }
      }
    }
  }
}

CrossPointWebServer::WsUploadStatus CrossPointWebServer::getWsUploadStatus() const {
  WsUploadStatus status;
  status.inProgress = wsUploadInProgress;
  status.received = wsUploadReceived;
  status.total = wsUploadSize;
  status.filename = wsUploadFileName.c_str();
  status.lastCompleteName = wsLastCompleteName.c_str();
  status.lastCompleteSize = wsLastCompleteSize;
  status.lastCompleteAt = wsLastCompleteAt;
  return status;
}

static void sendHtmlContent(WebServer* server, const char* data, size_t len) {
  server->sendHeader("Content-Encoding", "gzip");
  server->send_P(200, "text/html", data, len);
}

void CrossPointWebServer::handleRoot() const {
  sendHtmlContent(server.get(), HomePageHtml, sizeof(HomePageHtml));
  LOG_DBG("WEB", "Served root page");
}

void CrossPointWebServer::handleNotFound() const {
  String message = "404 Not Found\n\n";
  message += "URI: " + server->uri() + "\n";
  server->send(404, "text/plain", message);
}

void CrossPointWebServer::handleStatus() const {
  // Get correct IP based on AP vs STA mode
  const String ipAddr = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();

  JsonDocument doc;
  doc["version"] = CROSSPOINT_VERSION;
  doc["ip"] = ipAddr;
  doc["mode"] = apMode ? "AP" : "STA";
  doc["rssi"] = apMode ? 0 : WiFi.RSSI();
  doc["freeHeap"] = ESP.getFreeHeap();
  doc["uptime"] = millis() / 1000;

  String json;
  serializeJson(doc, json);
  server->send(200, "application/json", json);
}

void CrossPointWebServer::scanFiles(const char* path, const std::function<void(FileInfo)>& callback) const {
  FsFile root = Storage.open(path);
  if (!root) {
    LOG_DBG("WEB", "Failed to open directory: %s", path);
    return;
  }

  if (!root.isDirectory()) {
    LOG_DBG("WEB", "Not a directory: %s", path);
    root.close();
    return;
  }

  LOG_DBG("WEB", "Scanning files in: %s", path);

  FsFile file = root.openNextFile();
  char name[256];
  while (file) {
    file.getName(name, sizeof(name));
    auto fileName = String(name);

    // Skip hidden items (starting with ".")
    bool shouldHide = !SETTINGS.showHiddenFiles && fileName.startsWith(".");

    // Check against explicitly hidden items list
    if (!shouldHide) {
      for (size_t i = 0; i < HIDDEN_ITEMS_COUNT; i++) {
        if (fileName.equals(HIDDEN_ITEMS[i])) {
          shouldHide = true;
          break;
        }
      }
    }

    if (!shouldHide) {
      FileInfo info;
      info.name = fileName;
      info.isDirectory = file.isDirectory();

      if (info.isDirectory) {
        info.size = 0;
        info.isEpub = false;
        info.isBmp = false;
      } else {
        info.size = file.size();
        info.isEpub = isEpubFile(info.name);
        info.isBmp = isBmpFile(info.name);
      }

      callback(info);
    }

    file.close();
    yield();               // Yield to allow WiFi and other tasks to process during long scans
    esp_task_wdt_reset();  // Reset watchdog to prevent timeout on large directories
    file = root.openNextFile();
  }
  root.close();
}

bool CrossPointWebServer::isEpubFile(const String& filename) const {
  String lower = filename;
  lower.toLowerCase();
  return lower.endsWith(".epub");
}

bool CrossPointWebServer::isBmpFile(const String& filename) const {
  String lower = filename;
  lower.toLowerCase();
  return lower.endsWith(".bmp");
}

void CrossPointWebServer::handleFileList() const {
  sendHtmlContent(server.get(), FilesPageHtml, sizeof(FilesPageHtml));
}

void CrossPointWebServer::handleFileListData() const {
  // Get current path from query string (default to root)
  String currentPath = "/";
  if (server->hasArg("path")) {
    currentPath = server->arg("path");
    // Ensure path starts with /
    if (!currentPath.startsWith("/")) {
      currentPath = "/" + currentPath;
    }
    // Remove trailing slash unless it's root
    if (currentPath.length() > 1 && currentPath.endsWith("/")) {
      currentPath = currentPath.substring(0, currentPath.length() - 1);
    }
  }

  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[");
  char output[1024];  // Must fit JSON with FAT32 max filename (255 chars) + metadata
  constexpr size_t outputSize = sizeof(output);
  bool seenFirst = false;
  JsonDocument doc;

  scanFiles(currentPath.c_str(), [this, &output, &doc, seenFirst](const FileInfo& info) mutable {
    doc.clear();
    doc["name"] = info.name;
    doc["size"] = info.size;
    doc["isDirectory"] = info.isDirectory;
    doc["isEpub"] = info.isEpub;
    doc["isBmp"] = info.isBmp;

    const size_t written = serializeJson(doc, output, outputSize);
    if (written >= outputSize) {
      // JSON output truncated; skip this entry to avoid sending malformed JSON
      LOG_DBG("WEB", "Skipping file entry with oversized JSON for name: %s", info.name.c_str());
      return;
    }

    if (seenFirst) {
      server->sendContent(",");
    } else {
      seenFirst = true;
    }
    server->sendContent(output);
  });
  server->sendContent("]");
  // End of streamed response, empty chunk to signal client
  server->sendContent("");
  LOG_DBG("WEB", "Served file listing page for path: %s", currentPath.c_str());
}

void CrossPointWebServer::handleDownload() const {
  if (!server->hasArg("path")) {
    server->send(400, "text/plain", "Missing path");
    return;
  }

  String itemPath = server->arg("path");
  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  if (!itemPath.startsWith("/")) {
    itemPath = "/" + itemPath;
  }

  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);
  if (itemName.startsWith(".")) {
    server->send(403, "text/plain", "Cannot access system files");
    return;
  }
  for (size_t i = 0; i < HIDDEN_ITEMS_COUNT; i++) {
    if (itemName.equals(HIDDEN_ITEMS[i])) {
      server->send(403, "text/plain", "Cannot access protected items");
      return;
    }
  }

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  FsFile file = Storage.open(itemPath.c_str());
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  if (file.isDirectory()) {
    file.close();
    server->send(400, "text/plain", "Path is a directory");
    return;
  }

  String contentType = "application/octet-stream";
  if (isEpubFile(itemPath)) {
    contentType = "application/epub+zip";
  }

  char nameBuf[128] = {0};
  String filename = "download";
  if (file.getName(nameBuf, sizeof(nameBuf))) {
    filename = nameBuf;
  }

  const size_t fileSize = file.size();

  // Send exact Content-Length so the browser knows when the download is done.
  // Note: _prepareHeader already adds Connection: close, don't duplicate it.
  server->setContentLength(fileSize);
  server->sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  server->send(200, contentType.c_str(), "");

  constexpr size_t DL_BUF_SIZE = 4096;
  auto buf = std::make_unique<uint8_t[]>(DL_BUF_SIZE);
  if (!buf) {
    LOG_ERR("WEB", "Download OOM: cannot allocate %d byte buffer", DL_BUF_SIZE);
    file.close();
    return;
  }

  size_t totalSent = 0;
  bool ok = true;
  while (file.available() && totalSent < fileSize) {
    esp_task_wdt_reset();

    // Check if client is still connected
    if (!server->client().connected()) {
      LOG_ERR("WEB", "Download aborted: client disconnected at %u/%u bytes", totalSent, fileSize);
      ok = false;
      break;
    }

    const size_t remaining = fileSize - totalSent;
    const size_t toRead = remaining < DL_BUF_SIZE ? remaining : DL_BUF_SIZE;
    const int bytesRead = file.read(buf.get(), toRead);
    if (bytesRead <= 0) break;

    // Write directly to client and check return value
    const size_t written = server->client().write(buf.get(), bytesRead);
    if (written == 0) {
      LOG_ERR("WEB", "Download write failed at %u/%u bytes", totalSent, fileSize);
      ok = false;
      break;
    }
    totalSent += written;

    // Yield to let WiFi stack flush TCP buffers
    yield();
  }
  file.close();

  if (ok) {
    LOG_DBG("WEB", "Download complete: %s (%u bytes)", filename.c_str(), totalSent);
  }
}

void CrossPointWebServer::handlePreview() const {
  if (!server->hasArg("path")) {
    server->send(400, "text/plain", "Missing path");
    return;
  }

  String itemPath = server->arg("path");
  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  if (!itemPath.startsWith("/")) {
    itemPath = "/" + itemPath;
  }

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "File not found");
    return;
  }

  FsFile file = Storage.open(itemPath.c_str());
  if (!file || file.isDirectory()) {
    if (file) file.close();
    server->send(400, "text/plain", "Not a file");
    return;
  }

  // Use chunked transfer encoding for proper HTTP-level EOF signaling
  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "image/bmp", "");

  constexpr size_t PV_BUF_SIZE = 4096;
  auto buf = std::make_unique<uint8_t[]>(PV_BUF_SIZE);
  if (!buf) {
    LOG_ERR("WEB", "Preview OOM: cannot allocate buffer");
    file.close();
    server->sendContent("");
    return;
  }

  while (file.available()) {
    esp_task_wdt_reset();
    const int bytesRead = file.read(buf.get(), PV_BUF_SIZE);
    if (bytesRead <= 0) break;
    server->sendContent(reinterpret_cast<const char*>(buf.get()), bytesRead);
    yield();
  }
  file.close();

  server->sendContent("");
  LOG_DBG("WEB", "Preview served");
}

// Diagnostic counters for upload performance analysis
static unsigned long uploadStartTime = 0;
static unsigned long totalWriteTime = 0;
static size_t writeCount = 0;

static bool flushUploadBuffer(CrossPointWebServer::UploadState& state) {
  if (state.bufferPos > 0 && state.file) {
    esp_task_wdt_reset();  // Reset watchdog before potentially slow SD write
    const unsigned long writeStart = millis();
    const size_t written = state.file.write(state.buffer.data(), state.bufferPos);
    totalWriteTime += millis() - writeStart;
    writeCount++;
    esp_task_wdt_reset();  // Reset watchdog after SD write

    if (written != state.bufferPos) {
      LOG_DBG("WEB", "[UPLOAD] Buffer flush failed: expected %d, wrote %d", state.bufferPos, written);
      state.bufferPos = 0;
      return false;
    }
    state.bufferPos = 0;
  }
  return true;
}

void CrossPointWebServer::handleUpload(UploadState& state) const {
  static size_t lastLoggedSize = 0;

  // Reset watchdog at start of every upload callback - HTTP parsing can be slow
  esp_task_wdt_reset();

  // Safety check: ensure server is still valid
  if (!running || !server) {
    LOG_DBG("WEB", "[UPLOAD] ERROR: handleUpload called but server not running!");
    return;
  }

  const HTTPUpload& upload = server->upload();

  if (upload.status == UPLOAD_FILE_START) {
    // Reset watchdog - this is the critical 1% crash point
    esp_task_wdt_reset();

    state.fileName = upload.filename;
    state.size = 0;
    state.success = false;
    state.error = "";
    uploadStartTime = millis();
    lastLoggedSize = 0;
    state.bufferPos = 0;
    totalWriteTime = 0;
    writeCount = 0;

    // Get upload path from query parameter (defaults to root if not specified)
    // Note: We use query parameter instead of form data because multipart form
    // fields aren't available until after file upload completes
    if (server->hasArg("path")) {
      state.path = normalizeWebPath(server->arg("path"));
    } else {
      state.path = "/";
    }

    // Sanitize filename: strip path separators to prevent traversal
    state.fileName.replace("/", "");
    state.fileName.replace("\\", "");
    state.fileName.replace("..", "");

    LOG_DBG("WEB", "[UPLOAD] START: %s to path: %s", state.fileName.c_str(), state.path.c_str());
    LOG_DBG("WEB", "[UPLOAD] Free heap: %d bytes", ESP.getFreeHeap());

    // Create file path
    String filePath = state.path;
    if (!filePath.endsWith("/")) filePath += "/";
    filePath += state.fileName;

    // Check if file already exists - SD operations can be slow
    esp_task_wdt_reset();
    if (Storage.exists(filePath.c_str())) {
      LOG_DBG("WEB", "[UPLOAD] Overwriting existing file: %s", filePath.c_str());
      esp_task_wdt_reset();
      Storage.remove(filePath.c_str());
    }

    // Open file for writing - this can be slow due to FAT cluster allocation
    esp_task_wdt_reset();
    if (!Storage.openFileForWrite("WEB", filePath, state.file)) {
      state.error = "Failed to create file on SD card";
      LOG_DBG("WEB", "[UPLOAD] FAILED to create file: %s", filePath.c_str());
      return;
    }
    esp_task_wdt_reset();

    LOG_DBG("WEB", "[UPLOAD] File created successfully: %s", filePath.c_str());
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (state.file && state.error.isEmpty()) {
      // Buffer incoming data and flush when buffer is full
      // This reduces SD card write operations and improves throughput
      const uint8_t* data = upload.buf;
      size_t remaining = upload.currentSize;

      while (remaining > 0) {
        const size_t space = UploadState::UPLOAD_BUFFER_SIZE - state.bufferPos;
        const size_t toCopy = (remaining < space) ? remaining : space;

        memcpy(state.buffer.data() + state.bufferPos, data, toCopy);
        state.bufferPos += toCopy;
        data += toCopy;
        remaining -= toCopy;

        // Flush buffer when full
        if (state.bufferPos >= UploadState::UPLOAD_BUFFER_SIZE) {
          if (!flushUploadBuffer(state)) {
            state.error = "Failed to write to SD card - disk may be full";
            state.file.close();
            return;
          }
        }
      }

      state.size += upload.currentSize;

      // Log progress every 100KB
      if (state.size - lastLoggedSize >= 102400) {
        const unsigned long elapsed = millis() - uploadStartTime;
        const float kbps = (elapsed > 0) ? (state.size / 1024.0) / (elapsed / 1000.0) : 0;
        LOG_DBG("WEB", "[UPLOAD] %d bytes (%.1f KB), %.1f KB/s, %d writes", state.size, state.size / 1024.0, kbps,
                writeCount);
        lastLoggedSize = state.size;
      }
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (state.file) {
      // Flush any remaining buffered data
      if (!flushUploadBuffer(state)) {
        state.error = "Failed to write final data to SD card";
      }
      state.file.close();

      if (state.error.isEmpty()) {
        state.success = true;
        const unsigned long elapsed = millis() - uploadStartTime;
        const float avgKbps = (elapsed > 0) ? (state.size / 1024.0) / (elapsed / 1000.0) : 0;
        const float writePercent = (elapsed > 0) ? (totalWriteTime * 100.0 / elapsed) : 0;
        LOG_DBG("WEB", "[UPLOAD] Complete: %s (%d bytes in %lu ms, avg %.1f KB/s)", state.fileName.c_str(), state.size,
                elapsed, avgKbps);
        LOG_DBG("WEB", "[UPLOAD] Diagnostics: %d writes, total write time: %lu ms (%.1f%%)", writeCount, totalWriteTime,
                writePercent);

        // Clear epub cache to prevent stale metadata issues when overwriting files
        String filePath = state.path;
        if (!filePath.endsWith("/")) filePath += "/";
        filePath += state.fileName;
        clearBookCacheIfNeeded(filePath);
      }
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    state.bufferPos = 0;  // Discard buffered data
    if (state.file) {
      state.file.close();
      // Try to delete the incomplete file
      String filePath = state.path;
      if (!filePath.endsWith("/")) filePath += "/";
      filePath += state.fileName;
      Storage.remove(filePath.c_str());
    }
    state.error = "Upload aborted";
    LOG_DBG("WEB", "Upload aborted");
  }
}

void CrossPointWebServer::handleUploadPost(UploadState& state) const {
  if (state.success) {
    server->send(200, "text/plain", "File uploaded successfully: " + state.fileName);
  } else {
    const String error = state.error.isEmpty() ? "Unknown error during upload" : state.error;
    server->send(400, "text/plain", error);
  }
}

void CrossPointWebServer::handleCreateFolder() const {
  // Get folder name from form data
  if (!server->hasArg("name")) {
    server->send(400, "text/plain", "Missing folder name");
    return;
  }

  const String folderName = server->arg("name");

  // Validate folder name
  if (folderName.isEmpty()) {
    server->send(400, "text/plain", "Folder name cannot be empty");
    return;
  }

  // Get parent path
  String parentPath = "/";
  if (server->hasArg("path")) {
    parentPath = server->arg("path");
    if (!parentPath.startsWith("/")) {
      parentPath = "/" + parentPath;
    }
    if (parentPath.length() > 1 && parentPath.endsWith("/")) {
      parentPath = parentPath.substring(0, parentPath.length() - 1);
    }
  }

  // Build full folder path
  String folderPath = parentPath;
  if (!folderPath.endsWith("/")) folderPath += "/";
  folderPath += folderName;

  LOG_DBG("WEB", "Creating folder: %s", folderPath.c_str());

  // Check if already exists
  if (Storage.exists(folderPath.c_str())) {
    server->send(400, "text/plain", "Folder already exists");
    return;
  }

  // Create the folder
  if (Storage.mkdir(folderPath.c_str())) {
    LOG_DBG("WEB", "Folder created successfully: %s", folderPath.c_str());
    server->send(200, "text/plain", "Folder created: " + folderName);
  } else {
    LOG_DBG("WEB", "Failed to create folder: %s", folderPath.c_str());
    server->send(500, "text/plain", "Failed to create folder");
  }
}

void CrossPointWebServer::handleRename() const {
  if (!server->hasArg("path") || !server->hasArg("name")) {
    server->send(400, "text/plain", "Missing path or new name");
    return;
  }

  String itemPath = normalizeWebPath(server->arg("path"));
  String newName = server->arg("name");
  newName.trim();

  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  if (newName.isEmpty()) {
    server->send(400, "text/plain", "New name cannot be empty");
    return;
  }
  if (newName.indexOf('/') >= 0 || newName.indexOf('\\') >= 0) {
    server->send(400, "text/plain", "Invalid file name");
    return;
  }
  if (isProtectedItemName(newName)) {
    server->send(403, "text/plain", "Cannot rename to protected name");
    return;
  }

  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);
  if (isProtectedItemName(itemName)) {
    server->send(403, "text/plain", "Cannot rename protected item");
    return;
  }
  if (newName == itemName) {
    server->send(200, "text/plain", "Name unchanged");
    return;
  }

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  FsFile file = Storage.open(itemPath.c_str());
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  if (file.isDirectory()) {
    file.close();
    server->send(400, "text/plain", "Only files can be renamed");
    return;
  }

  String parentPath = itemPath.substring(0, itemPath.lastIndexOf('/'));
  if (parentPath.isEmpty()) {
    parentPath = "/";
  }
  String newPath = parentPath;
  if (!newPath.endsWith("/")) {
    newPath += "/";
  }
  newPath += newName;

  if (Storage.exists(newPath.c_str())) {
    file.close();
    server->send(409, "text/plain", "Target already exists");
    return;
  }

  const bool success = file.rename(newPath.c_str());
  file.close();

  if (success) {
    migrateBookData(itemPath, newPath);
    LOG_DBG("WEB", "Renamed file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(200, "text/plain", "Renamed successfully");
  } else {
    LOG_ERR("WEB", "Failed to rename file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(500, "text/plain", "Failed to rename file");
  }
}

void CrossPointWebServer::handleMove() const {
  if (!server->hasArg("path") || !server->hasArg("dest")) {
    server->send(400, "text/plain", "Missing path or destination");
    return;
  }

  String itemPath = normalizeWebPath(server->arg("path"));
  String destPath = normalizeWebPath(server->arg("dest"));

  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  if (destPath.isEmpty()) {
    server->send(400, "text/plain", "Invalid destination");
    return;
  }

  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);
  if (isProtectedItemName(itemName)) {
    server->send(403, "text/plain", "Cannot move protected item");
    return;
  }
  if (destPath != "/") {
    const String destName = destPath.substring(destPath.lastIndexOf('/') + 1);
    if (isProtectedItemName(destName)) {
      server->send(403, "text/plain", "Cannot move into protected folder");
      return;
    }
  }

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  FsFile file = Storage.open(itemPath.c_str());
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  if (file.isDirectory()) {
    file.close();
    server->send(400, "text/plain", "Only files can be moved");
    return;
  }

  if (!Storage.exists(destPath.c_str())) {
    file.close();
    server->send(404, "text/plain", "Destination not found");
    return;
  }
  FsFile destDir = Storage.open(destPath.c_str());
  if (!destDir || !destDir.isDirectory()) {
    if (destDir) {
      destDir.close();
    }
    file.close();
    server->send(400, "text/plain", "Destination is not a folder");
    return;
  }
  destDir.close();

  String newPath = destPath;
  if (!newPath.endsWith("/")) {
    newPath += "/";
  }
  newPath += itemName;

  if (newPath == itemPath) {
    file.close();
    server->send(200, "text/plain", "Already in destination");
    return;
  }
  if (Storage.exists(newPath.c_str())) {
    file.close();
    server->send(409, "text/plain", "Target already exists");
    return;
  }

  const bool success = file.rename(newPath.c_str());
  file.close();

  if (success) {
    migrateBookData(itemPath, newPath);
    LOG_DBG("WEB", "Moved file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(200, "text/plain", "Moved successfully");
  } else {
    LOG_ERR("WEB", "Failed to move file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(500, "text/plain", "Failed to move file");
  }
}

void CrossPointWebServer::handleDelete() const {
  // Check if 'paths' argument is provided
  if (!server->hasArg("paths")) {
    server->send(400, "text/plain", "Missing paths");
    return;
  }

  // Parse paths
  String pathsArg = server->arg("paths");
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, pathsArg);
  if (error) {
    server->send(400, "text/plain", "Invalid paths format");
    return;
  }

  auto paths = doc.as<JsonArray>();
  if (paths.isNull() || paths.size() == 0) {
    server->send(400, "text/plain", "No paths provided");
    return;
  }

  // Iterate over paths and delete each item
  bool allSuccess = true;
  String failedItems;
  failedItems.reserve(256);

  for (const auto& p : paths) {
    auto itemPath = p.as<String>();

    // Validate path
    if (itemPath.isEmpty() || itemPath == "/") {
      failedItems += itemPath + " (cannot delete root); ";
      allSuccess = false;
      continue;
    }

    // Ensure path starts with /
    if (!itemPath.startsWith("/")) {
      itemPath = "/" + itemPath;
    }

    // Security check: prevent deletion of protected items
    const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);

    // Hidden/system files are protected
    if (itemName.startsWith(".")) {
      failedItems += itemPath + " (hidden/system file); ";
      allSuccess = false;
      continue;
    }

    // Check against explicitly protected items
    bool isProtected = false;
    for (size_t i = 0; i < HIDDEN_ITEMS_COUNT; i++) {
      if (itemName.equals(HIDDEN_ITEMS[i])) {
        isProtected = true;
        break;
      }
    }
    if (isProtected) {
      failedItems += itemPath + " (protected file); ";
      allSuccess = false;
      continue;
    }

    // Check if item exists
    if (!Storage.exists(itemPath.c_str())) {
      failedItems += itemPath + " (not found); ";
      allSuccess = false;
      continue;
    }

    // Decide whether it's a directory or file by opening it
    bool success = false;
    FsFile f = Storage.open(itemPath.c_str());
    if (f && f.isDirectory()) {
      // For folders, ensure empty before removing
      FsFile entry = f.openNextFile();
      if (entry) {
        entry.close();
        f.close();
        failedItems += itemPath + " (folder not empty); ";
        allSuccess = false;
        continue;
      }
      f.close();
      success = Storage.rmdir(itemPath.c_str());
    } else {
      // It's a file (or couldn't open as dir) — remove file
      if (f) f.close();

      // Protect QUOTES files from deletion
      if (itemPath.endsWith("_QUOTES.txt")) {
        failedItems += itemPath + " (QUOTES file protected); ";
        allSuccess = false;
        continue;
      }

      success = Storage.remove(itemPath.c_str());
      clearBookCacheIfNeeded(itemPath);
      RECENT_BOOKS.removeBook(itemPath.c_str());
    }

    if (!success) {
      failedItems += itemPath + " (deletion failed); ";
      allSuccess = false;
    }
  }

  if (allSuccess) {
    server->send(200, "text/plain", "All items deleted successfully");
  } else {
    server->send(500, "text/plain", "Failed to delete some items: " + failedItems);
  }
}

void CrossPointWebServer::handleSettingsPage() const {
  sendHtmlContent(server.get(), SettingsPageHtml, sizeof(SettingsPageHtml));
  LOG_DBG("WEB", "Served settings page");
}

void CrossPointWebServer::handleGetSettings() const {
  auto settings = getSettingsList();

  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[");

  char output[512];
  constexpr size_t outputSize = sizeof(output);
  bool seenFirst = false;
  JsonDocument doc;

  for (const auto& s : settings) {
    if (!s.key) continue;  // Skip ACTION-only entries
    doc.clear();
    doc["key"] = s.key;
    doc["name"] = I18N.get(s.nameId);
    doc["category"] = I18N.get(s.category);

    switch (s.type) {
      case SettingType::TOGGLE: {
        doc["type"] = "toggle";
        if (s.valuePtr) {
          doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
        }
        break;
      }
      case SettingType::ENUM: {
        doc["type"] = "enum";
        if (s.valuePtr) {
          if (s.valuePtr == &CrossPointSettings::fontFamily) {
            doc["value"] = static_cast<int>(
                CrossPointSettings::fontFamilyToDisplayIndex(
                    SETTINGS.fontFamily));
          } else {
            doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
          }
        } else if (s.valueGetter) {
          doc["value"] = static_cast<int>(s.valueGetter());
        }
        JsonArray options = doc["options"].to<JsonArray>();
        if (s.valuePtr == &CrossPointSettings::fontSize) {
          const uint8_t optionCount =
              CrossPointSettings::fontSizeOptionCount(SETTINGS.fontFamily);
          for (uint8_t i = 0; i < optionCount; ++i) {
            const uint8_t sizeValue =
                CrossPointSettings::displayIndexToFontSize(SETTINGS.fontFamily,
                                                           i);
            options.add(String(CrossPointSettings::fontSizeToPointSize(
                SETTINGS.fontFamily, sizeValue)));
          }
          doc["value"] = static_cast<int>(
              CrossPointSettings::fontSizeToDisplayIndex(SETTINGS.fontFamily,
                                                         SETTINGS.fontSize));
        } else {
          for (const auto& opt : s.enumValues) {
            options.add(I18N.get(opt));
          }
        }
        break;
      }
      case SettingType::VALUE: {
        doc["type"] = "value";
        if (s.valuePtr) {
          doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
        }
        doc["min"] = s.valueRange.min;
        doc["max"] = s.valueRange.max;
        doc["step"] = s.valueRange.step;
        break;
      }
      case SettingType::STRING: {
        doc["type"] = "string";
        if (s.stringGetter) {
          doc["value"] = s.stringGetter();
        } else if (s.stringPtr) {
          doc["value"] = s.stringPtr;
        }
        break;
      }
      default:
        continue;
    }

    const size_t written = serializeJson(doc, output, outputSize);
    if (written >= outputSize) {
      LOG_DBG("WEB", "Skipping oversized setting JSON for: %s", s.key);
      continue;
    }

    if (seenFirst) {
      server->sendContent(",");
    } else {
      seenFirst = true;
    }
    server->sendContent(output);
  }

  server->sendContent("]");
  server->sendContent("");
  LOG_DBG("WEB", "Served settings API");
}

void CrossPointWebServer::handlePostSettings() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  auto settings = getSettingsList();
  int applied = 0;

  for (auto& s : settings) {
    if (!s.key) continue;
    if (!doc[s.key].is<JsonVariant>()) continue;

    switch (s.type) {
      case SettingType::TOGGLE: {
        const int val = doc[s.key].as<int>() ? 1 : 0;
        if (s.valuePtr) {
          SETTINGS.*(s.valuePtr) = val;
        }
        applied++;
        break;
      }
      case SettingType::ENUM: {
        const int val = doc[s.key].as<int>();
        const int maxEnumValue =
            (s.valuePtr == &CrossPointSettings::fontSize)
                ? static_cast<int>(CrossPointSettings::fontSizeOptionCount(
                      SETTINGS.fontFamily))
                : static_cast<int>(s.enumValues.size());
        if (val >= 0 && val < maxEnumValue) {
          if (s.valuePtr) {
            if (s.valuePtr == &CrossPointSettings::fontSize) {
              SETTINGS.fontSize =
                  CrossPointSettings::displayIndexToFontSize(
                      SETTINGS.fontFamily, static_cast<uint8_t>(val));
            } else if (s.valuePtr == &CrossPointSettings::fontFamily) {
              SETTINGS.fontFamily =
                  CrossPointSettings::displayIndexToFontFamily(
                      static_cast<uint8_t>(val));
              SETTINGS.fontSize =
                  CrossPointSettings::normalizeFontSizeForFamily(
                      SETTINGS.fontFamily, SETTINGS.fontSize);
              SETTINGS.lineSpacingPercent = 90;  // Reset to default on font change
            } else {
              SETTINGS.*(s.valuePtr) = static_cast<uint8_t>(val);
            }
          } else if (s.valueSetter) {
            s.valueSetter(static_cast<uint8_t>(val));
          }
          applied++;
        }
        break;
      }
      case SettingType::VALUE: {
        const int val = doc[s.key].as<int>();
        if (val >= s.valueRange.min && val <= s.valueRange.max) {
          if (s.valuePtr) {
            SETTINGS.*(s.valuePtr) = static_cast<uint8_t>(val);
          }
          applied++;
        }
        break;
      }
      case SettingType::STRING: {
        const std::string val = doc[s.key].as<std::string>();
        if (s.stringSetter) {
          s.stringSetter(val);
        } else if (s.stringPtr && s.stringMaxLen > 0) {
          strncpy(s.stringPtr, val.c_str(), s.stringMaxLen - 1);
          s.stringPtr[s.stringMaxLen - 1] = '\0';
        }
        applied++;
        break;
      }
      default:
        break;
    }
  }

  SETTINGS.saveToFile();

  LOG_DBG("WEB", "Applied %d setting(s)", applied);
  server->send(200, "text/plain", String("Applied ") + String(applied) + " setting(s)");
}

// WebSocket callback trampoline — check both pointer and running flag.
// During shutdown, wsInstance is cleared after running is set to false.
// Checking running prevents processing events in the teardown window.
void CrossPointWebServer::wsEventCallback(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  // Local copy avoids TOCTOU if stop() nulls wsInstance between the check and the call.
  auto* inst = wsInstance;
  if (inst && inst->running) {
    inst->onWebSocketEvent(num, type, payload, length);
  }
}

// WebSocket event handler for fast binary uploads
// Protocol:
//   1. Client sends TEXT message: "START:<filename>:<size>:<path>"
//   2. Client sends BINARY messages with file data chunks
//   3. Server sends TEXT "PROGRESS:<received>:<total>" after each chunk
//   4. Server sends TEXT "DONE" or "ERROR:<message>" when complete
void CrossPointWebServer::onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      LOG_DBG("WS", "Client %u disconnected", num);
      // Only clean up if this is the client that owns the active upload.
      // A new client may have already started a fresh upload before this
      // DISCONNECTED event fires (race condition on quick cancel + retry).
      if (num == wsUploadClientNum && wsUploadInProgress && wsUploadFile) {
        abortWsUpload("WS");
      }
      // Clean up any in-progress download
      if (wsDownloadInProgress && wsDownloadClientNum == num) {
        wsDownloadFile.close();
        wsDownloadInProgress = false;
        LOG_DBG("WS", "Cancelled download due to disconnect");
      }
      break;

    case WStype_CONNECTED: {
      LOG_DBG("WS", "Client %u connected", num);
      break;
    }

    case WStype_TEXT: {
      String msg = String((char*)payload);
      LOG_DBG("WS", "Text from client %u: %s", num, msg.c_str());

      if (msg.startsWith("DOWNLOAD:")) {
        handleWsDownloadRequest(num, msg);
      } else if (msg.startsWith("START:")) {
        handleWsUploadStart(num, msg);
      }
      break;
    }

    case WStype_BIN: {
      handleWsUploadData(num, payload, length);
      break;
    }

    default:
      break;
  }
}

void CrossPointWebServer::handleWsDownloadRequest(uint8_t num, const String& msg) {
  // Parse: DOWNLOAD:<path>
  String dlPath = msg.substring(9);
  if (!dlPath.startsWith("/")) dlPath = "/" + dlPath;

  if (wsUploadInProgress) {
    wsServer->sendTXT(num, "ERROR:Upload in progress");
    return;
  }
  if (wsDownloadInProgress) {
    wsServer->sendTXT(num, "ERROR:Download already in progress");
    return;
  }
  if (!Storage.exists(dlPath.c_str())) {
    wsServer->sendTXT(num, "ERROR:File not found");
    return;
  }

  wsDownloadFile = Storage.open(dlPath.c_str());
  if (!wsDownloadFile || wsDownloadFile.isDirectory()) {
    if (wsDownloadFile) wsDownloadFile.close();
    wsServer->sendTXT(num, "ERROR:Cannot open file");
    return;
  }

  wsDownloadSize = wsDownloadFile.size();
  wsDownloadSent = 0;
  wsDownloadClientNum = num;
  wsDownloadInProgress = true;

  // Extract filename from path
  char nameBuf[128] = {0};
  String fname = "download";
  if (wsDownloadFile.getName(nameBuf, sizeof(nameBuf))) {
    fname = nameBuf;
  }

  char readyBuf[192];
  snprintf(readyBuf, sizeof(readyBuf), "DLREADY:%s:%zu", fname.c_str(), wsDownloadSize);
  wsServer->sendTXT(num, readyBuf);
  LOG_DBG("WS", "Starting download: %s (%d bytes)", fname.c_str(), wsDownloadSize);
}

void CrossPointWebServer::handleWsUploadStart(uint8_t num, const String& msg) {
  // Reject any START while an upload is already active to prevent
  // leaking the open wsUploadFile handle (owning client re-START included)
  if (wsUploadInProgress) {
    wsServer->sendTXT(num, "ERROR:Upload already in progress");
    return;
  }

  // Parse: START:<filename>:<size>:<path>
  int firstColon = msg.indexOf(':', 6);
  int secondColon = msg.indexOf(':', firstColon + 1);

  if (firstColon <= 0 || secondColon <= 0) {
    wsServer->sendTXT(num, "ERROR:Invalid START format");
    return;
  }

  wsUploadFileName = msg.substring(6, firstColon);
  wsUploadSize = msg.substring(firstColon + 1, secondColon).toInt();
  wsUploadPath = normalizeWebPath(msg.substring(secondColon + 1));
  wsUploadReceived = 0;
  wsUploadLastProgressSent = 0;
  wsUploadStartTime = millis();

  // Sanitize filename: strip path separators to prevent traversal
  wsUploadFileName.replace("/", "");
  wsUploadFileName.replace("\\", "");
  wsUploadFileName.replace("..", "");

  // Build file path
  String filePath = wsUploadPath;
  if (!filePath.endsWith("/")) filePath += "/";
  filePath += wsUploadFileName;

  LOG_DBG("WS", "Starting upload: %s (%d bytes) to %s", wsUploadFileName.c_str(), wsUploadSize,
          filePath.c_str());

  // Check if file exists and remove it
  esp_task_wdt_reset();
  if (Storage.exists(filePath.c_str())) {
    Storage.remove(filePath.c_str());
  }

  // Open file for writing
  esp_task_wdt_reset();
  if (!Storage.openFileForWrite("WS", filePath, wsUploadFile)) {
    wsServer->sendTXT(num, "ERROR:Failed to create file");
    wsUploadInProgress = false;
    wsUploadClientNum = 255;
    return;
  }
  esp_task_wdt_reset();

  wsUploadClientNum = num;
  wsUploadInProgress = true;
  wsServer->sendTXT(num, "READY");
}

void CrossPointWebServer::handleWsUploadData(uint8_t num, uint8_t* payload, size_t length) {
  if (!wsUploadInProgress || !wsUploadFile || num != wsUploadClientNum) {
    wsServer->sendTXT(num, "ERROR:No upload in progress");
    return;
  }

  // Check for upload overflow
  size_t remaining = wsUploadSize - wsUploadReceived;
  if (length > remaining) {
    abortWsUpload("WS");
    wsServer->sendTXT(num, "ERROR:Upload overflow");
    return;
  }

  // Write binary data directly to file
  esp_task_wdt_reset();
  size_t written = wsUploadFile.write(payload, length);
  esp_task_wdt_reset();

  if (written != length) {
    abortWsUpload("WS");
    wsServer->sendTXT(num, "ERROR:Write failed - disk full?");
    return;
  }

  wsUploadReceived += written;

  // Send progress update (every 64KB or at end)
  if (wsUploadReceived - wsUploadLastProgressSent >= 65536 || wsUploadReceived >= wsUploadSize) {
    char progressBuf[64];
    snprintf(progressBuf, sizeof(progressBuf), "PROGRESS:%zu:%zu", wsUploadReceived, wsUploadSize);
    wsServer->sendTXT(num, progressBuf);
    wsUploadLastProgressSent = wsUploadReceived;
  }

  // Check if upload complete
  if (wsUploadReceived >= wsUploadSize) {
    wsUploadFile.close();
    wsUploadInProgress = false;
    wsUploadClientNum = 255;

    wsLastCompleteName = wsUploadFileName;
    wsLastCompleteSize = wsUploadSize;
    wsLastCompleteAt = millis();

    unsigned long elapsed = millis() - wsUploadStartTime;
    float kbps = (elapsed > 0) ? (wsUploadSize / 1024.0) / (elapsed / 1000.0) : 0;

    LOG_DBG("WS", "Upload complete: %s (%d bytes in %lu ms, %.1f KB/s)", wsUploadFileName.c_str(), wsUploadSize,
            elapsed, kbps);

    // Clear epub cache to prevent stale metadata issues when overwriting files
    String filePath = wsUploadPath;
    if (!filePath.endsWith("/")) filePath += "/";
    filePath += wsUploadFileName;
    clearBookCacheIfNeeded(filePath);

    wsServer->sendTXT(num, "DONE");
    wsUploadLastProgressSent = 0;
  }
}
