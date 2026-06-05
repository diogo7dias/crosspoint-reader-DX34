/**
 * @file SdFatSleepFs.h
 * @brief Production ISleepFs backed by HalStorage / SDCardManager.
 *
 * Excluded from the host test build: HalStorage pulls in ESP32-only headers.
 */
#pragma once

#include "SleepFs.h"

namespace crosspoint {
namespace sleep {

class SdFatSleepFs final : public ISleepFs {
 public:
  SdFatSleepFs() = default;

  size_t countSleepBmps(size_t scanCap) override;
  std::vector<std::string> listSleepBmps(size_t maxEntries) override;
  std::vector<SleepBmpEntry> listSleepBmpsWithMtime(size_t maxEntries) override;
  void walkSleepBmps(const std::function<void(const char*, size_t, uint32_t)>& cb) override;
  std::string nextSleepBmpAfter(const std::string& after) override;
  std::string nthSleepBmp(size_t n) override;
  NextBmpResult nextSleepBmpAfterWithCount(const std::string& after, size_t scanCap) override;
  bool exists(const std::string& path) override;
  bool mkdir(const std::string& path) override;
  bool rename(const std::string& from, const std::string& to) override;
};

}  // namespace sleep
}  // namespace crosspoint
