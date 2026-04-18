/**
 * @file IFileIO.h
 * @brief Abstraction over filesystem ops used by PersistentStore<T>.
 *
 * Production: SdFatFileIO wraps HalStorage (SD card). Host tests:
 * InMemoryFileIO uses a std::unordered_map<path, content>. Keeps
 * PersistentStore and PersistManager hardware-free and unit-testable
 * without an ESP32 toolchain.
 */
#pragma once

#include <cstddef>
#include <string>

namespace crosspoint {
namespace persist {

struct IFileIO {
  virtual ~IFileIO() = default;

  // Atomic write: content → `path`, rotating any prior file to `.bak`
  // and using `.tmp` as the intermediate. Creates parent directory if
  // missing. Returns true on success.
  virtual bool safeWrite(const std::string& path, const std::string& content) = 0;

  // Read with crash-safe fallback: real → `.bak` → `.tmp`. Returns
  // empty string if none of the three yielded content.
  virtual std::string safeRead(const std::string& path) = 0;

  // Existence / management used for sidecar backup + diagnostics.
  virtual bool exists(const std::string& path) = 0;
  virtual bool mkdir(const std::string& path) = 0;
  virtual bool copy(const std::string& from, const std::string& to) = 0;
};

}  // namespace persist
}  // namespace crosspoint
