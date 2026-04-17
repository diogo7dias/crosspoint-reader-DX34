#pragma once
// Stub — DX34 CrossPointWebServer uses WiFiUDP for discovery broadcast.
// Simulator has no network broadcast; provide a no-op class so the code compiles.
#include <cstdint>
#include <cstddef>
#include "Print.h"
#include "WiFi.h"

class WiFiUDP : public Print {
 public:
  uint8_t begin(uint16_t) { return 0; }
  uint8_t beginMulticast(IPAddress, uint16_t) { return 0; }
  void stop() {}
  int beginPacket(IPAddress, uint16_t) { return 0; }
  int beginPacket(const char*, uint16_t) { return 0; }
  int endPacket() { return 0; }
  size_t write(uint8_t) override { return 0; }
  size_t write(const uint8_t*, size_t n) override { return n; }
  int parsePacket() { return 0; }
  int available() { return 0; }
  int read() { return -1; }
  int read(unsigned char*, size_t) { return 0; }
  int read(char*, size_t) { return 0; }
  IPAddress remoteIP() { return {}; }
  uint16_t remotePort() { return 0; }
  void flush() {}
};
