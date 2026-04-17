#include "BookProgress.h"

#include <Epub.h>
#include <HalStorage.h>
#include <Serialization.h>
#include <Txt.h>
#include <Xtc.h>

#include "Paths.h"
#include "StringUtils.h"

namespace {
constexpr const char* kUnreadPrefix = "[ ]";

template <typename T>
T clampValue(T value, T min, T max) {
  if (value < min) return min;
  if (value > max) return max;
  return value;
}

std::optional<int> getEpubPercent(const std::string& path) {
  Epub epub(path, Paths::kDataDir);
  if (!epub.load(false, true)) {
    return std::nullopt;
  }

  FsFile f;
  if (!Storage.openFileForRead("BPR", epub.getCachePath() + "/progress.bin", f)) {
    return std::nullopt;
  }

  uint8_t data[6];
  if (f.read(data, sizeof(data)) != sizeof(data)) {
    f.close();
    return std::nullopt;
  }
  f.close();

  const int currentSpineIndex = data[0] | (data[1] << 8);
  const int currentPage = data[2] | (data[3] << 8);
  const int pageCount = data[4] | (data[5] << 8);
  const int spineCount = epub.getSpineItemsCount();
  if (pageCount <= 0 || spineCount <= 0) {
    return std::nullopt;
  }

  int safeSpineIndex = currentSpineIndex;
  float chapterProgress = 0.0f;
  if (safeSpineIndex >= spineCount) {
    safeSpineIndex = spineCount - 1;
    chapterProgress = 1.0f;
  } else {
    const int safeCurrentPage = clampValue(currentPage, 0, pageCount - 1);
    chapterProgress = static_cast<float>(safeCurrentPage) / static_cast<float>(pageCount);
  }

  const int percent = static_cast<int>(epub.calculateProgress(safeSpineIndex, chapterProgress) * 100.0f + 0.5f);
  return clampValue(percent, 0, 100);
}

std::optional<int> getXtcPercent(const std::string& path) {
  Xtc xtc(path, Paths::kDataDir);
  if (!xtc.load()) {
    return std::nullopt;
  }

  FsFile f;
  if (!Storage.openFileForRead("BPR", xtc.getCachePath() + "/progress.bin", f)) {
    return std::nullopt;
  }

  uint8_t data[4];
  if (f.read(data, sizeof(data)) != sizeof(data)) {
    f.close();
    return std::nullopt;
  }
  f.close();

  const uint32_t currentPage = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
  const uint32_t pageCount = xtc.getPageCount();
  if (pageCount == 0) {
    return std::nullopt;
  }
  const uint32_t safePage = (currentPage >= pageCount) ? (pageCount - 1) : currentPage;
  return clampValue(static_cast<int>(xtc.calculateProgress(safePage)), 0, 100);
}

std::optional<int> getTxtPercent(const std::string& path) {
  Txt txt(path, Paths::kDataDir);
  const std::string cachePath = txt.getCachePath();

  FsFile progressFile;
  if (!Storage.openFileForRead("BPR", cachePath + "/progress.bin", progressFile)) {
    return std::nullopt;
  }

  uint8_t progressData[4];
  if (progressFile.read(progressData, sizeof(progressData)) != sizeof(progressData)) {
    progressFile.close();
    return std::nullopt;
  }
  progressFile.close();

  const int currentPage =
      static_cast<int>(static_cast<uint32_t>(progressData[0]) | (static_cast<uint32_t>(progressData[1]) << 8) |
                       (static_cast<uint32_t>(progressData[2]) << 16) | (static_cast<uint32_t>(progressData[3]) << 24));

  FsFile indexFile;
  if (!Storage.openFileForRead("BPR", cachePath + "/index.bin", indexFile)) {
    return std::nullopt;
  }

  uint32_t magic = 0;
  uint8_t version = 0;
  uint32_t fileSize = 0;
  int32_t viewportWidth = 0;
  int32_t linesPerPage = 0;
  int32_t fontId = 0;
  int32_t marginHorizontal = 0;
  int32_t marginTop = 0;
  int32_t marginBottom = 0;
  uint8_t alignment = 0;
  uint32_t totalPages = 0;

  serialization::readPod(indexFile, magic);
  serialization::readPod(indexFile, version);
  serialization::readPod(indexFile, fileSize);
  serialization::readPod(indexFile, viewportWidth);
  serialization::readPod(indexFile, linesPerPage);
  serialization::readPod(indexFile, fontId);
  serialization::readPod(indexFile, marginHorizontal);
  serialization::readPod(indexFile, marginTop);
  serialization::readPod(indexFile, marginBottom);
  serialization::readPod(indexFile, alignment);
  serialization::readPod(indexFile, totalPages);
  indexFile.close();

  if (totalPages == 0) {
    return std::nullopt;
  }

  const int clampedPage = clampValue(currentPage, 0, static_cast<int>(totalPages - 1));
  const int percent =
      static_cast<int>((static_cast<float>(clampedPage + 1) * 100.0f) / static_cast<float>(totalPages) + 0.5f);
  return clampValue(percent, 0, 100);
}
}  // namespace

namespace BookProgress {

std::optional<int> getPercent(const std::string& path) {
  if (StringUtils::checkFileExtension(path, ".epub")) {
    return getEpubPercent(path);
  }
  if (StringUtils::checkFileExtension(path, ".xtc") || StringUtils::checkFileExtension(path, ".xtch")) {
    return getXtcPercent(path);
  }
  if (StringUtils::checkFileExtension(path, ".txt") || StringUtils::checkFileExtension(path, ".md")) {
    return getTxtPercent(path);
  }
  return std::nullopt;
}

std::string getPrefix(const std::string& path) {
  const auto percent = getPercent(path);
  if (!percent.has_value() || percent.value() <= 1) {
    return kUnreadPrefix;
  }
  return "[" + std::to_string(percent.value()) + " %]";
}

std::string withPrefix(const std::string& path, const std::string& title) { return getPrefix(path) + " " + title; }

}  // namespace BookProgress
