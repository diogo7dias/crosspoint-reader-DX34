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

// CrossPointSettings
bool saveSettings(const CrossPointSettings& s, const char* path);
bool loadSettings(CrossPointSettings& s, const char* json, bool* needsResave = nullptr);

// WifiCredentialStore
bool saveWifi(const WifiCredentialStore& store, const char* path);
bool loadWifi(WifiCredentialStore& store, const char* json, bool* needsResave = nullptr);

// KOReaderCredentialStore
bool saveKOReader(const KOReaderCredentialStore& store, const char* path);
bool loadKOReader(KOReaderCredentialStore& store, const char* json, bool* needsResave = nullptr);

// RecentBooksStore
bool saveRecentBooks(const RecentBooksStore& store, const char* path);
bool loadRecentBooks(RecentBooksStore& store, const char* json);

// ReadingThemeStore
bool saveReadingTheme(const ReadingTheme& theme, const char* path);
bool loadReadingTheme(ReadingTheme& theme, const char* json);
bool saveReadingThemes(const ReadingThemeStore& store, const char* path);
bool loadReadingThemes(ReadingThemeStore& store, const char* json);

}  // namespace JsonSettingsIO
