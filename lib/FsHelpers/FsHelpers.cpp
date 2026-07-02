#include "FsHelpers.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace {
bool isHexDigit(const char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

uint8_t hexValue(const char c) {
  if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
  if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(10 + (c - 'a'));
  return static_cast<uint8_t>(10 + (c - 'A'));
}
}  // namespace

std::string FsHelpers::decodeUriEscapes(const std::string& path) {
  std::string decoded;
  decoded.reserve(path.size());

  for (size_t i = 0; i < path.size(); i++) {
    if (path[i] == '%' && i + 2 < path.size() && isHexDigit(path[i + 1]) && isHexDigit(path[i + 2])) {
      const uint8_t value = static_cast<uint8_t>((hexValue(path[i + 1]) << 4) | hexValue(path[i + 2]));
      decoded += static_cast<char>(value);
      i += 2;
      continue;
    }

    decoded += path[i];
  }

  return decoded;
}

std::string FsHelpers::normalisePath(const std::string& path) {
  std::vector<std::string> components;
  std::string component;

  for (const auto c : path) {
    if (c == '/') {
      if (!component.empty()) {
        if (component == "..") {
          if (!components.empty()) {
            components.pop_back();
          }
        } else {
          components.push_back(component);
        }
        component.clear();
      }
    } else {
      component += c;
    }
  }

  if (!component.empty()) {
    components.push_back(component);
  }

  std::string result;
  for (const auto& c : components) {
    if (!result.empty()) {
      result += "/";
    }
    result += c;
  }

  return result;
}

std::string FsHelpers::detectImageExtFromMagic(const uint8_t* data, const size_t len) {
  if (data == nullptr) {
    return "";
  }
  // JPEG: FF D8 FF. PNG: 89 50 4E 47 0D 0A 1A 0A. Only formats the reader decodes.
  static constexpr uint8_t JPEG_SIG[] = {0xFF, 0xD8, 0xFF};
  static constexpr uint8_t PNG_SIG[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  if (len >= sizeof(JPEG_SIG) && memcmp(data, JPEG_SIG, sizeof(JPEG_SIG)) == 0) {
    return ".jpg";
  }
  if (len >= sizeof(PNG_SIG) && memcmp(data, PNG_SIG, sizeof(PNG_SIG)) == 0) {
    return ".png";
  }
  return "";
}
