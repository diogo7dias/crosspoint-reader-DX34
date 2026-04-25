#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "EpdBinFormat.h"
#include "EpdFont.h"
#include "EpdFontData.h"

// SD-backed loader for a single CPBN .bin file. Owns a heap buffer
// holding the entire file; presents an EpdFontData view on top so the
// built-in renderer + FontDecompressor consume it without branching.
// Must outlive any EpdFont/EpdFontFamily constructed from its data().
namespace crosspoint {
namespace binfont {

class EpdBinFontLoader {
 public:
  EpdBinFontLoader() = default;
  ~EpdBinFontLoader();
  EpdBinFontLoader(const EpdBinFontLoader&) = delete;
  EpdBinFontLoader& operator=(const EpdBinFontLoader&) = delete;

  bool openFromFile(const std::string& path);

  // Header-only precheck without allocating the full buffer. Used by
  // the upload endpoint before committing the atomic rename.
  static bool validateFile(const std::string& path, std::string* error = nullptr);

  bool isOpen() const { return buffer_ != nullptr; }
  const EpdFontData* data() const { return isOpen() ? &fontData_ : nullptr; }
  uint8_t sizePt() const { return header_.sizePt; }
  uint8_t variant() const { return header_.variant; }
  const std::string& lastError() const { return lastError_; }

 private:
  uint8_t* buffer_ = nullptr;
  uint32_t fileBytes_ = 0;
  Header header_ = {};
  EpdFontData fontData_ = {};
  std::string lastError_;

  void release();
};

}  // namespace binfont
}  // namespace crosspoint
