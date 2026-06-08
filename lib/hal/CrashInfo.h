#pragma once

// Consolidated on-SD diagnostics file.
//
// Replaces the three separate, each-overwriting report files
// (/crash_report.txt panic dumps, /diag_report.txt WiFi/OTA failures,
// /heap_report.txt pre-fragmentation-restart snapshots) with ONE appended file,
// /CRASH_INFO.TXT, so every diagnostic event is retained as history rather than
// clobbering the previous one. Each event is written as a delimited section:
//
//   === <TYPE>  <version>  +<uptime>ms ===
//   <body>
//
// The file is byte-capped (~32 KB): once it grows past the cap, the OLDEST
// sections are trimmed from the front so the newest always survive and the SD
// can never fill. All writes are best-effort and heap-free (fixed stack
// buffers + a temp file), so they are safe on the low-heap paths that produce
// these reports — panic-boot and the pre-fragmentation silent restart.

#include <HalStorage.h>  // HalFile (== FsFile)

#include <string>

namespace crosspoint {
namespace diag {

constexpr const char* kCrashInfoPath = "/CRASH_INFO.TXT";

// Append a complete section (header for `type` + `body`) and trim to the cap.
// Use from callers that already have the whole body as a string (panic, heap).
void appendCrashSection(const char* type, const std::string& body);

// Streaming variant for callers that emit their body incrementally (WiFi diag).
// beginCrashSection() opens the file at end, writes the section header, and
// hands back the open file; the caller writes its body to `file`, then calls
// endCrashSection(file) to close + trim. Returns false (file not opened) on
// failure — caller must check before writing.
bool beginCrashSection(const char* type, HalFile& file);
void endCrashSection(HalFile& file);

}  // namespace diag
}  // namespace crosspoint
