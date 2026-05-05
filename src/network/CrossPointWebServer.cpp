#include "CrossPointWebServer.h"

#include <ArduinoJson.h>
#include <BookFingerprint.h>
#include <EpdBinFontLoader.h>
#include <EpdBinFormat.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Txt.h>
#include <WiFi.h>
#include <Xtc.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <unordered_set>
#include <utility>

#include "../activities/home/LibraryListingFilter.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "Paths.h"
#include "RecentBooksStore.h"
#include "SettingsList.h"
#include "fonts/CustomBinFontManager.h"
#if __has_include("WebDAVHandler.h")
#include "WebDAVHandler.h"
#define CROSSPOINT_HAS_WEBDAV 1
#endif
#include "I18n.h"
#include "WebI18nDict.generated.h"
#include "html/FilesPageHtml.generated.h"
#include "html/FontsPageHtml.generated.h"
#include "html/HomePageHtml.generated.h"
#include "html/SleepConverterPageHtml.generated.h"
#include "html/brutalistCss.generated.h"
#include "html/js/jszip_minJs.generated.h"
#include "html/js/opentype_minJs.generated.h"
#include "html/js/pako_minJs.generated.h"
#include "network/ws/WsUploadSession.h"
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

// RFC #24 globals. Session owns the WS upload state machine, created in
// begin(), destroyed in stop().
std::unique_ptr<crosspoint::ws::WsUploadSession> g_wsUploadSession;

