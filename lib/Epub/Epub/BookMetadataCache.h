#pragma once

#include <HalStorage.h>

#include <algorithm>
#include <deque>
#include <functional>
#include <string>

#ifndef UNIT_TEST_HOST
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

class BookMetadataCache {
 public:
  struct BookMetadata {
    std::string title;
    std::string author;
    std::string language;
    std::string coverItemHref;
    std::string textReferenceHref;
  };

  struct SpineEntry {
    std::string href;
    size_t cumulativeSize;
    int16_t tocIndex;

    SpineEntry() : cumulativeSize(0), tocIndex(-1) {}
    SpineEntry(std::string href, const size_t cumulativeSize, const int16_t tocIndex)
        : href(std::move(href)), cumulativeSize(cumulativeSize), tocIndex(tocIndex) {}
  };

  struct TocEntry {
    std::string title;
    std::string href;
    std::string anchor;
    uint8_t level;
    int16_t spineIndex;

    TocEntry() : level(0), spineIndex(-1) {}
    TocEntry(std::string title, std::string href, std::string anchor, const uint8_t level, const int16_t spineIndex)
        : title(std::move(title)),
          href(std::move(href)),
          anchor(std::move(anchor)),
          level(level),
          spineIndex(spineIndex) {}
  };

 private:
  std::string cachePath;
  size_t lutOffset;
  uint16_t spineCount;
  uint16_t tocCount;
  bool loaded;
  bool buildMode;

  FsFile bookFile;
  // Temp file handles during build
  FsFile spineFile;
  FsFile tocFile;

  // Serialises the seek+read sequence on the single shared `bookFile` cursor.
  // load()/getSpineEntry()/getTocEntry() all move one shared file position. The
  // HAL makes each individual seek and each individual read atomic, but NOT the
  // seek->read PAIR, so without this lock a concurrent reader can land its read
  // on another caller's seek offset and return the wrong record. At book open
  // the render task builds the status-bar title (getTocEntry) while the loop
  // task resolves the text-reference spine index (getSpineEntry) on the same
  // handle; the corrupted read surfaced as a chapter title showing a spine href
  // ("OEBPS/partNNNN.xhtml") or an empty title falling back to "Unnamed".
#ifndef UNIT_TEST_HOST
  SemaphoreHandle_t fileMutex_ = nullptr;
#else
  void* fileMutex_ = nullptr;  // host build is single-threaded; lock is a no-op
#endif

  // Index for fast href→spineIndex lookup (used only for large EPUBs)
  struct SpineHrefIndexEntry {
    uint64_t hrefHash;  // FNV-1a 64-bit hash
    uint16_t hrefLen;   // length for collision reduction
    int16_t spineIndex;
  };
  std::deque<SpineHrefIndexEntry> spineHrefIndex;
  bool useSpineHrefIndex = false;

  static constexpr uint16_t LARGE_SPINE_THRESHOLD = 400;

  // FNV-1a 64-bit hash function
  static uint64_t fnvHash64(const std::string& s) {
    uint64_t hash = 14695981039346656037ull;
    for (char c : s) {
      hash ^= static_cast<uint8_t>(c);
      hash *= 1099511628211ull;
    }
    return hash;
  }

  uint32_t writeSpineEntry(FsFile& file, const SpineEntry& entry) const;
  uint32_t writeTocEntry(FsFile& file, const TocEntry& entry) const;
  SpineEntry readSpineEntry(FsFile& file) const;
  TocEntry readTocEntry(FsFile& file) const;

 public:
  BookMetadata coreMetadata;

  explicit BookMetadataCache(std::string cachePath)
      : cachePath(std::move(cachePath)), lutOffset(0), spineCount(0), tocCount(0), loaded(false), buildMode(false) {
#ifndef UNIT_TEST_HOST
    fileMutex_ = xSemaphoreCreateMutex();
#endif
  }
  ~BookMetadataCache() {
#ifndef UNIT_TEST_HOST
    if (fileMutex_) {
      vSemaphoreDelete(fileMutex_);
    }
#endif
  }

  // Building phase (stream to disk immediately)
  bool beginWrite();
  bool beginContentOpfPass();
  void createSpineEntry(const std::string& href);
  bool endContentOpfPass();
  bool beginTocPass();
  void createTocEntry(const std::string& title, const std::string& href, const std::string& anchor, uint8_t level);
  bool endTocPass();
  bool endWrite();
  bool cleanupTmpFiles() const;

  // Post-processing to update mappings and sizes
  bool buildBookBin(const std::string& epubPath, const BookMetadata& metadata,
                    const std::function<void(int)>& progressCallback = nullptr);

  // Reading phase (read mode)
  bool load(const std::function<void(int)>& progressCallback = nullptr);
  SpineEntry getSpineEntry(int index);
  TocEntry getTocEntry(int index);
  int getSpineCount() const { return spineCount; }
  int getTocCount() const { return tocCount; }
  bool isLoaded() const { return loaded; }
};
