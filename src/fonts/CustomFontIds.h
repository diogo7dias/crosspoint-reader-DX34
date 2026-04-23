#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

// Runtime font IDs for user-dropped BDF fonts. Kept OUT of fontIds.h
// (which is auto-generated from the built-in font name list) so
// regenerating that file cannot accidentally rewrite or collide with
// these values.
//
// Built-in IDs are computed at build time from a name list; custom
// fonts are dropped at runtime so we compute on the fly from the family
// name AND size. The 0x4346xxxx prefix acts as a type tag — the low 16
// bits hold a CRC32 truncation of "name#size", giving 65k slots. At the
// theoretical max of 50 registered (name, size) pairs collision risk
// is ~1.9 %; typical 3–12 registrations are ~0.01–0.1 %.

namespace crosspoint {
namespace fonts {

constexpr uint32_t kCustomFontIdPrefix = 0x43460000;

// CRC32 (Ethernet polynomial) of a byte range. Compile-time capable so
// callers can also pre-hash known names in tests.
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

// Hash composite "<name>#<size>" so each installed (name, size) pair gets
// its own fontId. The '#' separator prevents name-size collisions like
// ("foo", 12) vs ("foo1", 2). Callers that want to register the whole
// family under one id (e.g. for legacy single-size paths) pass size=0 —
// the hash is still deterministic.
inline int idForFamily(const std::string& name, uint16_t sizePt) {
  char buf[64];
  const int n = snprintf(buf, sizeof(buf), "%s#%u", name.c_str(), static_cast<unsigned>(sizePt));
  const size_t len = n > 0 ? static_cast<size_t>(n) : 0;
  const uint32_t h = crc32(buf, len);
  // Cast to int (not uint32_t) because GfxRenderer stores fontIds as int.
  return static_cast<int>(kCustomFontIdPrefix | (h & 0xFFFFu));
}

}  // namespace fonts
}  // namespace crosspoint
