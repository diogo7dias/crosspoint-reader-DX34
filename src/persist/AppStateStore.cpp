#ifdef PERSIST_V2

#include "AppStateStore.h"

#include "../CrossPointState.h"
#include "PersistManager.h"
#include "SdFatFileIO.h"

namespace crosspoint {
namespace persist {

namespace {
SdFatFileIO& io() {
  static SdFatFileIO instance;
  return instance;
}
}  // namespace

PersistentStore<CrossPointState>& appStateStore() {
  static PersistentStore<CrossPointState> store("APP_STATE", "/.crosspoint/state.json", io(), &serializeCrossPointState,
                                                &deserializeCrossPointState);
  static bool registered = false;
  if (!registered) {
    PersistManager().registerStore(PersistManagerImpl::Entry{
        [](uint32_t now) { return store.tickPersist(now); },
        []() { return store.flushNow(); },
        []() { return store.isDirty(); },
        store.name(),
        store.path(),
    });
    registered = true;
  }
  return store;
}

}  // namespace persist
}  // namespace crosspoint

#endif  // PERSIST_V2
