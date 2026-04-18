#include "CrossPointState.h"

#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>
#include <Serialization.h>

#include "Paths.h"

#ifdef PERSIST_V2
#include "persist/AppStateStore.h"
#endif

namespace {
constexpr uint8_t STATE_FILE_VERSION = 5;
constexpr char STATE_FILE_BIN[] = "/.crosspoint/state.bin";
constexpr char STATE_FILE_JSON[] = "/.crosspoint/state.json";
constexpr char STATE_FILE_BAK[] = "/.crosspoint/state.bin.bak";
}  // namespace

#ifndef PERSIST_V2
CrossPointState CrossPointState::instance;

CrossPointState& CrossPointState::getInstance() { return instance; }
#else
CrossPointState& CrossPointState::getInstance() { return crosspoint::persist::appStateStore().unsafeMut(); }
#endif

bool CrossPointState::saveToFile() const {
  Storage.mkdir(Paths::kDataDir);
#ifdef PERSIST_V2
  // Debounced: mark dirty now, coalesce with any subsequent saveToFile() in
  // the same tick window into a single write. PersistManager::flushAll() at
  // activity-transition chokepoints + main-loop tickPersist drain the queue.
  crosspoint::persist::appStateStore().flushSoon();
  return true;
#else
  return JsonSettingsIO::saveState(*this, STATE_FILE_JSON);
#endif
}

bool CrossPointState::loadFromFile() {
#ifdef PERSIST_V2
  // Route through PersistentStore — IFileIO.safeRead already handles the
  // real → .bak → .tmp fallback chain.
  auto report = crosspoint::persist::appStateStore().load();
  if (report.status == crosspoint::persist::LoadReport::kOk ||
      report.status == crosspoint::persist::LoadReport::kRecoveredFromBak) {
    LOG_DBG("CPS", "V2 load: %s (%s)", report.name, report.detail);
    return true;
  }
  // JSON missing or corrupt → try legacy binary migration, same as V1.
  if (Storage.exists(STATE_FILE_BIN) && loadFromBinaryFile()) {
    if (saveToFile()) {
      // V2 saveToFile is debounced; force an immediate flush so migration
      // persists before the .bin file is renamed.
      crosspoint::persist::appStateStore().flushNow();
      Storage.rename(STATE_FILE_BIN, STATE_FILE_BAK);
      LOG_DBG("CPS", "Migrated state.bin to state.json (V2)");
      return true;
    }
    LOG_ERR("CPS", "Failed to save state during V2 migration");
    return false;
  }
  return false;
#else
  // V1: try JSON first, then binary migration.
  if (!Storage.exists(STATE_FILE_JSON)) return false;

  String json = JsonSettingsIO::safeReadFile(STATE_FILE_JSON);
  if (!json.isEmpty()) {
    return JsonSettingsIO::loadState(*this, json.c_str());
  }

  if (Storage.exists(STATE_FILE_BIN)) {
    if (loadFromBinaryFile()) {
      if (saveToFile()) {
        Storage.rename(STATE_FILE_BIN, STATE_FILE_BAK);
        LOG_DBG("CPS", "Migrated state.bin to state.json");
        return true;
      } else {
        LOG_ERR("CPS", "Failed to save state during migration");
        return false;
      }
    }
  }

  return false;
#endif
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
  if (version >= 2) {
    serialization::readPod(inputFile, lastSleepImage);
  } else {
    lastSleepImage = UINT8_MAX;
  }

  if (version >= 5) {
    uint16_t playlistCount = 0;
    serialization::readPod(inputFile, playlistCount);
    if (playlistCount > SLEEP_PLAYLIST_MAX_PERSIST) {
      playlistCount = SLEEP_PLAYLIST_MAX_PERSIST;
    }
    sleepImagePlaylist.clear();
    sleepImagePlaylist.reserve(playlistCount);
    for (uint16_t i = 0; i < playlistCount; i++) {
      std::string filename;
      serialization::readString(inputFile, filename);
      sleepImagePlaylist.push_back(std::move(filename));
    }
  } else {
    sleepImagePlaylist.clear();
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
