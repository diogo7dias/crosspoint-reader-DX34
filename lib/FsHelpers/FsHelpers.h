#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

class FsHelpers {
 public:
  static std::string normalisePath(const std::string& path);
  // Decode %XX percent-escapes in an EPUB-internal href/path (e.g. "Chapter%201"
  // -> "Chapter 1") so the byte form matches the actual zip entry name. Leaves a
  // bare '%' or malformed escape untouched. (#2249/#2271)
  static std::string decodeUriEscapes(const std::string& path);
  // Detect a decodable image format from a file's leading magic bytes. Returns a
  // canonical extension (".jpg"/".png") for formats the reader can decode, or "" if
  // the bytes match no supported format. Used to render EPUB images referenced
  // without a (correct) file extension. (#2386)
  static std::string detectImageExtFromMagic(const uint8_t* data, size_t len);
};
