/**
 * @file CrossPointSettingsJson.h
 * @brief PersistentStore<CrossPointSettings> serializer pair (RFC #147 stage 1).
 *
 * String + streaming serialize and deserialize for the settings store.
 * Schema lives in JsonSettingsIO::populateSettingsDoc — both paths share it.
 * Migration logic + needsResave detection stays in JsonSettingsIO::loadSettings
 * for now (stage-2 work moves it into a versioned migration chain).
 */
#pragma once

#include <string>

#include "IFileIO.h"

class CrossPointSettings;

namespace crosspoint {
namespace persist {

std::string serializeCrossPointSettings(const CrossPointSettings& s);

// Streaming write: ArduinoJson serializeJson(doc, sink). Pre-empts the
// std::string-peak failure mode under tight free heap; mirrors the
// streamSerializeCrossPointState path.
void streamSerializeCrossPointSettings(const CrossPointSettings& s, JsonSink& sink);

// Parse JSON string into the struct. Delegates to JsonSettingsIO::loadSettings
// so existing migration / normalize* / `needsResave` ladder stays put.
// When the legacy loader flips the resave flag we set a thread_local sticky
// bit; SettingsStore::loadAndApplyMigrations consumes it and calls touch()
// to schedule a rewrite. The store itself is not touched from inside the
// deserializer (would re-enter the singleton during its load()).
bool deserializeCrossPointSettings(const std::string& json, CrossPointSettings& s);

// Read + clear the sticky resave flag set by the most recent
// deserializeCrossPointSettings call. Returns true if the legacy loader
// reported a needs-resave migration. Idempotent: subsequent calls return
// false until the next deserialize.
bool consumeSettingsResaveSticky();

}  // namespace persist
}  // namespace crosspoint
