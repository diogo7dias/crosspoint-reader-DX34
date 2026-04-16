#pragma once

#include <cstdint>
#include <string>
#include <vector>

/// Per-book bookmark storage. Bookmarks are saved to bookmarks.json inside
/// the book's cache directory (e.g. /.crosspoint/epub_<hash>/bookmarks.json).
class BookmarkStore {
 public:
  struct Bookmark {
    int spineIndex;
    int pageNumber;
  };

  static constexpr int MAX_BOOKMARKS = 20;

  /// Load bookmarks from the cache directory's bookmarks.json.
  bool load(const std::string& cachePath);

  /// Save current bookmarks to cachePath/bookmarks.json.
  bool save(const std::string& cachePath) const;

  /// Toggle bookmark at current position.
  /// Returns 1 if added, 0 if removed, -1 if at capacity (not added).
  int toggle(int spineIndex, int pageNumber);

  /// Check if a bookmark exists at this position.
  bool has(int spineIndex, int pageNumber) const;

  /// Remove bookmark at index.
  void remove(int index);

  const std::vector<Bookmark>& getAll() const { return bookmarks; }

  int count() const { return static_cast<int>(bookmarks.size()); }

  bool empty() const { return bookmarks.empty(); }

 private:
  std::vector<Bookmark> bookmarks;
};
