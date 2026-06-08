#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct RecentBook {
  std::string path;
  std::string title;
  std::string author;
  std::string coverBmpPath;
  // Per-book reader Bold Swap preference (0 = off, 1 = on). Persisted alongside
  // the rest of the recent-books record so it survives reboots and only
  // affects the book it was toggled on.
  uint8_t boldSwap = 0;

  // Cached reading progress (0-100, or -1 = unknown). Stored here so the
  // home screen can render the per-book percent without opening the EPUB
  // (which previously cost a spine+TOC parse per recent on every home
  // entry, fragmenting the heap badly enough to block large books from
  // opening). Updated on book exit / page turn from the reader.
  int8_t percent = -1;

  bool operator==(const RecentBook& other) const { return path == other.path; }
};

class RecentBooksStore;
namespace JsonSettingsIO {
bool loadRecentBooks(RecentBooksStore& store, const char* json);
bool loadRecentBooksFromFile(RecentBooksStore& store, const char* path);
}  // namespace JsonSettingsIO

class RecentBooksStore {
  // Static instance
  static RecentBooksStore instance;

  std::vector<RecentBook> recentBooks;

  friend bool JsonSettingsIO::loadRecentBooks(RecentBooksStore&, const char*);
  friend bool JsonSettingsIO::loadRecentBooksFromFile(RecentBooksStore&, const char*);

 public:
  ~RecentBooksStore() = default;

  // Get singleton instance
  static RecentBooksStore& getInstance() { return instance; }

  // Add a book to the recent list (moves to front if already exists)
  void addBook(const std::string& path, const std::string& title, const std::string& author,
               const std::string& coverBmpPath);

  void updateBook(const std::string& path, const std::string& title, const std::string& author,
                  const std::string& coverBmpPath);

  // True if the book's backing file is no longer present on the SD card.
  static bool isMissing(const RecentBook& book);

  // Remove entries whose backing file is no longer on the SD card. Returns
  // true if at least one entry was removed. Does NOT persist — the caller
  // decides when to saveToFile() so prune-on-add can piggy-back on the
  // following addBook write.
  bool pruneMissing();

  // Update only the cached reading percent for a book; no-op if the book is
  // not registered. Used by the reader on exit / progress save so the home
  // screen can show progress without re-opening every recent EPUB.
  // The new value is held in RAM and marked dirty; the SD write is deferred to
  // flushPercentIfDirty() so per-page-turn percent ticks don't contend with the
  // render task's page-load reads on the shared SD bus.
  void setPercent(const std::string& path, int percent);

  // Persist a pending setPercent() change to SD (via saveToFileAsync), if any.
  // Called from the reader's force-flush path (book exit / sleep / menu open),
  // which then drains the async writer. No-op when nothing changed.
  void flushPercentIfDirty() const;

  // Lookup cached reading percent. Returns 0-100 if known and the book is
  // tracked in recents, or -1 otherwise. Library list rendering uses this
  // to avoid re-opening every visible EPUB on every scroll tick.
  int getCachedPercent(const std::string& path) const;
  void removeBook(const std::string& path);

  // Move a book file (and its QUOTES sidecar) to /recents/.
  // Returns new path on success, empty string if skipped or failed.
  std::string moveBookToRecents(const std::string& bookPath);

  // Get the list of recent books (most recent first)
  const std::vector<RecentBook>& getBooks() const { return recentBooks; }

  // Maximum number of recent books stored
  static constexpr int MAX_RECENT_BOOKS = 100;

  // Get the count of recent books
  int getCount() const { return static_cast<int>(recentBooks.size()); }

  bool saveToFile() const;

  // Background-task save path. Builds the JSON snapshot synchronously on the
  // caller's thread (so the recentBooks vector isn't read mid-mutation), then
  // submits the actual SD atomic-write to AsyncWriter. Used by setPercent on
  // the per-page-turn path so a percent tick (~120 ms safeWriteFile) doesn't
  // block button polling. Lifecycle drains in persistAppState() / deep sleep
  // entry guarantee queued writes hit disk before power-down.
  void saveToFileAsync() const;

  // Per-book Bold Swap preference. Lookup is by book path; unknown paths
  // return false so first-time opens always start with bold swap OFF.
  // setBoldSwap is a no-op on unknown paths (logs an error) — callers must
  // register the book via addBook() first, which the reader always does on
  // open before the menu that exposes this toggle is reachable.
  bool getBoldSwap(const std::string& path) const;
  void setBoldSwap(const std::string& path, bool enabled);

  bool loadFromFile();
  RecentBook getDataFromBook(std::string path) const;

 private:
  bool loadFromBinaryFile();
  // Set by setPercent(), cleared by flushPercentIfDirty(). Mutable so the
  // const flush path can clear it.
  mutable bool percentDirty_ = false;
};

// Helper macro to access recent books store
#define RECENT_BOOKS RecentBooksStore::getInstance()
