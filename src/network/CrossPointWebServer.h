/**
 * @file CrossPointWebServer.h
 * @brief HTTP + WebSocket server for Wi-Fi file transfer and device settings.
 *
 * When Wi-Fi is connected, provides a web UI for uploading/downloading books,
 * browsing the SD card, creating folders, renaming/deleting files, adjusting
 * settings, and triggering OTA updates. File transfers use both HTTP POST
 * (multipart upload) and WebSocket binary (chunked download/upload).
 *
 * UDP broadcast discovery (port 54982+) lets the web UI auto-detect the device.
 * The server runs on the main loop via handleClient() — not on a separate task.
 */
#pragma once

#include <HalStorage.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <WiFiUdp.h>

#include <memory>
#include <string>
#include <vector>

// Structure to hold file information
struct FileInfo {
  String name;
  size_t size;
  bool isEpub;
  bool isBmp;
  bool isPxc;
  bool isDirectory;
};

class CrossPointWebServer {
 public:
  struct WsUploadStatus {
    bool inProgress = false;
    size_t received = 0;
    size_t total = 0;
    std::string filename;
    std::string lastCompleteName;
    size_t lastCompleteSize = 0;
    unsigned long lastCompleteAt = 0;
  };

  // Used by POST upload handler
  struct UploadState {
    FsFile file;
    String fileName;
    String path = "/";
    size_t size = 0;
    bool success = false;
    String error = "";

    // Upload write buffer - batches small writes into larger SD card operations
    // 4KB is a good balance: large enough to reduce syscall overhead, small enough
    // to keep individual write times short and avoid watchdog issues
    static constexpr size_t UPLOAD_BUFFER_SIZE = 4096;  // 4KB buffer
    // Lazily allocated on UPLOAD_FILE_START, freed on END/ABORTED. Keeping it
    // resident across the entire web-server lifetime cost ~4 KB of always-on
    // heap on a 123 KB chip; we only need it during an active POST.
    std::vector<uint8_t> buffer;
    size_t bufferPos = 0;
  } upload;

  CrossPointWebServer();
  ~CrossPointWebServer();

  // Start the web server (call after WiFi is connected)
  void begin();

  // Stop the web server
  void stop();

  // Call this periodically to handle client requests
  void handleClient();

  // Check if server is running
  bool isRunning() const { return running; }

  WsUploadStatus getWsUploadStatus() const;

  // Get the port number
  uint16_t getPort() const { return port; }

 private:
  std::unique_ptr<WebServer> server = nullptr;
  std::unique_ptr<WebSocketsServer> wsServer = nullptr;
  bool running = false;
  bool apMode = false;  // true when running in AP mode, false for STA mode
  uint16_t port = 80;
  uint16_t wsPort = 81;  // WebSocket port
  WiFiUDP udp;
  bool udpActive = false;

  // WebSocket upload state
  void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length);
  void handleWsDownloadRequest(uint8_t num, const String& msg);
  void handleWsUploadStart(uint8_t num, const String& msg);
  void handleWsUploadData(uint8_t num, uint8_t* payload, size_t length);
  void pumpWsDownload();
  static void wsEventCallback(uint8_t num, WStype_t type, uint8_t* payload, size_t length);

  // File scanning
  void scanFiles(const char* path, const std::function<void(FileInfo)>& callback) const;
  String formatFileSize(size_t bytes) const;
  bool isEpubFile(const String& filename) const;
  bool isBmpFile(const String& filename) const;
  bool isPxcFile(const String& filename) const;

  // Request handlers
  void handleRoot() const;
  void handleNotFound() const;
  void handleStatus() const;
  void handleFileList() const;
  void handleFileListData() const;
  void handleDownload() const;
  void handlePreview() const;
  void handleUpload(UploadState& state) const;
  void handleUploadPost(UploadState& state) const;
  void handleCreateFolder() const;
  void handleRename() const;
  void handleMove() const;
  void handleDelete() const;
  void handleJszip() const;
  void handleOpentypeJs() const;
  void handlePakoJs() const;
  void handleBrutalistCss() const;
  void handleI18nDict() const;
  void abortWsUpload(const char* tag);

  // Font-management handlers.
  //  GET  /fonts              — FontsPage.html (placeholder until slice 2b)
  //  GET  /api/fonts          — JSON listing of installed families
  //  POST /api/fonts/upload   — multipart: writes one CPBN .bin to
  //                             /custom-font/<family>/<variant>_<size>.bin
  //                             atomically via a .tmp sidecar.
  //  POST /api/fonts/delete   — form fields: family [, size]
  struct FontUploadState {
    HalFile file;
    String family;  // validated family name (subdir under /custom-font/)
    String tmpPath;
    String finalPath;
    uint8_t variant = 0;
    uint16_t sizePt = 0;
    size_t size = 0;
    bool success = false;
    String error;
    static constexpr size_t UPLOAD_BUFFER_SIZE = 4096;
    // Lazily allocated on UPLOAD_FILE_START, freed on END/ABORTED — same
    // rationale as UploadState::buffer above.
    std::vector<uint8_t> buffer;
    size_t bufferPos = 0;
  } fontUpload;

  void handleFontsPage() const;
  void handleGetFonts() const;
  void handleUploadFont(FontUploadState& state);
  void handleUploadFontPost(FontUploadState& state);
  void handleDeleteFont();

  void handleSleepConverterPage() const;
};
