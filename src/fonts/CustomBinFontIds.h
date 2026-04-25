#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

// Runtime font IDs for user-installed .bin (CPBN) fonts.
//
// Built-in IDs are computed at build time from a name list; custom fonts
// are installed at runtime via the web UI, so we compute on the fly from
// the family name AND size. The 0x43460000 prefix ("CF") acts as a type
// tag; the low 16 bits hold a CRC32 truncation of "name#size". At the
// realistic max of a few dozen registered (name, size) pairs the
// collision risk is <1%.

namespace crosspoint {
namespace fonts {

constexpr uint32_t kCustomFontIdPrefix = 0x43460000;

constexpr uint32_t crc32(const char* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    crc ^= static_cast<uint8_t>(data[i]);
    for (int bit = 0; bit < 8; ++bit) {
      const uint32_t mask = -static_cast<int32_t>(crc & 1u);
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return ~crc;
}

inline int idForFamily(const std::string& name, uint16_t sizePt) {
  char buf[64];
  const int n = snprintf(buf, sizeof(buf), "%s#%u", name.c_str(), static_cast<unsigned>(sizePt));
  const size_t len = n > 0 ? static_cast<size_t>(n) : 0;
  const uint32_t h = crc32(buf, len);
  return static_cast<int>(kCustomFontIdPrefix | (h & 0xFFFFu));
}

}  // namespace fonts
}  // namespace crosspoint
