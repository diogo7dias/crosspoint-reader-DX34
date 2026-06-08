/**
 * @file JsonSettingsIO.h
 * @brief JSON serialization/deserialization for CrossPointSettings and CrossPointState.
 *
 * Provides safeWriteFile() for atomic writes (write .tmp, rename old to .bak,
 * rename .tmp to target) and safeReadFile() for reads with fallback to .bak.
 * This prevents data loss if the device loses power mid-save.
 *
 * All settings fields use ArduinoJson's "value | default" pattern so that
 * missing keys (from older firmware versions) silently fall back to defaults.
 */
#pragma once
#include <ArduinoJson.h>
#include <WString.h>

#include <functional>

class Print;
class CrossPointSettings;
class WifiCredentialStore;
class KOReaderCredentialStore;
class RecentBooksStore;
class ReadingThemeStore;
struct ReadingTheme;

namespace JsonSettingsIO {

// ---- Atomic read/write helper ----
String safeReadFile(const char* path);

// Stream-parse a JSON file into `doc` with the same crash-safe fallback order
// as safeReadFile (primary → .bak → .tmp). ArduinoJson reads incrementally
// from the open file, so the payload is never first slurped into a full-size
// String — peak heap is just the parsed document. Returns DeserializationError
// ::Ok on the first tier that parses; a non-Ok error if none did. Prefer the
// load*FromFile() helpers below over calling this directly.
DeserializationError safeDeserializeFile(const char* path, JsonDocument& doc);

// Atomic write helper: write→.tmp, rotate target→.bak, rename .tmp→target.
// Exposed so background-write paths (AsyncWriter) can reuse the same atomic
// rotation as synchronous savers.
bool safeWriteFile(const char* path, const String& json);

// Streaming atomic write: same crash-safe .tmp→.bak→target rotation as
// safeWriteFile, but `serialize` writes straight to the open file (a Print)
// instead of the caller first building a full-size String. Use for the larger
// payloads (recent books, reading themes) so peak heap is just the JsonDocument
// during the write, not document + serialized String. `serialize` returns true
// iff it wrote successfully (typically `serializeJson(doc, out) > 0`).
bool safeWriteFileStreamed(const char* path, const std::function<bool(Print&)>& serialize);

// CrossPointSettings
bool saveSettings(const CrossPointSettings& s, const char* path);
bool loadSettings(CrossPointSettings& s, const char* json, bool* needsResave = nullptr);

// Populate a JsonDocument with the settings schema. Shared between
// saveSettings (string + safeWrite path) and the streaming serializer in
// CrossPointSettingsJson.cpp (heap-safe stream-to-tmp path) so the schema
// lives in exactly one place.
void populateSettingsDoc(const CrossPointSettings& s, JsonDocument& doc);

// WifiCredentialStore
bool saveWifi(const WifiCredentialStore& store, const char* path);
bool loadWifi(WifiCredentialStore& store, const char* json, bool* needsResave = nullptr);
// Streaming load straight from `path` (primary → .bak → .tmp). Equivalent to
// safeReadFile + loadWifi but without holding the whole file in a String.
bool loadWifiFromFile(WifiCredentialStore& store, const char* path, bool* needsResave = nullptr);

// KOReaderCredentialStore
bool saveKOReader(const KOReaderCredentialStore& store, const char* path);
bool loadKOReader(KOReaderCredentialStore& store, const char* json, bool* needsResave = nullptr);

// RecentBooksStore
bool saveRecentBooks(const RecentBooksStore& store, const char* path);
bool loadRecentBooks(RecentBooksStore& store, const char* json);
// Streaming load straight from `path` (primary → .bak → .tmp). Equivalent to
// safeReadFile + loadRecentBooks but without holding the whole file in a
// String — recent.json is the largest of the persisted JSON files.
bool loadRecentBooksFromFile(RecentBooksStore& store, const char* path);
// Serialize the recent-books store to a JSON String without writing to disk.
// Lets callers snapshot the store on the caller's thread and hand the
// resulting String off to a background SD writer.
String serializeRecentBooks(const RecentBooksStore& store);

// ReadingThemeStore
bool saveReadingTheme(const ReadingTheme& theme, const char* path);
bool loadReadingTheme(ReadingTheme& theme, const char* json);
// Streaming load of a single reading-theme object straight from `path`.
bool loadReadingThemeFromFile(ReadingTheme& theme, const char* path);
bool saveReadingThemes(const ReadingThemeStore& store, const char* path);
bool loadReadingThemes(ReadingThemeStore& store, const char* json);
// Streaming load straight from `path` (primary → .bak → .tmp). reading_themes
// .json (up to 16 themes × ~46 fields) is the other large persisted JSON file.
bool loadReadingThemesFromFile(ReadingThemeStore& store, const char* path);

}  // namespace JsonSettingsIO
