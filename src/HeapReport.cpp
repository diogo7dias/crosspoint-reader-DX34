#include "HeapReport.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <stdio.h>

#ifndef CROSSPOINT_VERSION
#define CROSSPOINT_VERSION "unknown"
#endif

namespace {

constexpr const char* REPORT_PATH = "/heap_report.txt";

void appendf(std::string& s, const char* fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n > 0) {
    s.append(buf, (n < static_cast<int>(sizeof(buf))) ? n : static_cast<int>(sizeof(buf) - 1));
  }
}

const char* resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_UNKNOWN: return "UNKNOWN";
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXT";
    case ESP_RST_SW: return "SW";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    default: return "?";
  }
}

}  // namespace

HeapSnapshot captureHeapSnapshot() {
  HeapSnapshot s{};
  s.freeBytes = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_8BIT));
  s.largestFreeBlockBytes = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  s.minFreeEverBytes = static_cast<uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
  s.internalFreeBytes = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  s.internalLargestBytes = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  return s;
}

void appendHeapSnapshot(std::string& out, const HeapSnapshot& snap) {
  appendf(out, "free=%u largest=%u min=%u intl=%u intlLargest=%u",
          static_cast<unsigned>(snap.freeBytes), static_cast<unsigned>(snap.largestFreeBlockBytes),
          static_cast<unsigned>(snap.minFreeEverBytes), static_cast<unsigned>(snap.internalFreeBytes),
          static_cast<unsigned>(snap.internalLargestBytes));
}

bool writeHeapReport(const char* reason) {
  const HeapSnapshot snap = captureHeapSnapshot();
  const uint32_t uptimeMs = static_cast<uint32_t>(millis());
  const esp_reset_reason_t resetReason = esp_reset_reason();

  std::string body;
  body.reserve(640);
  appendf(body, "CrossPoint Heap Report\n");
  appendf(body, "Firmware:        %s\n", CROSSPOINT_VERSION);
  appendf(body, "Reason:          %s\n", reason ? reason : "(none)");
  appendf(body, "Uptime:          %u ms\n", static_cast<unsigned>(uptimeMs));
  appendf(body, "Reset reason:    %s (%d)\n", resetReasonName(resetReason), static_cast<int>(resetReason));
  appendf(body, "\n");
  appendf(body, "Heap snapshot at write:\n");
  appendf(body, "  Free 8-bit:     %u bytes\n", static_cast<unsigned>(snap.freeBytes));
  appendf(body, "  Largest 8-bit:  %u bytes\n", static_cast<unsigned>(snap.largestFreeBlockBytes));
  appendf(body, "  Min free ever:  %u bytes\n", static_cast<unsigned>(snap.minFreeEverBytes));
  appendf(body, "  Free internal:  %u bytes\n", static_cast<unsigned>(snap.internalFreeBytes));
  appendf(body, "  Largest intl:   %u bytes\n", static_cast<unsigned>(snap.internalLargestBytes));
  appendf(body, "\n");

  // Fragmentation ratio: 100 * (1 - largest/free). 0 = perfect, near 100 =
  // heavily fragmented. Cheap to compute, useful for quick triage.
  if (snap.freeBytes > 0) {
    const uint32_t fragPct =
        100u - static_cast<uint32_t>((static_cast<uint64_t>(snap.largestFreeBlockBytes) * 100u) / snap.freeBytes);
    appendf(body, "Fragmentation:   %u%% (100*(1 - largest/free))\n", static_cast<unsigned>(fragPct));
  } else {
    appendf(body, "Fragmentation:   n/a (free=0)\n");
  }

  // Total heap configured at boot, for reference.
  appendf(body, "Total heap:      %u bytes\n", static_cast<unsigned>(ESP.getHeapSize()));

  if (!Storage.writeFile(REPORT_PATH, String(body.c_str()))) {
    LOG_ERR("HEAP_RPT", "Failed to write %s", REPORT_PATH);
    return false;
  }
  LOG_INF("HEAP_RPT", "Wrote %s (%u B, reason=%s)", REPORT_PATH, static_cast<unsigned>(body.size()),
          reason ? reason : "(none)");
  return true;
}
