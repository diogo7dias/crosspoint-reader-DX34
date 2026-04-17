#include "Logging.h"

#include <string>

#ifdef SIMULATOR
// On device, the Arduino framework provides the concrete Serial object that
// MySerialImpl wraps. In the simulator there is no hardware UART — route
// everything to stdout so LOG_* calls are visible while running the SDL app.
#include <cstdarg>
#include <cstdio>
MySerialImpl MySerialImpl::instance;
size_t MySerialImpl::write(uint8_t b) {
  fputc(b, stdout);
  return 1;
}
size_t MySerialImpl::write(const uint8_t* buffer, size_t size) {
  fwrite(buffer, 1, size, stdout);
  return size;
}
void MySerialImpl::flush() { fflush(stdout); }
size_t MySerialImpl::printf(const char* format, ...) {
  va_list args;
  va_start(args, format);
  int n = vfprintf(stdout, format, args);
  va_end(args);
  return n < 0 ? 0 : (size_t)n;
}
#endif

#define MAX_ENTRY_LEN 256
#define MAX_LOG_LINES 16

// Simple ring buffer log in RTC memory, survives deep sleep and panic reboots.
// Used by HalSystem to include recent logs in crash reports.
RTC_NOINIT_ATTR char logMessages[MAX_LOG_LINES][MAX_ENTRY_LEN];
RTC_NOINIT_ATTR size_t logHead = 0;
// Magic word to detect uninitialized RTC memory on cold boot.
RTC_NOINIT_ATTR uint32_t rtcLogMagic;
static constexpr uint32_t LOG_RTC_MAGIC = 0xDEADBEEF;

void addToLogRingBuffer(const char* message) {
  if (rtcLogMagic != LOG_RTC_MAGIC || logHead >= MAX_LOG_LINES) {
    memset(logMessages, 0, sizeof(logMessages));
    logHead = 0;
    rtcLogMagic = LOG_RTC_MAGIC;
  }
  strncpy(logMessages[logHead], message, MAX_ENTRY_LEN - 1);
  logMessages[logHead][MAX_ENTRY_LEN - 1] = '\0';
  logHead = (logHead + 1) % MAX_LOG_LINES;
}

// Since logging can take a large amount of flash, we want to make the format string as short as possible.
// This logPrintf prepend the timestamp, level and origin to the user-provided message, so that the user only needs to
// provide the format string for the message itself.
void logPrintf(const char* level, const char* origin, const char* format, ...) {
  va_list args;
  va_start(args, format);
  char buf[MAX_ENTRY_LEN];
  char* c = buf;
  // add timestamp, level and origin
  {
    unsigned long ms = millis();
    int len = snprintf(c, sizeof(buf), "[%lu] [%s] [%s] ", ms, level, origin);
    // erro while writing => return
    if (len < 0) {
      va_end(args);
      return;
    }
    // clamp c to be in buffer range
    c += std::min(len, MAX_ENTRY_LEN);
  }
  // add the user message
  {
    int len = vsnprintf(c, sizeof(buf) - (c - buf), format, args);
    if (len < 0) {
      va_end(args);
      return;
    }
  }
  va_end(args);
  if (logSerial) {
    logSerial.print(buf);
  }
  addToLogRingBuffer(buf);
}

std::string getLastLogs() {
  if (rtcLogMagic != LOG_RTC_MAGIC) {
    return {};
  }
  std::string output;
  for (size_t i = 0; i < MAX_LOG_LINES; i++) {
    size_t idx = (logHead + i) % MAX_LOG_LINES;
    if (logMessages[idx][0] != '\0') {
      const size_t len = strnlen(logMessages[idx], MAX_ENTRY_LEN);
      output.append(logMessages[idx], len);
    }
  }
  return output;
}

bool sanitizeLogHead() {
  if (rtcLogMagic != LOG_RTC_MAGIC || logHead >= MAX_LOG_LINES) {
    logHead = 0;
    return true;
  }
  return false;
}

void clearLastLogs() {
  for (size_t i = 0; i < MAX_LOG_LINES; i++) {
    logMessages[i][0] = '\0';
  }
  logHead = 0;
  rtcLogMagic = LOG_RTC_MAGIC;
}
