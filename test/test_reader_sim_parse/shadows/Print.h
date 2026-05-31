// Minimal host shadow of Arduino's Print base class. HalFile derives from it
// and ZipFile::readFileToStream writes decompressed bytes to a Print&.
#pragma once

#include <cstddef>
#include <cstdint>

class Print {
 public:
  virtual ~Print() = default;
  virtual size_t write(uint8_t b) = 0;
  virtual size_t write(const uint8_t* buf, size_t size) {
    size_t n = 0;
    for (size_t i = 0; i < size; ++i) {
      if (!write(buf[i])) break;
      ++n;
    }
    return n;
  }
  size_t write(const char* buf, size_t size) { return write(reinterpret_cast<const uint8_t*>(buf), size); }
};
