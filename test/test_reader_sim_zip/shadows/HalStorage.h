// Host shadow of HalStorage — a stdio-backed filesystem so the REAL ZipFile
// can open + read + inflate a real .epub on the host. Declares exactly the
// surface ZipFile (and the test) use: a move-only HalFile (== FsFile) with
// read/seek/size/position/available/close, and a Storage singleton with
// openFileForRead/open/exists. No FreeRTOS, no SdFat, no pimpl.
#pragma once

#include <Print.h>
#include <common/FsApiConstants.h>

#include <cstdint>
#include <cstdio>
#include <string>

#include "WString.h"

class HalFile : public Print {
 public:
  HalFile() = default;
  ~HalFile();
  HalFile(HalFile&& other) noexcept;
  HalFile& operator=(HalFile&& other) noexcept;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  // Open a real host file for reading. Returns true on success.
  bool openForRead(const char* path);

  size_t size();
  bool seek(size_t pos);
  bool seekCur(int64_t offset);
  bool seekSet(size_t offset);
  int available() const;
  size_t position() const;
  int read(void* buf, size_t count);
  int read();  // single byte, -1 on EOF
  size_t write(const void* buf, size_t count);
  size_t write(uint8_t b) override;
  size_t getName(char* name, size_t len);
  bool close();
  bool isOpen() const { return f_ != nullptr; }
  explicit operator bool() const { return f_ != nullptr; }

 private:
  FILE* f_ = nullptr;
  size_t size_ = 0;
};

// Downstream EPUB/ZipFile code uses FsFile; alias it to the host HalFile.
#define FsFile HalFile

class HalStorage {
 public:
  static HalStorage& getInstance();

  bool openFileForRead(const char* moduleName, const char* path, HalFile& file);
  bool openFileForRead(const char* moduleName, const std::string& path, HalFile& file);
  HalFile open(const char* path, oflag_t oflag = O_RDONLY);
  bool exists(const char* path);
};

#define Storage HalStorage::getInstance()
