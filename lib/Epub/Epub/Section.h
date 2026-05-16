#pragma once
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Epub.h"

class Page;
class GfxRenderer;

class Section {
  std::shared_ptr<Epub> epub;
  const int spineIndex;
  GfxRenderer& renderer;
  std::string filePath;
  FsFile file;
  // Deques instead of vectors so growth doesn't require a contiguous
  // realloc when the chapter has many anchors / pages. The exponential
  // doubling of std::vector hit `std::bad_alloc → abort` on a fragmented
  // heap (14 KB request, 6 KB largest contiguous) during long reading
  // sessions — see TODO.md reader-cache fragmentation entry. Deques
  // allocate small fixed-size chunks (~512 B max each) so they fit
  // through tight contiguous gaps. `operator[]` random access is still
  // O(1) — only the per-element pointer chase is slightly more expensive,
  // and the LUTs are tiny (max ~thousands of entries).
  std::deque<uint32_t> pageLut;
  std::deque<std::pair<std::string, uint16_t>> anchorLut;
  std::deque<int16_t> pageTocLut;

  void writeSectionFileHeader(int fontId, float lineCompression, uint8_t extraParagraphSpacingLevel,
                              uint8_t paragraphAlignment, uint16_t viewportWidth, uint16_t viewportHeight,
                              bool hyphenationEnabled, uint8_t wordSpacingPercent, uint8_t firstLineIndentMode,
                              uint8_t readerStyleMode, uint8_t textRenderMode, bool readerBoldSwap);
  uint32_t onPageComplete(std::unique_ptr<Page> page);

 public:
  uint16_t pageCount = 0;
  int currentPage = 0;

  explicit Section(const std::shared_ptr<Epub>& epub, const int spineIndex, GfxRenderer& renderer)
      : epub(epub),
        spineIndex(spineIndex),
        renderer(renderer),
        filePath(epub->getCachePath() + "/sections/" + std::to_string(spineIndex) + ".bin") {}
  ~Section() = default;
  bool loadSectionFile(int fontId, float lineCompression, uint8_t extraParagraphSpacingLevel,
                       uint8_t paragraphAlignment, uint16_t viewportWidth, uint16_t viewportHeight,
                       bool hyphenationEnabled, uint8_t wordSpacingPercent, uint8_t firstLineIndentMode,
                       uint8_t readerStyleMode, uint8_t textRenderMode, bool readerBoldSwap);
  bool clearCache() const;
  // Walk <cachePath>/sections/*.bin and delete every file whose stored
  // fontId differs from currentFontId. Cheap: peeks only the first 5
  // bytes of each file (version + fontId). Returns the number of files
  // deleted. Used after a font switch so orphaned per-fontId caches
  // don't accumulate on SD. Doesn't affect correctness — the existing
  // fontId-mismatch detection in loadSectionFile already triggers a
  // rebuild — but it reclaims SD space and avoids stale-cache races
  // when a future build introduces a new fontId for a renamed font.
  static size_t pruneStaleCachesForFont(const std::string& cachePath, int currentFontId);
  bool createSectionFile(int fontId, float lineCompression, uint8_t extraParagraphSpacingLevel,
                         uint8_t paragraphAlignment, uint16_t viewportWidth, uint16_t viewportHeight,
                         bool hyphenationEnabled, uint8_t wordSpacingPercent, uint8_t firstLineIndentMode,
                         uint8_t readerStyleMode, uint8_t textRenderMode, bool readerBoldSwap,
                         const std::function<void(int)>& progressFn = nullptr);
  std::unique_ptr<Page> loadPageFromSectionFile();
  std::unique_ptr<Page> loadPageFromSectionFile(int pageIndex);
  int getPageForAnchor(const std::string& anchor) const;
  std::string getCurrentAnchorForPage(int page) const;
  int getTocIndexForPage(int page) const;
};
