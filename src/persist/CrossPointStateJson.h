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

class CrossPointState;

namespace crosspoint {
namespace persist {

// Serialize to a JSON string (no file I/O). Mirrors JsonSettingsIO::saveState.
std::string serializeCrossPointState(const CrossPointState& s);

// Parse JSON string into the struct. Returns false on parse error.
// Mirrors JsonSettingsIO::loadState.
bool deserializeCrossPointState(const std::string& json, CrossPointState& s);

}  // namespace persist
}  // namespace crosspoint
