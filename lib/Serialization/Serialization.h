#pragma once
#include <HalStorage.h>

#include <iostream>
#include <type_traits>

namespace serialization {
template <typename T>
static void writePod(std::ostream& os, const T& value) {
#ifdef SIMULATOR
  if constexpr (std::is_same_v<T, size_t>) {
    uint32_t narrow = static_cast<uint32_t>(value);
    os.write(reinterpret_cast<const char*>(&narrow), sizeof(narrow));
    return;
  }
#endif
  os.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
static void writePod(FsFile& file, const T& value) {
#ifdef SIMULATOR
  if constexpr (std::is_same_v<T, size_t>) {
    uint32_t narrow = static_cast<uint32_t>(value);
    file.write(reinterpret_cast<const uint8_t*>(&narrow), sizeof(narrow));
    return;
  }
#endif
  file.write(reinterpret_cast<const uint8_t*>(&value), sizeof(T));
}

template <typename T>
static void readPod(std::istream& is, T& value) {
#ifdef SIMULATOR
  if constexpr (std::is_same_v<T, size_t>) {
    uint32_t narrow = 0;
    is.read(reinterpret_cast<char*>(&narrow), sizeof(narrow));
    value = narrow;
    return;
  }
#endif
  is.read(reinterpret_cast<char*>(&value), sizeof(T));
}

template <typename T>
static void readPod(FsFile& file, T& value) {
#ifdef SIMULATOR
  // On device, size_t is 4 bytes (ESP32-C3 is 32-bit). Cache files on disk
  // were produced with 4-byte size_t fields. The macOS simulator is 64-bit
  // so size_t would natively read 8 bytes, shifting every subsequent field
  // by 4 and corrupting the whole record. Special-case size_t reads so
  // files written by the device still parse correctly.
  if constexpr (std::is_same_v<T, size_t>) {
    uint32_t narrow = 0;
    file.read(reinterpret_cast<uint8_t*>(&narrow), sizeof(narrow));
    value = narrow;
    return;
  }
#endif
  file.read(reinterpret_cast<uint8_t*>(&value), sizeof(T));
}

static void writeString(std::ostream& os, const std::string& s) {
  const uint32_t len = s.size();
  writePod(os, len);
  os.write(s.data(), len);
}

static void writeString(FsFile& file, const std::string& s) {
  const uint32_t len = s.size();
  writePod(file, len);
  file.write(reinterpret_cast<const uint8_t*>(s.data()), len);
}

static void readString(std::istream& is, std::string& s) {
  uint32_t len;
  readPod(is, len);
  // Guard against corrupted files claiming absurdly large strings (OOM on 320KB RAM device)
  constexpr uint32_t MAX_STRING_LEN = 4096;
  if (len > MAX_STRING_LEN) {
    s.clear();
    return;
  }
  s.resize(len);
  is.read(&s[0], len);
}

static void readString(FsFile& file, std::string& s) {
  uint32_t len;
  readPod(file, len);
  // Guard against corrupted files claiming absurdly large strings (OOM on 320KB RAM device)
  constexpr uint32_t MAX_STRING_LEN = 4096;
  if (len > MAX_STRING_LEN) {
    s.clear();
    return;
  }
  s.resize(len);
  file.read(&s[0], len);
}
}  // namespace serialization
