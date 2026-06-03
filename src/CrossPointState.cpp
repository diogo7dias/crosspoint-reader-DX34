#include "CrossPointState.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include "Paths.h"
#include "persist/AppStateStore.h"

namespace {
constexpr uint8_t STATE_FILE_VERSION = 5;
constexpr char STATE_FILE_BIN[] = "/.crosspoint/state.bin";
constexpr char STATE_FILE_JSON[] = "/.crosspoint/state.json";
constexpr char STATE_FILE_BAK[] = "/.crosspoint/state.bin.bak";
}  // namespace

CrossPointState& CrossPointState::getInstance() { return crosspoint::persist::appStateStore().unsafeMut(); }

bool CrossPointState::saveToFile() const {
  Storage.mkdir(Paths::kDataDir);
  // Debounced: mark dirty now, coalesce with any subsequent saveToFile() in
  // the same tick window into a single write. PersistManager::flushAll() at
  // activity-transition chokepoints + main-loop tickPersist drain the queue.
  crosspoint::persist::appStateStore().flushSoon();
  return true;
}

bool CrossPointState::saveToFileSync() const {
  Storage.mkdir(Paths::kDataDir);
  // flushNow writes unconditionally on this thread and clears the dirty flag,
  // so the value is durable on SD before we return — no dependence on the
  // main-loop tickPersist draining the debounce queue.
  return crosspoint::persist::appStateStore().flushNow();
}

bool CrossPointState::loadFromFile() {
  // Route through PersistentStore — IFileIO.safeRead already handles the
  // real → .bak → .tmp fallback chain.
  auto report = crosspoint::persist::appStateStore().load();
  if (report.status == crosspoint::persist::LoadReport::kOk ||
      report.status == crosspoint::persist::LoadReport::kRecoveredFromBak) {
    LOG_DBG("CPS", "load: %s (%s)", report.name, report.detail);
    return true;
  }
  // JSON missing or corrupt → try legacy binary migration from pre-RFC-#20 builds.
  if (Storage.exists(STATE_FILE_BIN) && loadFromBinaryFile()) {
    if (saveToFile()) {
      // saveToFile() is debounced; force an immediate flush so migration
      // persists before the .bin file is renamed.
      crosspoint::persist::appStateStore().flushNow();
      Storage.rename(STATE_FILE_BIN, STATE_FILE_BAK);
      LOG_DBG("CPS", "Migrated state.bin to state.json");
      return true;
    }
    LOG_ERR("CPS", "Failed to save state during migration");
    return false;
  }
  return false;
}

bool CrossPointState::loadFromBinaryFile() {
  FsFile inputFile;
  if (!Storage.openFileForRead("CPS", STATE_FILE_BIN, inputFile)) {
    return false;
  }

  lastSleepWallpaperPath.clear();
  favoriteBmpPaths.clear();

  uint8_t version = UINT8_MAX;  // safe default: triggers "unknown version" if read fails
  serialization::readPod(inputFile, version);
  if (version > STATE_FILE_VERSION) {
    LOG_ERR("CPS", "Deserialization failed: Unknown version %u", version);
    inputFile.close();
    return false;
  }

  serialization::readString(inputFile, openEpubPath);
  // Legacy fields lastSleepImage (v>=2) and the in-RAM playlist vector (v>=5)
  // were removed when the V2 order-file replaced them (RFC #145). Still consume
  // their bytes during this one-time .bin->JSON migration so the fields written
  // after them stay aligned; the values themselves are discarded.
  if (version >= 2) {
    uint8_t legacyLastSleepImage = 0;
    serialization::readPod(inputFile, legacyLastSleepImage);
  }
  if (version >= 5) {
    uint16_t playlistCount = 0;
    serialization::readPod(inputFile, playlistCount);
    // The original loader capped the stored count at 30; replicate that exactly
    // so a legacy .bin written under the cap stays byte-aligned.
    if (playlistCount > 30) playlistCount = 30;
    for (uint16_t i = 0; i < playlistCount; i++) {
      std::string discarded;
      serialization::readString(inputFile, discarded);
    }
  }

  if (version >= 3) {
    serialization::readPod(inputFile, readerActivityLoadCount);
  }

  if (version >= 4) {
    serialization::readPod(inputFile, lastSleepFromReader);
  } else {
    lastSleepFromReader = false;
  }

  inputFile.close();
  return true;
}
