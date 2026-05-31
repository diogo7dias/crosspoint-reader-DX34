// Host definitions for the Arduino runtime bits declared in the esp_heap_caps
// shim (ESP global, delay, millis). delay is a no-op; millis is a monotonic
// counter (only used in diagnostic logs).
#include <esp_heap_caps.h>

EspClass ESP;

void delay(uint32_t) {}

uint32_t millis() {
  static uint32_t t = 0;
  return ++t;
}
