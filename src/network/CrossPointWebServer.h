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

#include "network/HttpUploadSink.h"

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
    // Batches small multipart chunks into 4 KB SD writes. Lazily sized on
    // UPLOAD_FILE_START, released on END/ABORTED — keeping it resident across
    // the whole web-server lifetime cost ~4 KB of always-on heap on a 123 KB
    // chip; we only need it during an active POST.
    crosspoint::net::HttpUploadSink sink{UPLOAD_BUFFER_SIZE};
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
  void handleBrutalistCss() const;
  void handleI18nDict() const;
  void abortWsUpload(const char* tag);

  void handleSleepConverterPage() const;

  // Firmware install: browser POSTs raw firmware.bin bytes (no multipart).
  // Bytes stream into the inactive OTA partition via esp_ota_*. On success
  // the device reboots into the new firmware. No TLS — browser handled
  // the GitHub download, device just receives a binary stream.
  struct FirmwareUploadState {
    void* otaHandle = nullptr;  // esp_ota_handle_t (kept as void* to avoid esp_ota_ops.h in header)
    size_t bytesWritten = 0;
    bool started = false;
    bool aborted = false;
    String error;
  } firmwareUpload;

  void handleFirmwareUpload();
  void handleFirmwareUploadDone();
  void handleUpdatePage() const;
  // RFC #160: post-reboot verify target — reports the running version + OTA
  // image state so the browser /update page can confirm the new firmware booted.
  void handleFirmwareStatus() const;
};
