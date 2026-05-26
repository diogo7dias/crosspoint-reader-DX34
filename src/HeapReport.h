#pragma once

#include <stdint.h>

#include <string>

// Heap diagnostics for post-hoc fragmentation analysis.
//
// Two responsibilities:
//   1. captureHeapSnapshot() / formatHeapSnapshot(): a single point-in-time
//      reading of free / largest-free-block / min-free / internal-free, with a
//      canonical string format ("free=NNNN largest=NNNN min=NNNN intl=NNNN")
//      so [HEAP] log lines stay consistent across modules.
//   2. writeHeapReport(reason): dump the snapshot + reset reason + firmware
//      version + heap-region breakdown to /heap_report.txt. Called from the
//      silentRestart() path so every silent reboot leaves a trace on the SD
//      card the user can share without needing serial.
//
// The report file is overwritten on each write — most-recent-restart wins.

struct HeapSnapshot {
  uint32_t freeBytes;            // total free across all 8-bit-capable regions
  uint32_t largestFreeBlockBytes;  // largest contiguous free chunk (8-bit)
  uint32_t minFreeEverBytes;     // smallest free total seen since boot
  uint32_t internalFreeBytes;    // free in INTERNAL caps (no SPIRAM split on C3,
                                 // but kept for forward portability)
  uint32_t internalLargestBytes;
};

HeapSnapshot captureHeapSnapshot();

// Append a one-line "free=… largest=… min=… intl=… intlLargest=…" to a string.
// No trailing newline. For embedding inside a longer log message.
void appendHeapSnapshot(std::string& out, const HeapSnapshot& snap);

// Write /heap_report.txt with the current snapshot, a free-form `reason`
// string (e.g. "wifi-exit-CrossPointWebServer", "reader-oom-recovery"),
// firmware version, reset reason, and a heap-region breakdown. Safe to call
// from any task; takes the storage mutex internally. Returns true on success.
bool writeHeapReport(const char* reason);
