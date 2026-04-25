#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "EpdBinFormat.h"
#include "EpdFont.h"
#include "EpdFontData.h"

// SD-backed loader for a single CPBN .bin file.
//
// Loads only the small fixed tables (header, glyph metadata, intervals,
// groups) into heap. The DEFLATE bitmap blob stays on SD and is read
// on demand via the EpdFontData read-bitmap callback. Total heap per
// loaded variant is a few KB regardless of font size, so a 4-variant
// active family at 30 pt costs roughly the same RAM as at 14 pt — the
// previous full-file-in-heap design didn't.
//
// Must outlive any EpdFont/EpdFontFamily constructed from its data();
// destruction closes the underlying HalFile.
namespace crosspoint {
namespace binfont {

class EpdBinFontLoader {
 public:
  EpdBinFontLoader() = default;
  ~EpdBinFontLoader();
  EpdBinFontLoader(const EpdBinFontLoader&) = delete;
  EpdBinFontLoader& operator=(const EpdBinFontLoader&) = delete;

  bool openFromFile(const std::string& path);

  // Same as openFromFile but writes the tables into a caller-owned
  // buffer instead of mallocing. Used by CustomBinFontManager for the
  // regular variant: a static BSS-resident buffer keeps the ~10 KB
  // tables block out of the heap, so it can never bracket the largest
  // free region during EPUB layout. The buffer must outlive the loader
  // and be at least kMaxTablesBytes wide.
  bool openFromFileExternalBuf(const std::string& path, uint8_t* externalBuf, uint32_t externalBufCap);

  // Header-only precheck without holding the file open. Used by the
  // upload endpoint before the atomic rename commits the new file.
  static bool validateFile(const std::string& path, std::string* error = nullptr);

  bool isOpen() const { return tablesBuf_ != nullptr; }
  const EpdFontData* data() const { return isOpen() ? &fontData_ : nullptr; }
  uint8_t sizePt() const { return header_.sizePt; }
  uint8_t variant() const { return header_.variant; }
  const std::string& lastError() const { return lastError_; }

 private:
  // Heap buffer holding header + glyph table + interval table + group
  // table only. fontData_ pointers all index into this region. When
  // ownsTablesBuf_ is false the buffer is caller-owned (BSS / static)
  // and release() leaves it alone.
  uint8_t* tablesBuf_ = nullptr;
  uint32_t tablesBytes_ = 0;
  bool ownsTablesBuf_ = false;

  // SD-resident bitmap blob. The HalFile stays open for the lifetime
  // of the loader; readBitmapBytes seeks + reads on each call.
  HalFile file_;
  uint32_t bitmapBlobFileOffset_ = 0;  // absolute file offset to start of blob

  Header header_ = {};
  EpdFontData fontData_ = {};
  std::string lastError_;

  void release();

  // Static trampoline matching EpdFontData::readBitmapBytes. ctx is a
  // pointer to the EpdBinFontLoader instance.
  static int readBitmapBytesCallback(void* ctx, uint32_t offset, uint8_t* dst, size_t len);
};

}  // namespace binfont
}  // namespace crosspoint
