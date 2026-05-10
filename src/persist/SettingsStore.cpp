#include "SettingsStore.h"

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

PersistentStore<CrossPointSettings>& settingsStore() {
  static PersistentStore<CrossPointSettings> store("SETTINGS", "/.crosspoint/settings.json", io(),
                                                   &serializeCrossPointSettings, &deserializeCrossPointSettings);
  static bool registered = false;
  if (!registered) {
    // Stream serializer keeps the JSON payload off the heap as a single
    // std::string. settings.json is ~3-4 KB today but every release adds
    // fields; the same OOM mode that bit APP_STATE (issue #43) is latent
    // here. Pre-empt it by routing writes straight to the tmp file.
    store.setStreamSerializer(&streamSerializeCrossPointSettings);
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
