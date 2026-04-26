/**
 * @file SleepFs.h
 * @brief Abstraction over /sleep directory filesystem ops for WallpaperPlaylist.
 *
 * Production: SdFatSleepFs wraps HalStorage. Host tests: FakeSleepFs uses a
 * std::vector<std::string>. Keeps WallpaperPlaylist hardware-free and
 * unit-testable without an ESP32 toolchain.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace crosspoint {
namespace sleep {

struct NextBmpResult {
  std::string next;  // lex-smallest > after, or lex-min if wrap; empty on no files
  size_t count;      // total .bmp count, capped at scanCap
};

// Filename + last-modify time (FAT date/time packed). Used by V2 wallpaper
// rotation for new-on-top insertion ordering and oldest-first trim selection.
struct SleepBmpEntry {
  std::string name;
  uint32_t mtime;
};

struct ISleepFs {
  virtual ~ISleepFs() = default;

  // Count .bmp files directly under /sleep (not recursive). Stops scanning
  // once the running count exceeds scanCap — caller passes a cap to bound
  // worst-case time when the folder is larger than interesting.
  virtual size_t countSleepBmps(size_t scanCap) = 0;

  // Collect up to maxEntries .bmp filenames (basename only, no path) from
  // /sleep, returned sorted ascending. Dotfiles and non-.bmp entries skipped.
  // Only used by Small strategy and trim — maxEntries is bounded.
  virtual std::vector<std::string> listSleepBmps(size_t maxEntries) = 0;

  // Streaming lex-next lookup. For Large strategy advance — O(n) time, O(1)
  // heap beyond a single returned std::string. Returns the lexicographically
  // smallest .bmp filename strictly greater than `after`. If `after` is empty
  // or no such file exists (wrap), returns the lex-min filename. Empty if
  // /sleep has no .bmp files.
  virtual std::string nextSleepBmpAfter(const std::string& after) = 0;

  // Streaming nth-in-directory-order lookup. For Large strategy reshuffle —
  // O(n) time, O(1) heap. Order follows the SD iteration order (not sorted).
  // Returns empty if n >= total count.
  virtual std::string nthSleepBmp(size_t n) = 0;

  // Combined count + lex-next in a single directory scan. Large strategy
  // steady state needs both (count for hysteresis, next for advance) —
  // merging saves one full /sleep scan per sleep render. Default impl falls
  // back to two separate calls; subclasses override for the single-pass win.
  virtual NextBmpResult nextSleepBmpAfterWithCount(const std::string& after, size_t scanCap) {
    NextBmpResult r;
    r.count = countSleepBmps(scanCap);
    r.next = nextSleepBmpAfter(after);
    return r;
  }

  // V2 rotation: collect up to maxEntries .bmp basenames + mtimes in directory
  // iteration order. Default impl falls back to listSleepBmps + mtime=0 so
  // existing fakes still link.
  virtual std::vector<SleepBmpEntry> listSleepBmpsWithMtime(size_t maxEntries) {
    std::vector<SleepBmpEntry> out;
    auto names = listSleepBmps(maxEntries);
    out.reserve(names.size());
    for (auto& n : names) out.push_back({std::move(n), 0});
    return out;
  }

  // Generic storage ops used during trim / rename bookkeeping.
  virtual bool exists(const std::string& path) = 0;
  virtual bool mkdir(const std::string& path) = 0;
  virtual bool rename(const std::string& from, const std::string& to) = 0;
};

}  // namespace sleep
}  // namespace crosspoint
