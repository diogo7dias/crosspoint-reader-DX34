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
}  // namespace JsonSettingsIO

class RecentBooksStore {
  // Static instance
  static RecentBooksStore instance;

  std::vector<RecentBook> recentBooks;

  friend bool JsonSettingsIO::loadRecentBooks(RecentBooksStore&, const char*);

 public:
  ~RecentBooksStore() = default;

  // Get singleton instance
  static RecentBooksStore& getInstance() { return instance; }

  // Add a book to the recent list (moves to front if already exists)
  void addBook(const std::string& path, const std::string& title, const std::string& author,
               const std::string& coverBmpPath);

  void updateBook(const std::string& path, const std::string& title, const std::string& author,
                  const std::string& coverBmpPath);
  // Update only the cached reading percent for a book; no-op if the book is
  // not registered. Used by the reader on exit / progress save so the home
  // screen can show progress without re-opening every recent EPUB.
  void setPercent(const std::string& path, int percent);

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
};

// Helper macro to access recent books store
#define RECENT_BOOKS RecentBooksStore::getInstance()
