#pragma once

#include <WString.h>

#include <cstddef>
#include <cstring>
#include <string>

namespace StringUtils {

/**
 * Copy a C string into a fixed-size char buffer, always null-terminating.
 * Consolidates the strncpy+manual-null-terminate pattern repeated across
 * settings and persistence code. Caller guarantees src is a valid pointer,
 * matching the semantics of the inline sites this replaces.
 */
template <size_t N>
inline void safeStrncpy(char (&dst)[N], const char* src) {
  std::strncpy(dst, src, N - 1);
  dst[N - 1] = '\0';
}

/**
 * Sanitize a string for use as a filename.
 * Replaces invalid characters with underscores, trims spaces/dots,
 * and limits length to maxLength characters.
 */
std::string sanitizeFilename(const std::string& name, size_t maxLength = 100);

/**
 * Check if the given filename ends with the specified extension (case-insensitive).
 */
bool checkFileExtension(const std::string& fileName, const char* extension);
bool checkFileExtension(const String& fileName, const char* extension);

/**
 * Convert ASCII lowercase letters to uppercase. Non-letter bytes are unchanged.
 */
std::string toUpperAscii(std::string text);

}  // namespace StringUtils
