#include "CrashInfo.h"

#include <Arduino.h>
#include <Logging.h>

#include <cstring>

#ifndef CROSSPOINT_VERSION
#define CROSSPOINT_VERSION "unknown"
#endif

namespace crosspoint {
namespace diag {

namespace {

// Keep the consolidated file bounded. When it exceeds this, the oldest sections
// are trimmed from the front. 32 KB keeps plenty of recent history while staying
// small enough to open/share and to never pressure the SD.
constexpr size_t kCapBytes = 32 * 1024;
constexpr const char* kTmpPath = "/CRASH_INFO.TXT.tmp";

// After a write, if the file is over the cap, drop the oldest bytes: keep the
// last ~kCapBytes, advanced to the next line boundary so the retained head is
// not a mid-line fragment. Streams through a small fixed stack buffer + a temp
// file — no heap, safe on the low-heap report paths. Best-effort: on any error
// the file is simply left as-is (it will be retrimmed on the next append).
void trimToCap() {
  HalFile in = Storage.open(kCrashInfoPath, O_RDONLY);
  if (!in) return;
  const size_t sz = static_cast<size_t>(in.size());
  if (sz <= kCapBytes) {
    in.close();
    return;
  }

  size_t keepFrom = sz - kCapBytes;
  char buf[256];

  // Advance keepFrom past the next newline so we start the retained head on a
  // clean line. Bounded scan; if no newline is found nearby, keep the raw byte
  // offset (a slightly ragged first line is acceptable for a diagnostics file).
  in.seek(keepFrom);
  size_t scanned = 0;
  while (scanned < 2048) {
    const int n = in.read(buf, sizeof(buf));
    if (n <= 0) break;
    for (int i = 0; i < n; i++) {
      if (buf[i] == '\n') {
        keepFrom += scanned + static_cast<size_t>(i) + 1;
        scanned = 2048;  // force loop exit
        break;
      }
    }
    if (scanned >= 2048) break;
    scanned += static_cast<size_t>(n);
  }
  if (keepFrom >= sz) {
    in.close();
    return;
  }

  HalFile out = Storage.open(kTmpPath, O_WRITE | O_CREAT | O_TRUNC);
  if (!out) {
    in.close();
    return;
  }
  in.seek(keepFrom);
  for (;;) {
    const int n = in.read(buf, sizeof(buf));
    if (n <= 0) break;
    out.write(reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(n));
  }
  in.close();
  out.close();

  if (!Storage.remove(kCrashInfoPath) || !Storage.rename(kTmpPath, kCrashInfoPath)) {
    LOG_ERR("CRASH", "trimToCap: failed to swap trimmed file");
  }
}

}  // namespace

bool beginCrashSection(const char* type, HalFile& file) {
  // Open for append: O_WRITE|O_CREAT then seek to end (portable across the SD
  // backend instead of relying on an O_APPEND flag).
  file = Storage.open(kCrashInfoPath, O_WRITE | O_CREAT);
  if (!file) {
    LOG_ERR("CRASH", "beginCrashSection: cannot open %s", kCrashInfoPath);
    return false;
  }
  file.seek(file.size());
  char header[96];
  const int n = snprintf(header, sizeof(header), "\n=== %s  %s  +%lums ===\n", type ? type : "?", CROSSPOINT_VERSION,
                         static_cast<unsigned long>(millis()));
  if (n > 0) {
    file.write(reinterpret_cast<const uint8_t*>(header),
               static_cast<size_t>(n < static_cast<int>(sizeof(header)) ? n : static_cast<int>(sizeof(header)) - 1));
  }
  return true;
}

void endCrashSection(HalFile& file) {
  // Ensure the section ends with a newline so the next header starts cleanly.
  file.write(reinterpret_cast<const uint8_t*>("\n"), 1);
  file.close();
  trimToCap();
}

void appendCrashSection(const char* type, const std::string& body) {
  HalFile file;
  if (!beginCrashSection(type, file)) return;
  if (!body.empty()) {
    file.write(reinterpret_cast<const uint8_t*>(body.data()), body.size());
  }
  endCrashSection(file);
  LOG_INF("CRASH", "Appended %s section to %s", type ? type : "?", kCrashInfoPath);
}

}  // namespace diag
}  // namespace crosspoint
