#include "CrossPointStateJson.h"

#include <ArduinoJson.h>

#include <cstdint>

#include "../CrossPointState.h"

namespace crosspoint {
namespace persist {

std::string serializeCrossPointState(const CrossPointState& s) {
  JsonDocument doc;
  doc["openEpubPath"] = s.openEpubPath;
  doc["lastSleepImage"] = s.lastSleepImage;
  doc["lastShownSleepFilename"] = s.lastShownSleepFilename;
  doc["lastSleepWallpaperPath"] = s.lastSleepWallpaperPath;
  doc["readerActivityLoadCount"] = s.readerActivityLoadCount;
  doc["sessionPagesRead"] = s.sessionPagesRead;
  doc["lastSleepFromReader"] = s.lastSleepFromReader;
  doc["wallpaperRotationPaused"] = s.wallpaperRotationPaused;
  doc["lastSleepWasQuotes"] = s.lastSleepWasQuotes;
  // Large playlists omitted on write to avoid heap/IO cost; MyLibrary/sleep
  // code uses lastShownSleepFilename for those. Matches JsonSettingsIO.
  if (s.sleepImagePlaylist.size() <= CrossPointState::SLEEP_PLAYLIST_MAX_PERSIST) {
    JsonArray arr = doc["sleepImagePlaylist"].to<JsonArray>();
    for (const auto& entry : s.sleepImagePlaylist) arr.add(entry);
  }
  JsonArray favs = doc["favoriteBmpPaths"].to<JsonArray>();
  for (const auto& entry : s.favoriteBmpPaths) favs.add(entry);

  std::string out;
  serializeJson(doc, out);
  return out;
}

bool deserializeCrossPointState(const std::string& json, CrossPointState& s) {
  JsonDocument doc;
  auto err = deserializeJson(doc, json);
  if (err) return false;

  s.openEpubPath = doc["openEpubPath"] | std::string("");
  s.lastSleepImage = doc["lastSleepImage"] | (uint8_t)UINT8_MAX;
  s.lastShownSleepFilename = doc["lastShownSleepFilename"] | std::string("");
  s.lastSleepWallpaperPath = doc["lastSleepWallpaperPath"] | std::string("");
  s.readerActivityLoadCount = doc["readerActivityLoadCount"] | (uint8_t)0;
  s.sessionPagesRead = doc["sessionPagesRead"] | (uint32_t)0;
  s.lastSleepFromReader = doc["lastSleepFromReader"] | false;
  s.wallpaperRotationPaused = doc["wallpaperRotationPaused"] | false;
  s.lastSleepWasQuotes = doc["lastSleepWasQuotes"] | false;

  s.sleepImagePlaylist.clear();
  if (doc["sleepImagePlaylist"].is<JsonArray>()) {
    for (const JsonVariant value : doc["sleepImagePlaylist"].as<JsonArray>()) {
      const char* entry = value.as<const char*>();
      if (entry && entry[0] != '\0') {
        s.sleepImagePlaylist.emplace_back(entry);
        if (s.sleepImagePlaylist.size() >= CrossPointState::SLEEP_PLAYLIST_MAX_PERSIST) break;
      }
    }
  }

  s.favoriteBmpPaths.clear();
  if (doc["favoriteBmpPaths"].is<JsonArray>()) {
    for (const JsonVariant value : doc["favoriteBmpPaths"].as<JsonArray>()) {
      const char* entry = value.as<const char*>();
      if (entry && entry[0] != '\0') s.favoriteBmpPaths.emplace_back(entry);
    }
  }
  return true;
}

}  // namespace persist
}  // namespace crosspoint
