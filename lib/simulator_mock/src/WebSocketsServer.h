#pragma once
#include "WString.h"

// Dummy WebSockets object types
enum WStype_t {
  WStype_DISCONNECTED,
  WStype_CONNECTED,
  WStype_TEXT,
  WStype_BIN,
  WStype_ERROR,
  WStype_FRAGMENT_TEXT_START,
  WStype_FRAGMENT_BIN_START,
  WStype_FRAGMENT,
  WStype_FRAGMENT_FIN,
  WStype_PING,
  WStype_PONG,
};

class WebSocketsServer {
 public:
  WebSocketsServer(int port) {}
  void begin() {}
  void loop() {}
  template <typename T>
  void onEvent(T) {}
  void broadcastTXT(const String& txt) {}
  void broadcastTXT(const char* txt) {}
  void sendTXT(uint8_t num, const String& txt) {}
  void sendTXT(uint8_t num, const char* txt) {}
  bool sendBIN(uint8_t num, uint8_t* payload, size_t length, bool headerToPayload = false) { return true; }
  bool sendBIN(uint8_t num, const uint8_t* payload, size_t length) { return true; }
  bool broadcastBIN(uint8_t* payload, size_t length) { return true; }
  bool broadcastBIN(const uint8_t* payload, size_t length) { return true; }
  void disconnect(uint8_t num = 0) {}
  uint8_t connectedClients() { return 0; }
  void close() {}
};
