#pragma once
#include <string>

class FsHelpers {
 public:
  static std::string normalisePath(const std::string& path);
  // Decode %XX percent-escapes in an EPUB-internal href/path (e.g. "Chapter%201"
  // -> "Chapter 1") so the byte form matches the actual zip entry name. Leaves a
  // bare '%' or malformed escape untouched. (#2249/#2271)
  static std::string decodeUriEscapes(const std::string& path);
};
