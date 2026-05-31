// Host shim for esp_heap_caps + the Arduino runtime bits the parser uses
// (ESP global, delay, millis) — the parser includes <esp_heap_caps.h>
// unconditionally but not <Arduino.h>, so the runtime decls live here on host.
#pragma once
#include <cstddef>
#include <cstdint>

#define MALLOC_CAP_8BIT     (1 << 2)
#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_DEFAULT  0

inline size_t heap_caps_get_largest_free_block(uint32_t) { return 4u * 1024 * 1024; }
inline size_t heap_caps_get_free_size(uint32_t) { return 8u * 1024 * 1024; }
inline size_t heap_caps_get_minimum_free_size(uint32_t) { return 4u * 1024 * 1024; }
inline size_t esp_get_free_heap_size() { return 8u * 1024 * 1024; }

struct EspClass {
  uint32_t getFreeHeap() { return 8u * 1024 * 1024; }
  uint32_t getMinFreeHeap() { return 4u * 1024 * 1024; }
  uint32_t getMaxAllocHeap() { return 4u * 1024 * 1024; }
};
extern EspClass ESP;

void delay(uint32_t ms);
uint32_t millis();

// Arduino provides min()/max() transitively on-device; some EPUB code uses bare
// min()/max(). Provide them as global function templates so unqualified calls
// resolve WITHOUT shadowing/breaking the std::min/std::max calls in the same TU.
template <class T>
constexpr T min(T a, T b) {
  return a < b ? a : b;
}
template <class T>
constexpr T max(T a, T b) {
  return a > b ? a : b;
}
