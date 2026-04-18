/**
 * @file CrossPointStateJson.h
 * @brief Free toJson/fromJson pair for CrossPointState.
 *
 * Byte-identical to JsonSettingsIO::saveState/loadState. Extracted so
 * PersistentStore<CrossPointState> can own the store without reaching
 * into the 1045-LOC god-marshaller. Under PERSIST_V2 this is the
 * serializer plugged into the template.
 */
#pragma once

#include <string>

#include "IFileIO.h"

class CrossPointState;

namespace crosspoint {
namespace persist {

// Serialize to a JSON string (no file I/O). Mirrors JsonSettingsIO::saveState.
// Retained for non-streaming callers and tests that round-trip via a string.
std::string serializeCrossPointState(const CrossPointState& s);

// Streaming variant: writes the same JSON payload directly to a JsonSink
// (SD file on device, std::string on host tests). No std::string peak on
// the heap — required because APP_STATE with a populated sleep playlist
// can produce a ~15 KB payload that OOMs `new` inside basic_string::_M_mutate
// when flush fires mid-activity with tight free heap (see issue #43).
void streamSerializeCrossPointState(const CrossPointState& s, JsonSink& sink);

// Parse JSON string into the struct. Returns false on parse error.
// Mirrors JsonSettingsIO::loadState.
bool deserializeCrossPointState(const std::string& json, CrossPointState& s);

}  // namespace persist
}  // namespace crosspoint
