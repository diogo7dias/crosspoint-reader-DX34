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
    // Route writes through the streaming serializer so the payload flows
    // straight to the tmp file instead of peaking as a std::string on the
    // heap. A populated sleep playlist can produce ~15 KB of JSON; under
    // tight free heap (reader activity loaded, book pages cached), that
    // std::string peak trips `new` → __terminate (see issue #43 postmortem).
    store.setStreamSerializer(&streamSerializeCrossPointState);
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
