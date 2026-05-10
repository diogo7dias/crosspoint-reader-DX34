#include "CrossPointSettingsJson.h"

#include <ArduinoJson.h>

#include "../CrossPointSettings.h"
#include "../JsonSettingsIO.h"

namespace crosspoint {
namespace persist {

namespace {
// Thread-local: ESP32 main loop is single-task, so this is effectively a
// global flag with safer-than-static storage class. Tests on host can run
// settings parse cases concurrently across translation units without
// stomping on each other.
thread_local bool s_resaveSticky = false;
}  // namespace

std::string serializeCrossPointSettings(const CrossPointSettings& s) {
  JsonDocument doc;
  JsonSettingsIO::populateSettingsDoc(s, doc);
  std::string out;
  serializeJson(doc, out);
  return out;
}

void streamSerializeCrossPointSettings(const CrossPointSettings& s, JsonSink& sink) {
  JsonDocument doc;
  JsonSettingsIO::populateSettingsDoc(s, doc);
  // ArduinoJson duck-types the sink (write(uint8_t) + write(uint8_t*, size_t))
  // — bytes flow straight to the underlying tmp file or in-memory buffer
  // without an intermediate std::string peak.
  serializeJson(doc, sink);
}

bool deserializeCrossPointSettings(const std::string& json, CrossPointSettings& s) {
  bool needsResave = false;
  const bool ok = JsonSettingsIO::loadSettings(s, json.c_str(), &needsResave);
  if (ok && needsResave) s_resaveSticky = true;
  return ok;
}

bool consumeSettingsResaveSticky() {
  const bool was = s_resaveSticky;
  s_resaveSticky = false;
  return was;
}

}  // namespace persist
}  // namespace crosspoint
