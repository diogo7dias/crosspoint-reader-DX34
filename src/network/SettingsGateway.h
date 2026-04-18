#pragma once

// SettingsGateway — single boundary between the HTTP settings handler and
// the CrossPointSettings global.
//
// Today handlePostSettings() inlines: JSON parse → per-key switch over the
// getSettingsList() schema → SETTINGS.saveToFile() — and then ignores the
// save return value. That silent disk-error path is the primary bug fixed
// here (user sees "saved" in the browser, settings revert on next boot).
//
// The gateway owns orchestration only: it invokes an applier lambda to
// mutate settings and a persister lambda to write to disk, then reports
// both outcomes. Device code injects lambdas that wrap the existing switch
// and SETTINGS.saveToFile(); host tests inject fakes.
//
// Forward-compat with RFC #20 (PersistentStore<T>): when saveToFile migrates
// behind PersistentStore::commit(), only the persister lambda changes.

#include <ArduinoJson.h>

#include <functional>
#include <string>

namespace crosspoint {
namespace network {

class SettingsGateway {
 public:
  // Applies known keys from <doc> to the settings target. Returns the number
  // of fields that were actually mutated (unknown keys and out-of-range
  // values are ignored, matching legacy behavior).
  using ApplyFn = std::function<int(const JsonDocument& doc)>;

  // Persists the current settings. Returns true on success, false on any
  // storage error.
  using PersistFn = std::function<bool()>;

  struct Result {
    int applied = 0;
    bool persisted = false;
    std::string error;  // empty on success
  };

  SettingsGateway(ApplyFn apply, PersistFn persist);

  Result applyJson(const JsonDocument& doc);

 private:
  ApplyFn apply_;
  PersistFn persist_;
};

}  // namespace network
}  // namespace crosspoint
