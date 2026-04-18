/**
 * @file AppStateStore.h
 * @brief V2 PersistentStore<CrossPointState> singleton accessor.
 *
 * Under PERSIST_V2, CrossPointState::getInstance() is backed by this
 * store's internal data, and saveToFile()/loadFromFile() delegate to
 * flushSoon()/load(). Caller API on APP_STATE is unchanged — all 24
 * call sites keep working without caller changes, gaining debounce
 * coalescing transparently.
 */
#pragma once

#ifdef PERSIST_V2

#include "CrossPointStateJson.h"
#include "PersistentStore.h"

class CrossPointState;

namespace crosspoint {
namespace persist {

// Lazy-constructed singleton. First call also registers the store with
// PersistManager so tickPersist / flushAll work without extra boilerplate.
PersistentStore<CrossPointState>& appStateStore();

}  // namespace persist
}  // namespace crosspoint

#endif  // PERSIST_V2
