/**
 * @file SleepFs.h
 * @brief Abstraction over /sleep directory filesystem ops for WallpaperPlaylist.
 *
 * Production: SdFatSleepFs wraps HalStorage. Host tests: FakeSleepFs uses a
 * std::vector<std::string>. Keeps WallpaperPlaylist hardware-free and
 * unit-testable without an ESP32 toolchain.
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
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

  // Streaming successor in the V2 canonical rotation order: mtime DESCENDING
  // (newest first), then name ascending as a deterministic tiebreak — the exact
  // order the buffer-backed playlist materializes. Returns the basename that
  // comes immediately after `afterName` in that order, wrapping to the FRONT
  // (newest) when `afterName` is the last entry, is empty, or is not present.
  // Empty only when /sleep has no wallpapers.
  //
  // This lets the low-heap direct-pick fallback follow the STRICT rotation
  // order — a freshly uploaded wallpaper (newest mtime) leads each lap, matching
  // the "new wallpapers on top" semantics — instead of jumping to a random file
  // whenever heap pressure forces the fallback. The default impl materializes
  // the listing (fine for host tests); SdFatSleepFs overrides with an O(1)-heap
  // streaming scan so the device never allocates the full listing on the very
  // path the fallback exists to keep heap-cheap.
  virtual std::string nextSleepBmpByMtimeDesc(const std::string& afterName) {
    auto entries = listSleepBmpsWithMtime(1024);
    if (entries.empty()) return {};
    std::sort(entries.begin(), entries.end(), [](const SleepBmpEntry& a, const SleepBmpEntry& b) {
      if (a.mtime != b.mtime) return a.mtime > b.mtime;
      return a.name < b.name;
    });
    if (afterName.empty()) return entries.front().name;
    for (size_t i = 0; i < entries.size(); ++i) {
      if (entries[i].name == afterName) {
        return (i + 1 < entries.size()) ? entries[i + 1].name : entries.front().name;
      }
    }
    return entries.front().name;
  }

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

  // V2 streaming walk: invoke `cb(name, len, mtime)` once per .bmp under /sleep,
  // in SD iteration order. Caller decides what to retain — typically only NEW
  // files vs. an existing buffer set, keeping peak heap proportional to the
  // delta (usually 0-3 entries) rather than the full /sleep listing.
  // Required for the boot/home-route reconcile path where materializing all
  // 500 entries as a vector trips bad_alloc on a fragmented heap.
  //
  // `name` points at storage owned by the callee for the duration of the call
  // only (the SD impl hands the raw directory-entry buffer) — the callback must
  // copy if it needs to retain it. Passing char*+len instead of std::string
  // lets the production impl avoid one heap allocation per file. Default impl
  // falls back to listSleepBmpsWithMtime so existing fakes link.
  virtual void walkSleepBmps(const std::function<void(const char* /*name*/, size_t /*len*/, uint32_t /*mtime*/)>& cb) {
    auto entries = listSleepBmpsWithMtime(1024);
    for (const auto& e : entries) cb(e.name.c_str(), e.name.size(), e.mtime);
  }

  // Generic storage ops used during trim / rename bookkeeping.
  virtual bool exists(const std::string& path) = 0;
  virtual bool mkdir(const std::string& path) = 0;
  virtual bool rename(const std::string& from, const std::string& to) = 0;
};

}  // namespace sleep
}  // namespace crosspoint