// Compute the cache directory path for a book file (returns empty if not a book).
// Uses content-based fingerprint so the path is stable across file moves.
std::string bookCachePath(const std::string& filePath) {
  if (StringUtils::checkFileExtension(filePath, ".epub")) {
    return BookFingerprint::cacheDirName("epub", filePath, Paths::kDataDir);
  }
  if (StringUtils::checkFileExtension(filePath, ".xtc") || StringUtils::checkFileExtension(filePath, ".xtch")) {
    return BookFingerprint::cacheDirName("xtc", filePath, Paths::kDataDir);
  }
  if (StringUtils::checkFileExtension(filePath, ".txt") || StringUtils::checkFileExtension(filePath, ".md")) {
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

  LOG_DIAG("WEB", "[HEAP] s0_begin_entry free=%u min=%u", ESP.getFreeHeap(), ESP.getMinFreeHeap());
  LOG_DBG("WEB", "Network mode: %s", apMode ? "AP" : "STA");

  LOG_DBG("WEB", "Creating web server on port %d...", port);
  server.reset(new WebServer(port));
  LOG_DIAG("WEB", "[HEAP] s1_after_new_WebServer free=%u min=%u", ESP.getFreeHeap(), ESP.getMinFreeHeap());

  // Disable WiFi sleep to improve responsiveness and prevent 'unreachable' errors.
  // This is critical for reliable web server operation on ESP32.
  WiFi.setSleep(false);
  LOG_DIAG("WEB", "[HEAP] s2_after_setSleep_false free=%u min=%u", ESP.getFreeHeap(), ESP.getMinFreeHeap());

  // Ensure the SDK reconnects the STA link if the router flickers or
  // the client device briefly drops. Keeps the web session alive
  // across the blip; the activity's loss-recovery loop relies on
  // these driver retries before abandoning after WIFI_ABANDON_MS.
  if (!apMode) WiFi.setAutoReconnect(true);

  // Note: WebServer class doesn't have setNoDelay() in the standard ESP32 library.
  // We rely on disabling WiFi sleep for responsiveness.

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

  // opentype.js + pako for the in-browser font baker on /fonts. Bundled in
  // PROGMEM so the page works in AP mode where the phone has no internet.
  server->on("/js/opentype.min.js", HTTP_GET, [this] { handleOpentypeJs(); });
  server->on("/js/pako.min.js", HTTP_GET, [this] { handlePakoJs(); });

  server->on("/css/brutalist.css", HTTP_GET, [this] { handleBrutalistCss(); });

  // Web UI translation dict — picks language from SETTINGS.uiLanguage at request time.
  server->on("/api/i18n.json", HTTP_GET, [this] { handleI18nDict(); });

  // Custom-font endpoints.
  // /upload takes the file body via multipart; the family / variant /
  // size tuple rides in query params because form fields aren't
  // parsed until after the upload callback finishes.
  server->on("/fonts", HTTP_GET, [this] { handleFontsPage(); });
  server->on("/sleep-converter", HTTP_GET, [this] { handleSleepConverterPage(); });
  server->on("/api/fonts", HTTP_GET, [this] { handleGetFonts(); });
  server->on(
      "/api/fonts/upload", HTTP_POST, [this] { handleUploadFontPost(fontUpload); },
      [this] { handleUploadFont(fontUpload); });
  server->on("/api/fonts/delete", HTTP_POST, [this] { handleDeleteFont(); });

  server->onNotFound([this] { handleNotFound(); });
  LOG_DIAG("WEB", "[HEAP] s3_after_route_setup free=%u min=%u", ESP.getFreeHeap(), ESP.getMinFreeHeap());

#if defined(CROSSPOINT_HAS_WEBDAV)
  // Collect WebDAV headers and register handler
  const char* davHeaders[] = {"Depth", "Destination", "Overwrite", "If", "Lock-Token", "Timeout"};
  server->collectHeaders(davHeaders, 6);
  server->addHandler(new WebDAVHandler());  // WebDAVHandler is deleted by WebServer when server stops
  LOG_DBG("WEB", "WebDAV handler initialized");
#endif
  server->begin();
  LOG_DIAG("WEB", "[HEAP] s4_after_server_begin free=%u min=%u", ESP.getFreeHeap(), ESP.getMinFreeHeap());

  // WebSocket server, UDP discovery, and mDNS responder are all skipped
  // unconditionally for v2.0.0. The combined ~10 KB resident commit was
  // pushing the heap pool below the fragmentation floor needed for parallel
  // HTTP asset fetches on phone browsers in STA mode (free=24 KB baseline
  // collapses to <4 KB largest-block after fetching opentype.js + pako.js).
  // HTTP POST upload covers the WS fast-binary use case; UDP discovery is
  // unused by any current client; mDNS is replaced by typed IP entry.
  // Document in release notes; revisit when async esp_http_server lands in
  // a later release.
  LOG_DBG("WEB", "Skipping WebSocket server (heap budget — HTTP POST only)");
  LOG_DIAG("WEB", "[HEAP] s5_after_wsServer_begin free=%u min=%u apMode=%d", ESP.getFreeHeap(), ESP.getMinFreeHeap(),
           apMode ? 1 : 0);

  udpActive = false;
  LOG_DIAG("WEB", "[HEAP] s6_after_udp_begin free=%u min=%u udpActive=%d", ESP.getFreeHeap(), ESP.getMinFreeHeap(),
           udpActive ? 1 : 0);

  // WsUploadSession deps bind the file-scope upload state (wsUploadFile,
  // wsUploadFileName, wsUploadPath, wsUploadSize, wsLastCompleteName/Size/At)
  // so the HTTP status endpoints continue to read them.
  crosspoint::ws::WsUploadDeps wsDeps;
  wsDeps.beginWrite = [](const std::string& path, const std::string& name) {
    wsUploadPath = path.c_str();
    wsUploadFileName = name.c_str();
    String filePath = wsUploadPath;
    if (!filePath.endsWith("/")) filePath += "/";
    filePath += wsUploadFileName;
    esp_task_wdt_reset();
    if (Storage.exists(filePath.c_str())) Storage.remove(filePath.c_str());
    esp_task_wdt_reset();
    if (!Storage.openFileForWrite("WS", filePath, wsUploadFile)) return false;
    esp_task_wdt_reset();
    wsUploadSize = 0;
    wsUploadReceived = 0;
    wsUploadLastProgressSent = 0;
    wsUploadStartTime = millis();
    wsUploadInProgress = true;
    return true;
  };
  wsDeps.writeBytes = [](const uint8_t* data, size_t len) {
    esp_task_wdt_reset();
    const size_t written = wsUploadFile.write(data, len);
    esp_task_wdt_reset();
    return written == len;
  };
  wsDeps.closeAndDelete = [] {
    wsUploadFile.close();
    String filePath = wsUploadPath;
    if (!filePath.endsWith("/")) filePath += "/";
    filePath += wsUploadFileName;
    Storage.remove(filePath.c_str());
    wsUploadInProgress = false;
    wsUploadClientNum = 255;
    wsUploadLastProgressSent = 0;
  };
  wsDeps.closeFinalize = [] {
    wsUploadFile.close();
    wsLastCompleteName = wsUploadFileName;
    wsLastCompleteSize = wsUploadReceived;
    wsLastCompleteAt = millis();
    wsUploadInProgress = false;
    wsUploadClientNum = 255;
    wsUploadLastProgressSent = 0;
  };
  wsDeps.sendText = [this](uint8_t client, const std::string& text) {
    if (wsServer) wsServer->sendTXT(client, text.c_str());
  };
  wsDeps.nowMs = [] { return static_cast<uint32_t>(millis()); };
  wsDeps.clearBookCache = [](const std::string& path, const std::string& name) {
    String full = path.c_str();
    if (!full.endsWith("/")) full += "/";
    full += name.c_str();
    clearBookCacheIfNeeded(full);
  };
  g_wsUploadSession.reset(new crosspoint::ws::WsUploadSession(std::move(wsDeps)));

  running = true;

  LOG_DBG("WEB", "Web server started on port %d", port);
  // Show the correct IP based on network mode
  const String ipAddr = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  LOG_DBG("WEB", "Access at http://%s/", ipAddr.c_str());
  LOG_DBG("WEB", "WebSocket at ws://%s:%d/", ipAddr.c_str(), wsPort);
  LOG_DIAG("WEB", "[HEAP] s7_begin_done free=%u min=%u", ESP.getFreeHeap(), ESP.getMinFreeHeap());
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

// The three large JS bundles are pinned to STA mode. In AP mode their parallel
// fetch wedges the heap (see comment on kFontsPageApStub above). Returning
// 503 immediately frees the socket without committing tx buffers to the gzip
// payload, so a stale browser cache cannot resurrect the wedge.
void CrossPointWebServer::handleJszip() const {
  if (apMode) {
    server->send(503, "text/plain", "Asset disabled in hotspot mode");
    return;
  }
  server->sendHeader("Content-Encoding", "gzip");
  server->sendHeader("Cache-Control", "public, max-age=86400");
  server->send_P(200, "application/javascript", jszip_minJs, jszip_minJsCompressedSize);
  LOG_DBG("WEB", "Served jszip.min.js");
}

void CrossPointWebServer::handleOpentypeJs() const {
  if (apMode) {
    server->send(503, "text/plain", "Asset disabled in hotspot mode");
    return;
  }
  server->sendHeader("Content-Encoding", "gzip");
  server->sendHeader("Cache-Control", "public, max-age=86400");
  server->send_P(200, "application/javascript", opentype_minJs, opentype_minJsCompressedSize);
  LOG_DBG("WEB", "Served opentype.min.js");
}

void CrossPointWebServer::handlePakoJs() const {
  if (apMode) {
    server->send(503, "text/plain", "Asset disabled in hotspot mode");
    return;
  }
  server->sendHeader("Content-Encoding", "gzip");
  server->sendHeader("Cache-Control", "public, max-age=86400");
  server->send_P(200, "application/javascript", pako_minJs, pako_minJsCompressedSize);
  LOG_DBG("WEB", "Served pako.min.js");
}

void CrossPointWebServer::handleBrutalistCss() const {
  // Long cache lets pages share this asset across navigations without
  // re-fetching. Bumped by the build pipeline whenever the .generated.h
  // changes, so etag-style invalidation isn't needed.
  server->sendHeader("Content-Encoding", "gzip");
  server->sendHeader("Cache-Control", "public, max-age=86400");
  server->send_P(200, "text/css", brutalistCss, brutalistCssCompressedSize);
  LOG_DBG("WEB", "Served brutalist.css");
}

void CrossPointWebServer::handleI18nDict() const {
  // Pick the current device UI language. Cache disabled because the same URL
  // returns different bytes when the user changes language on the device.
  const auto lang = static_cast<Language>(SETTINGS.uiLanguage);
  const WebI18nBlob blob = getWebI18nBlob(lang);
  server->sendHeader("Content-Encoding", "gzip");
  server->sendHeader("Cache-Control", "no-cache");
  server->send_P(200, "application/json", reinterpret_cast<const char*>(blob.data), blob.size);
  LOG_DBG("WEB", "Served /api/i18n.json (lang=%u, %u B gz)", static_cast<unsigned>(lang),
          static_cast<unsigned>(blob.size));
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
  if (g_wsUploadSession) g_wsUploadSession->abort("server stopping");
  if (wsDownloadInProgress && wsDownloadFile) {
    wsDownloadFile.close();
    wsDownloadInProgress = false;
  }

  // Release session before the WS server goes away, so any pending callbacks
  // have no session to delegate to (checked at dispatch).
  g_wsUploadSession.reset();

  // Stop WebSocket server
  if (wsServer) {
    LOG_DBG("WEB", "Stopping WebSocket server...");
    // Disconnect each client explicitly so FIN frames go out before the
    // listening socket is torn down. Without this, lwIP holds onto the
    // per-client socket allocations until WiFi.mode(WIFI_OFF) yanks them,
    // which fragments heap on repeated enter/exit cycles.
    for (uint8_t i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; ++i) {
      wsServer->disconnect(i);
    }
    for (int k = 0; k < 3; ++k) {
      wsServer->loop();
      delay(5);
    }
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

  // Reset every file-scope global. Without this, Arduino String backing
  // buffers and stale primitive state survive across server lifecycles —
  // the next begin() then sees less heap than the last.
  if (wsUploadFile) wsUploadFile.close();
  if (wsDownloadFile) wsDownloadFile.close();
  wsUploadSize = 0;
  wsUploadReceived = 0;
  wsUploadLastProgressSent = 0;
  wsUploadStartTime = 0;
  wsUploadInProgress = false;
  wsUploadClientNum = 255;
  wsDownloadSize = 0;
  wsDownloadSent = 0;
  wsDownloadInProgress = false;
  wsDownloadClientNum = 0;
  wsLastCompleteSize = 0;
  wsLastCompleteAt = 0;
  // String::operator=("") would null the contents but keep the backing buffer.
  // Swapping with a fresh empty String forces the old buffer's destructor.
  {
    String tmp;
    std::swap(tmp, wsUploadFileName);
  }
  {
    String tmp;
    std::swap(tmp, wsUploadPath);
  }
  {
    String tmp;
    std::swap(tmp, wsLastCompleteName);
  }

  LOG_DIAG("WEB", "[HEAP] stop() done free=%u min=%u", ESP.getFreeHeap(), ESP.getMinFreeHeap());
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

  // Heap watchdog (B6): when free heap stays below the danger floor for >3 s,
  // kill any in-flight WS download/upload to release their buffers and break
  // the lwIP retransmit deadlock that otherwise pins ~8 KB of un-ACK'd pbufs
  // until reboot. STA-side last-line defense; AP path already trimmed.
  static unsigned long lowHeapSinceMs = 0;
  static unsigned long lastHeapDiagMs = 0;
  constexpr uint32_t kLowHeapDangerBytes = 8000;
  constexpr uint32_t kLowHeapDurationMs = 3000;
  const uint32_t freeNow = ESP.getFreeHeap();
  const unsigned long nowMs = millis();
  if (freeNow < kLowHeapDangerBytes) {
    if (lowHeapSinceMs == 0) lowHeapSinceMs = nowMs;
    if ((nowMs - lastHeapDiagMs) > 1000) {
      LOG_DIAG("WEB", "[HEAP] low free=%u dur=%lu", freeNow, nowMs - lowHeapSinceMs);
      lastHeapDiagMs = nowMs;
    }
    if ((nowMs - lowHeapSinceMs) > kLowHeapDurationMs) {
      LOG_DIAG("WEB", "[HEAP] danger sustained free=%u — aborting in-flight WS", freeNow);
      if (wsDownloadInProgress) {
        if (wsServer) wsServer->disconnect(wsDownloadClientNum);
        if (wsDownloadFile) wsDownloadFile.close();
        wsDownloadInProgress = false;
      }
      if (g_wsUploadSession) g_wsUploadSession->abort("low_heap");
      lowHeapSinceMs = nowMs;  // reset so we don't spam aborts every tick
    }
  } else if (lowHeapSinceMs != 0) {
    LOG_DIAG("WEB", "[HEAP] recovered free=%u after %lu ms low", freeNow, nowMs - lowHeapSinceMs);
    lowHeapSinceMs = 0;
  }

  server->handleClient();

  // Handle WebSocket events
  if (wsServer) {
    wsServer->loop();
  }

  pumpWsDownload();

  // Enforce the idle timeout on WS uploads. If a client dies without sending
  // TCP FIN the session would otherwise wedge until reboot.
  if (g_wsUploadSession) g_wsUploadSession->tick(static_cast<uint32_t>(millis()));

  // Respond to discovery broadcasts
  if (udpActive) {
    int packetSize = udp.parsePacket();
    if (packetSize > 0) {
      char buffer[16];
      int len = udp.read(buffer, sizeof(buffer) - 1);
      if (len > 0) {
        buffer[len] = '\0';
        if (strcmp(buffer, "hello") == 0) {
          const char* hostname = WiFi.getHostname();
          if (!hostname || hostname[0] == '\0') hostname = "crosspoint";
          char msg[128];
          const int msgLen = snprintf(msg, sizeof(msg), "crosspoint (on %s);%d", hostname, wsPort);
          udp.beginPacket(udp.remoteIP(), udp.remotePort());
          udp.write(reinterpret_cast<const uint8_t*>(msg), static_cast<size_t>(msgLen > 0 ? msgLen : 0));
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

void CrossPointWebServer::handleSleepConverterPage() const {
  sendHtmlContent(server.get(), SleepConverterPageHtml, sizeof(SleepConverterPageHtml));
  LOG_DBG("WEB", "Served sleep converter page");
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

// Case-insensitive tail-compare on a raw char buffer. Avoids String+toLowerCase
// allocations in hot per-file loops.
static bool hasExtCI(const char* name, size_t nameLen, const char* ext, size_t extLen) {
  if (nameLen < extLen) return false;
  const char* t = name + nameLen - extLen;
  for (size_t i = 0; i < extLen; i++) {
    char a = t[i];
    if (a >= 'A' && a <= 'Z') a += 32;
    if (a != ext[i]) return false;
  }
  return true;
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
  uint16_t seen = 0;
  while (file) {
    file.getName(name, sizeof(name));
    const size_t nameLen = strlen(name);

    // Skip hidden items (starting with ".")
    bool shouldHide = !SETTINGS.showHiddenFiles && nameLen > 0 && name[0] == '.';

    // Check against explicitly hidden items list
    if (!shouldHide) {
      for (size_t i = 0; i < HIDDEN_ITEMS_COUNT; i++) {
        if (strcmp(name, HIDDEN_ITEMS[i]) == 0) {
          shouldHide = true;
          break;
        }
      }
    }

    if (!shouldHide) {
      FileInfo info;
      info.name = String(name);
      info.isDirectory = file.isDirectory();

      if (info.isDirectory) {
        info.size = 0;
        info.isEpub = false;
        info.isBmp = false;
        info.isPxc = false;
      } else {
        info.size = file.size();
        info.isEpub = hasExtCI(name, nameLen, ".epub", 5);
        info.isBmp = hasExtCI(name, nameLen, ".bmp", 4);
        info.isPxc = hasExtCI(name, nameLen, ".pxc", 4);
      }

      callback(info);
    }

    file.close();
    // Yield + WDT reset every 16 files instead of every file — per-file overhead
    // dominated scan time on big dirs; WDT window is seconds, 16 iters is safe.
    if ((++seen & 0x0F) == 0) {
      yield();
      esp_task_wdt_reset();
    }
    file = root.openNextFile();
  }
  root.close();
}

bool CrossPointWebServer::isEpubFile(const String& filename) const {
  return hasExtCI(filename.c_str(), filename.length(), ".epub", 5);
}

bool CrossPointWebServer::isBmpFile(const String& filename) const {
  return hasExtCI(filename.c_str(), filename.length(), ".bmp", 4);
}

bool CrossPointWebServer::isPxcFile(const String& filename) const {
  return hasExtCI(filename.c_str(), filename.length(), ".pxc", 4);
}

void CrossPointWebServer::handleFileList() const {
  sendHtmlContent(server.get(), FilesPageHtml, sizeof(FilesPageHtml));
}

void CrossPointWebServer::handleFileListData() const {
  LOG_DIAG("WEB", "/api/files entry, free=%u min=%u", ESP.getFreeHeap(), ESP.getMinFreeHeap());
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

  // Pass 1: collect file basenames only (no full FileInfo heap cost) so we can
  // run the EPUB-cache PXC filter. For a 1000-file /sleep folder this is ~32 KB
  // of small strings — well within budget. The full vector<FileInfo> approach
  // of commit 50f2b01 peaked ~150 KB transient, exceeding the ~54 KB free heap.
  std::vector<std::string> names;
  scanFiles(currentPath.c_str(), [&names](const FileInfo& info) {
    if (!info.isDirectory) names.push_back(info.name.c_str());
  });

  LibraryListingFilter::filterEpubCachePxc(names);

  // Move survivors into a hash set for O(1) lookup; release the names vector
  // before pass 2 so only the set is live during JSON streaming.
  std::unordered_set<std::string> surviving(names.begin(), names.end());
  std::vector<std::string>().swap(names);

  // Pass 2: stream JSON directly from a second scanFiles pass — one FileInfo
  // on the stack at a time. Directories always pass through (they are not
  // subject to the EPUB-cache filter).
  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[");
  char output[1024];  // Must fit JSON with FAT32 max filename (255 chars) + metadata
  constexpr size_t outputSize = sizeof(output);
  bool seenFirst = false;
  JsonDocument doc;
  // Batch entries into a single buffer before flushing. Each sendContent()
  // emits a chunk + TCP flush; per-file flushes dominated /api/files wall-time.
  String batch;
  batch.reserve(3072);
  constexpr size_t kFlushAt = 2048;

  scanFiles(currentPath.c_str(), [this, &output, &doc, &seenFirst, &batch, &surviving](const FileInfo& info) {
    // Skip file entries that didn't survive the filter; always emit directories.
    if (!info.isDirectory && surviving.find(info.name.c_str()) == surviving.end()) {
      return;
    }

    doc.clear();
    doc["name"] = info.name;
    doc["size"] = info.size;
    doc["isDirectory"] = info.isDirectory;
    doc["isEpub"] = info.isEpub;
    doc["isBmp"] = info.isBmp;
    doc["isPxc"] = info.isPxc;

    const size_t written = serializeJson(doc, output, outputSize);
    if (written >= outputSize) {
      // JSON output truncated; skip this entry to avoid sending malformed JSON
      LOG_DBG("WEB", "Skipping file entry with oversized JSON for name: %s", info.name.c_str());
      return;
    }

    if (seenFirst) {
      batch += ',';
    } else {
      seenFirst = true;
    }
    batch.concat(output, written);

    if (batch.length() >= kFlushAt) {
      server->sendContent(batch);
      batch = "";
      batch.reserve(3072);
    }
  });

  if (batch.length()) server->sendContent(batch);
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

  // Sanity cap: FAT32 allows up to 4 GB per file, but a garbage directory
  // entry could report terabytes and we'd advertise that Content-Length to
  // the client, then loop forever reading zero bytes. Reject anything above
  // 2 GB — far larger than any legitimate book/image on this device.
  constexpr size_t DL_MAX_FILE_SIZE = 2ull * 1024 * 1024 * 1024;
  if (fileSize > DL_MAX_FILE_SIZE) {
    LOG_ERR("WEB", "Download refused: implausible file size %u bytes", (unsigned)fileSize);
    file.close();
    server->send(500, "text/plain", "file size invalid");
    return;
  }

  // Allocate the copy buffer BEFORE committing to a 200 response. If malloc
  // fails after headers are flushed, the client hangs waiting for bytes and
  // only learns about the failure via TCP timeout. Failing early lets us
  // return a proper 503.
  constexpr size_t DL_BUF_SIZE = 4096;
  auto buf = std::make_unique<uint8_t[]>(DL_BUF_SIZE);
  if (!buf) {
    LOG_ERR("WEB", "Download OOM: cannot allocate %d byte buffer", DL_BUF_SIZE);
    file.close();
    server->send(503, "text/plain", "out of memory");
    return;
  }

  // Send exact Content-Length so the browser knows when the download is done.
  // Note: _prepareHeader already adds Connection: close, don't duplicate it.
  server->setContentLength(fileSize);
  server->sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  server->send(200, contentType.c_str(), "");

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
  LOG_DIAG("WEB", "/preview entry, free=%u min=%u path=%s", ESP.getFreeHeap(), ESP.getMinFreeHeap(),
           server->hasArg("path") ? server->arg("path").c_str() : "(none)");
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
    // Pre-flight heap check before vector::resize. Exceptions are disabled
    // (-fno-exceptions), so a bad_alloc inside resize would call terminate()
    // and abort the firmware. Phone-on-STA after fetching the 64 KB JS bundle
    // hits this path: heap can fragment below the buffer size mid-upload.
    if (state.buffer.size() != UploadState::UPLOAD_BUFFER_SIZE) {
      const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
      if (largest < UploadState::UPLOAD_BUFFER_SIZE + 1024) {
        state.error = "out of memory (try again from desktop browser)";
        LOG_ERR("WEB", "[UPLOAD] buffer alloc skipped: largest=%u free=%u min=%u", static_cast<unsigned>(largest),
                ESP.getFreeHeap(), ESP.getMinFreeHeap());
        return;
      }
      state.buffer.resize(UploadState::UPLOAD_BUFFER_SIZE);
    }

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
    std::vector<uint8_t>().swap(state.buffer);
    state.bufferPos = 0;
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
    std::vector<uint8_t>().swap(state.buffer);
    LOG_DBG("WEB", "Upload aborted");
  }
}

void CrossPointWebServer::handleUploadPost(UploadState& state) const {
  LOG_DIAG("WEB", "/upload POST entry, free=%u min=%u", ESP.getFreeHeap(), ESP.getMinFreeHeap());
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

namespace {

// Accept "R" / "B" / "I" / "Z" as browser-side variant tags and map
// them to the enum the manager uses. Returns UINT8_MAX on mismatch.
uint8_t parseVariantTag(const String& v) {
  if (v.equalsIgnoreCase("R") || v.equalsIgnoreCase("regular")) return 0;
  if (v.equalsIgnoreCase("B") || v.equalsIgnoreCase("bold")) return 1;
  if (v.equalsIgnoreCase("I") || v.equalsIgnoreCase("italic")) return 2;
  if (v.equalsIgnoreCase("Z") || v.equalsIgnoreCase("bolditalic")) return 3;
  return 0xFFu;
}

const char* variantTagFor(uint8_t v) {
  switch (v) {
    case 0:
      return "regular";
    case 1:
      return "bold";
    case 2:
      return "italic";
    case 3:
      return "bolditalic";
  }
  return "regular";
}

}  // namespace

// Self-contained no-asset stub for /fonts in AP mode. The full font
// management UI pulls opentype.min.js (~49 KB gzip) + pako.min.js (~15 KB
// gzip) + brutalist.css in parallel; that quadruples concurrent TCP sockets
// against a precompiled lwIP whose per-socket tx buffer commit (5744 B)
// collapses the heap pool to <3 KB and triggers a pbuf alloc-fail storm
// inside server->handleClient(). The stub fits in one TCP segment and has
// zero external references, so the phone makes exactly one request.
static constexpr char kFontsPageApStub[] =
    "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>CrossPoint - Fonts</title>"
    "<style>body{font-family:system-ui,sans-serif;max-width:38em;margin:2em auto;"
    "padding:0 1em;line-height:1.5;color:#111;background:#fafafa}"
    "h1{margin-top:0}code{background:#eee;padding:0 .3em;border-radius:3px}"
    "a{color:#06c}</style></head><body>"
    "<h1>Font management unavailable in hotspot mode</h1>"
    "<p>The font upload page bundles ~70&nbsp;KB of JavaScript that the device "
    "cannot serve over its own access point without exhausting RAM.</p>"
    "<p><strong>To manage fonts:</strong> exit this screen, choose "
    "<em>Join Network</em>, connect to your home Wi-Fi, then re-open the web "
    "server and visit <code>/fonts</code> from a desktop browser.</p>"
    "<p><a href=\"/\">&larr; Back to home</a> &nbsp;&middot;&nbsp; "
    "<a href=\"/files\">Open file browser</a></p>"
    "</body></html>";

void CrossPointWebServer::handleFontsPage() const {
  if (apMode) {
    server->send(200, "text/html", kFontsPageApStub);
    LOG_DBG("WEB", "Served fonts page (AP stub)");
    return;
  }
  sendHtmlContent(server.get(), FontsPageHtml, sizeof(FontsPageHtml));
  LOG_DBG("WEB", "Served fonts page");
}

void CrossPointWebServer::handleGetFonts() const {
  LOG_DIAG("WEB", "/api/fonts entry, free=%u min=%u", ESP.getFreeHeap(), ESP.getMinFreeHeap());
  const auto& mgr = crosspoint::fonts::CustomBinFontManager::instance();
  // Stream chunked instead of building one big buffer. Under heap pressure
  // (AP mode + parallel asset fetches) a contiguous multi-KB allocation for
  // the full body throws bad_alloc and crashes the firmware. Per-family
  // serialization fits in a few hundred bytes and survives a fragmented heap.
  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[");
  bool firstFamily = true;
  for (const auto& fam : mgr.families()) {
    if (!firstFamily) server->sendContent(",");
    firstFamily = false;
    JsonDocument doc;
    doc["name"] = fam.name;
    JsonArray sizes = doc["sizes"].to<JsonArray>();
    for (const auto& sz : fam.sizes) {
      JsonObject o = sizes.add<JsonObject>();
      o["size"] = sz.sizePt;
      JsonArray variants = o["variants"].to<JsonArray>();
      if (sz.hasRegular) variants.add("R");
      if (sz.hasBold) variants.add("B");
      if (sz.hasItalic) variants.add("I");
      if (sz.hasBoldItalic) variants.add("Z");
    }
    std::string out;
    serializeJson(doc, out);
    server->sendContent(out.c_str());
  }
  server->sendContent("]");
  server->sendContent("");  // flush
}

void CrossPointWebServer::handleUploadFont(FontUploadState& state) {
  esp_task_wdt_reset();
  if (!running || !server) return;

  const HTTPUpload& upload = server->upload();

  if (upload.status == UPLOAD_FILE_START) {
    esp_task_wdt_reset();
    state.size = 0;
    state.success = false;
    state.error = "";
    state.bufferPos = 0;
    state.family = "";
    state.tmpPath = "";
    state.finalPath = "";
    // Pre-flight heap check (exceptions disabled — bad_alloc would abort).
    // Phone-on-STA after fetching the 64 KB JS bundle hits this path; heap
    // can fragment below the 4 KB buffer size. Bail gracefully so
    // handleUploadFontPost returns a 400 JSON error instead of crashing.
    if (state.buffer.size() != FontUploadState::UPLOAD_BUFFER_SIZE) {
      const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
      if (largest < FontUploadState::UPLOAD_BUFFER_SIZE + 1024) {
        state.error = "out of memory (try again from desktop browser)";
        LOG_ERR("WEB", "[FONT-UPLOAD] buffer alloc skipped: largest=%u free=%u min=%u", static_cast<unsigned>(largest),
                ESP.getFreeHeap(), ESP.getMinFreeHeap());
        return;
      }
      state.buffer.resize(FontUploadState::UPLOAD_BUFFER_SIZE);
    }

    // Extract (family, variant, size) from the query string — multipart
    // form fields aren't parsed until after the upload callback runs.
    const String family = server->hasArg("family") ? server->arg("family") : String();
    const String variant = server->hasArg("variant") ? server->arg("variant") : String();
    const String sizeStr = server->hasArg("size") ? server->arg("size") : String();

    const std::string familyStd = std::string(family.c_str());
    if (!crosspoint::fonts::isValidFamilyName(familyStd)) {
      state.error = "invalid family name";
      return;
    }
    const uint8_t v = parseVariantTag(variant);
    if (v > 3) {
      state.error = "invalid variant (want R/B/I/Z)";
      return;
    }
    const long sz = sizeStr.toInt();
    if (sz < 25 || sz > 40) {
      state.error = "size must be 25..40";
      return;
    }
    state.family = family;
    state.variant = v;
    state.sizePt = static_cast<uint16_t>(sz);

    const String dir = "/custom-font/" + family;
    Storage.mkdir(dir.c_str());  // idempotent
    state.finalPath = dir + "/" + variantTagFor(v) + "_" + String(state.sizePt) + ".bin";
    state.tmpPath = state.finalPath + ".tmp";

    // Clean up any stale .tmp from a previous interrupted upload.
    if (Storage.exists(state.tmpPath.c_str())) {
      Storage.remove(state.tmpPath.c_str());
    }
    if (!Storage.openFileForWrite("FONTUP", state.tmpPath, state.file)) {
      state.error = "failed to create temp file";
      return;
    }
    LOG_DBG("FONTUP", "START %s variant=%u size=%u → %s", family.c_str(), v, static_cast<unsigned>(state.sizePt),
            state.tmpPath.c_str());
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!state.file || !state.error.isEmpty()) return;
    const uint8_t* data = upload.buf;
    size_t remaining = upload.currentSize;
    while (remaining > 0) {
      const size_t space = FontUploadState::UPLOAD_BUFFER_SIZE - state.bufferPos;
      const size_t toCopy = (remaining < space) ? remaining : space;
      memcpy(state.buffer.data() + state.bufferPos, data, toCopy);
      state.bufferPos += toCopy;
      data += toCopy;
      remaining -= toCopy;
      if (state.bufferPos >= FontUploadState::UPLOAD_BUFFER_SIZE) {
        const int written = state.file.write(state.buffer.data(), state.bufferPos);
        if (written != static_cast<int>(state.bufferPos)) {
          state.error = "write failed";
          state.file.close();
          return;
        }
        state.bufferPos = 0;
      }
    }
    state.size += upload.currentSize;
    // Reject oversized payloads mid-stream so we don't waste SD on garbage.
    if (state.size > crosspoint::binfont::kMaxFileBytes) {
      state.error = "file too large";
      state.file.close();
      Storage.remove(state.tmpPath.c_str());
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (state.file) {
      if (state.bufferPos > 0 && state.error.isEmpty()) {
        const int written = state.file.write(state.buffer.data(), state.bufferPos);
        if (written != static_cast<int>(state.bufferPos)) {
          state.error = "final write failed";
        }
        state.bufferPos = 0;
      }
      state.file.close();
    }
    std::vector<uint8_t>().swap(state.buffer);
    if (!state.error.isEmpty()) return;

    // Validate the CPBN header before committing with the rename.
    std::string verr;
    if (!crosspoint::binfont::EpdBinFontLoader::validateFile(std::string(state.tmpPath.c_str()), &verr)) {
      state.error = String("invalid CPBN file: ") + verr.c_str();
      Storage.remove(state.tmpPath.c_str());
      return;
    }

    // Atomic publish: remove any existing same-name file, then rename.
    if (Storage.exists(state.finalPath.c_str())) Storage.remove(state.finalPath.c_str());
    if (!Storage.rename(state.tmpPath.c_str(), state.finalPath.c_str())) {
      state.error = "rename failed";
      Storage.remove(state.tmpPath.c_str());
      return;
    }
    crosspoint::fonts::CustomBinFontManager::instance().scan();
    state.success = true;
    LOG_INF("FONTUP", "installed %s (%u bytes)", state.finalPath.c_str(), static_cast<unsigned>(state.size));
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (state.file) state.file.close();
    if (!state.tmpPath.isEmpty()) Storage.remove(state.tmpPath.c_str());
    state.error = "upload aborted";
    std::vector<uint8_t>().swap(state.buffer);
    LOG_DBG("FONTUP", "aborted");
  }
}

void CrossPointWebServer::handleUploadFontPost(FontUploadState& state) {
  if (state.success) {
    String body = "{\"ok\":true,\"family\":\"" + state.family + "\",\"variant\":\"" +
                  String(variantTagFor(state.variant)) + "\",\"size\":" + String(state.sizePt) +
                  ",\"bytes\":" + String((uint32_t)state.size) + "}";
    server->send(201, "application/json", body);
  } else {
    const String err = state.error.isEmpty() ? "unknown error" : state.error;
    String body = "{\"ok\":false,\"error\":\"";
    // Escape quotes and backslashes for JSON.
    for (size_t i = 0; i < err.length(); ++i) {
      const char c = err[i];
      if (c == '"' || c == '\\') body += '\\';
      body += c;
    }
    body += "\"}";
    server->send(400, "application/json", body);
  }
}

void CrossPointWebServer::handleDeleteFont() {
  const String family = server->hasArg("family") ? server->arg("family") : String();
  const std::string familyStd = std::string(family.c_str());
  if (!crosspoint::fonts::isValidFamilyName(familyStd)) {
    server->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid family\"}");
    return;
  }

  auto& mgr = crosspoint::fonts::CustomBinFontManager::instance();
  size_t removed = 0;
  if (server->hasArg("size")) {
    const long sz = server->arg("size").toInt();
    if (sz < 25 || sz > 40) {
      server->send(400, "application/json", "{\"ok\":false,\"error\":\"size must be 25..40\"}");
      return;
    }
    removed = mgr.deleteFamilySize(familyStd, static_cast<uint16_t>(sz));
  } else {
    removed = mgr.deleteFamily(familyStd);
  }

  String body = "{\"ok\":true,\"removed\":" + String((uint32_t)removed) + "}";
  server->send(200, "application/json", body);
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
      if (g_wsUploadSession) g_wsUploadSession->onDisconnect(num);
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
        if (g_wsUploadSession) g_wsUploadSession->onStart(num, std::string(msg.c_str()));
      }
      break;
    }

    case WStype_BIN: {
      if (g_wsUploadSession) g_wsUploadSession->onBinary(num, payload, length);
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

  LOG_DBG("WS", "Starting upload: %s (%d bytes) to %s", wsUploadFileName.c_str(), wsUploadSize, filePath.c_str());

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
