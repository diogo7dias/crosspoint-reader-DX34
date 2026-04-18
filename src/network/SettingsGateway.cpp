#include "SettingsGateway.h"

#include <ArduinoJson.h>

namespace crosspoint {
namespace network {

SettingsGateway::SettingsGateway(ApplyFn apply, PersistFn persist)
    : apply_(std::move(apply)), persist_(std::move(persist)) {}

SettingsGateway::Result SettingsGateway::applyJson(const JsonDocument& doc) {
  Result r;
  r.applied = apply_ ? apply_(doc) : 0;
  r.persisted = persist_ ? persist_() : false;
  if (!r.persisted) {
    r.error = "save to file failed";
  }
  return r;
}

}  // namespace network
}  // namespace crosspoint
