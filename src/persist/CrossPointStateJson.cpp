#include "CrossPointStateJson.h"

#include <ArduinoJson.h>

#include <cstdint>

#include "../CrossPointState.h"

namespace crosspoint {
namespace persist {

namespace {
// Populate a JsonDocument with the current state. Shared by both the
// string-based and streamed paths so the schema lives in exactly one place.
void populateDoc(const CrossPointState& s, JsonDocument& doc) {
  doc["openEpubPath"] = s.openEpubPath;
  doc["lastSleepImage"] = s.lastSleepImage;
  doc["lastShownSleepFilename"] = s.lastShownSleepFilename;
  doc["lastSleepWallpaperPath"] = s.lastSleepWallpaperPath;
  doc["readerActivityLoadCount"] = s.readerActivityLoadCount;
  doc["sessionPagesRead"] = s.sessionPagesRead;
  doc["lastSleepFromReader"] = s.lastSleepFromReader;
  doc["wallpaperRotationPaused"] = s.wallpaperRotationPaused;
  doc["lastSleepWasQuotes"] = s.lastSleepWasQuotes;
  doc["sleepFavoritesCapReached"] = s.sleepFavoritesCapReached;
  doc["pendingSleepWallpapersMovedToPause"] = s.pendingSleepWallpapersMovedToPause;
  // Large playlists omitted on write to avoid heap/IO cost; MyLibrary/sleep
  // code uses lastShownSleepFilename for those. Matches JsonSettingsIO.
  if (s.sleepImagePlaylist.size() <= CrossPointState::SLEEP_PLAYLIST_MAX_PERSIST) {
    JsonArray arr = doc["sleepImagePlaylist"].to<JsonArray>();
    for (const auto& entry : s.sleepImagePlaylist) arr.add(entry);
  }
  JsonArray favs = doc["favoriteBmpPaths"].to<JsonArray>();
  for (const auto& entry : s.favoriteBmpPaths) favs.add(entry);
  doc["customFontLegacyCleanupDone"] = s.customFontLegacyCleanupDone;
}
}  // namespace

std::string serializeCrossPointState(const CrossPointState& s) {
  JsonDocument doc;
  populateDoc(s, doc);

  std::string out;
  serializeJson(doc, out);
  return out;
}

void streamSerializeCrossPointState(const CrossPointState& s, JsonSink& sink) {
  JsonDocument doc;
  populateDoc(s, doc);
  // ArduinoJson's serializeJson duck-types the sink: any object with
  // write(uint8_t) and write(uint8_t*, size_t) works. JsonSink provides
  // exactly that interface, so bytes flow straight to the underlying
  // HalFile (device) or std::string (host) without an intermediate buffer.
  serializeJson(doc, sink);
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
  s.sleepFavoritesCapReached = doc["sleepFavoritesCapReached"] | false;
  s.pendingSleepWallpapersMovedToPause = doc["pendingSleepWallpapersMovedToPause"] | (uint16_t)0;

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

  s.customFontLegacyCleanupDone = doc["customFontLegacyCleanupDone"] | (uint8_t)0;
  return true;
}

}  // namespace persist
}  // namespace crosspoint
