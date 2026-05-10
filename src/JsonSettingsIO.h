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

class CrossPointSettings;
class WifiCredentialStore;
class KOReaderCredentialStore;
class RecentBooksStore;
class ReadingThemeStore;
struct ReadingTheme;

namespace JsonSettingsIO {

// ---- Atomic read/write helper ----
String safeReadFile(const char* path);

// Atomic write helper: write→.tmp, rotate target→.bak, rename .tmp→target.
// Exposed so background-write paths (AsyncWriter) can reuse the same atomic
// rotation as synchronous savers.
bool safeWriteFile(const char* path, const String& json);

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

// KOReaderCredentialStore
bool saveKOReader(const KOReaderCredentialStore& store, const char* path);
bool loadKOReader(KOReaderCredentialStore& store, const char* json, bool* needsResave = nullptr);

// RecentBooksStore
bool saveRecentBooks(const RecentBooksStore& store, const char* path);
bool loadRecentBooks(RecentBooksStore& store, const char* json);
// Serialize the recent-books store to a JSON String without writing to disk.
// Lets callers snapshot the store on the caller's thread and hand the
// resulting String off to a background SD writer.
String serializeRecentBooks(const RecentBooksStore& store);

// ReadingThemeStore
bool saveReadingTheme(const ReadingTheme& theme, const char* path);
bool loadReadingTheme(ReadingTheme& theme, const char* json);
bool saveReadingThemes(const ReadingThemeStore& store, const char* path);
bool loadReadingThemes(ReadingThemeStore& store, const char* json);

}  // namespace JsonSettingsIO
