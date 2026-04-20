#pragma once

#include <string>

// BackupMirror: once-per-session snapshot of important files to
// /.crosspoint/backups/ using flat naming. Provides last-resort recovery
// if primary and .bak are both missing (e.g. cache dir was wiped by a bug).
namespace backup {

/// Copy all known important files (per-book caches, quotes sidecars, global
/// settings) from their primary locations into /.crosspoint/backups/ using
/// flat naming. Called on session boundaries (sleep entry). Safe to call
/// repeatedly; failures are logged but do not throw.
void snapshotAll();

/// Compute the flat mirror filename for a per-book cache file.
/// cachePath is the full path to the book cache dir (e.g.
/// "/.crosspoint/epub_1234567890"); logicalFile is the file inside it
/// (e.g. "progress.bin").
std::string flatNameForCacheFile(const std::string& cachePath, const std::string& logicalFile);

/// Compute the flat mirror filename for a _QUOTES.txt sidecar path.
/// quotesPath is the full sidecar path (e.g. "/recents/My Book_QUOTES.txt").
std::string flatNameForQuotesPath(const std::string& quotesPath);

/// If /.crosspoint/backups/<flatName> exists, copy it into destPath and
/// return true. Used by load paths as a last-resort recovery after primary
/// and .bak have both failed.
bool restoreFromMirror(const std::string& flatName, const std::string& destPath);

}  // namespace backup
