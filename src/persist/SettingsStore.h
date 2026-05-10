/**
 * @file SettingsStore.h
 * @brief PersistentStore<CrossPointSettings> singleton accessor (RFC #147 stage 1).
 *
 * Mirror of AppStateStore: SETTINGS now flows through the same machinery
 * as APP_STATE — debounce coalescing, streamed write, atomic rotation,
 * LoadReport. CrossPointSettings::getInstance() returns this store's
 * unsafeMut() so all 50+ existing field accessors keep working unchanged.
 */
#pragma once

#include "../CrossPointSettings.h"
#include "CrossPointSettingsJson.h"
#include "PersistentStore.h"

namespace crosspoint {
namespace persist {

// Lazy-constructed singleton. First call also registers the store with
// PersistManager so tickPersist / flushAll work without extra boilerplate.
PersistentStore<CrossPointSettings>& settingsStore();

}  // namespace persist
}  // namespace crosspoint
