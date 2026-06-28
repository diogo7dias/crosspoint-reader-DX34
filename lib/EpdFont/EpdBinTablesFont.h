#pragma once

#include <cstddef>
#include <cstdint>
#include <new>

#include "EpdBinFontLoader.h"  // BlobSource
#include "EpdBinFormat.h"
#include "EpdBinTables.h"
#include "EpdFontData.h"

// Opens a Tier-2 CPBN font (.bin with kFlagTablesInFile set) into a complete,
// ready-to-render EpdFontData. Where the Tier-1 EpdBinFontLoader only streams the
// bitmap and relies on flash-resident tables, this loader has no flash
// counterpart: it reads the serialized table section into a heap buffer,
// reconstructs the glyph / interval / group / kerning / ligature tables (aliasing
// that buffer, groups expanded into a heap array), and wires the same streaming
// bitmap callback for the blob that follows the tables.
//
// The two heap allocations (the table section, a few KB; the expanded group
// array, tens of bytes) live only while this object is open, i.e. while the font
// is the active reader font — the SdFontManager active-set swap closes it on
// switch, so the device never pins more than one font's tables at a time.
//
// Non-copyable / non-movable: it owns raw heap pointers and the EpdFontData's
// array pointers alias them (and bitmapCtx points back at `this`), so any
// relocation would dangle. The manager holds it in place.

namespace crosspoint {
namespace binfont {

class EpdBinTablesFont {
 public:
  EpdBinTablesFont() = default;
  ~EpdBinTablesFont() { close(); }
  EpdBinTablesFont(const EpdBinTablesFont&) = delete;
  EpdBinTablesFont& operator=(const EpdBinTablesFont&) = delete;
  EpdBinTablesFont(EpdBinTablesFont&&) = delete;
  EpdBinTablesFont& operator=(EpdBinTablesFont&&) = delete;

  // Validates and loads a Tier-2 font from `src`. On kOk the loader is open and
  // font() returns a usable EpdFontData; any other result leaves it closed so
  // the caller falls back to a flash-resident font.
  BlobReject open(BlobSource* src) {
    close();
    if (src == nullptr) return kTooSmall;

    uint8_t bhBytes[sizeof(BlobHeader)];
    if (src->read(0, bhBytes, sizeof(bhBytes)) != static_cast<int>(sizeof(bhBytes))) return kTooSmall;
    BlobHeader bh{};
    const FontBlobExpectation ignored{0, 0, 0};
    const BlobReject hr = parseHeader(bhBytes, sizeof(bhBytes), ignored, &bh);
    if (hr != kOk) return hr;
    if ((bh.flags & kFlagTablesInFile) == 0) return kBadVersion;  // not a Tier-2 blob

    uint8_t thBytes[sizeof(TablesHeader)];
    if (src->read(sizeof(BlobHeader), thBytes, sizeof(thBytes)) != static_cast<int>(sizeof(thBytes))) {
      return kTooSmall;
    }
    TablesHeader th{};
    for (size_t i = 0; i < sizeof(TablesHeader); ++i) reinterpret_cast<uint8_t*>(&th)[i] = thBytes[i];
    if (th.magic != kTablesMagic) return kBadMagic;
    if (th.version != kTableSectionVersion) return kBadVersion;

    const uint32_t sectionLen = static_cast<uint32_t>(sizeof(TablesHeader)) + th.tablesByteLength;
    const uint64_t bmpOffset = static_cast<uint64_t>(sizeof(BlobHeader)) + sectionLen;
    if (bmpOffset + bh.bitmapBlobSize > src->size()) return kSizeOverrun;

    uint8_t* tables = new (std::nothrow) uint8_t[sectionLen];
    if (tables == nullptr) return kOom;
    if (src->read(sizeof(BlobHeader), tables, sectionLen) != static_cast<int>(sectionLen)) {
      delete[] tables;
      return kTooSmall;
    }

    EpdFontGroup* groups = nullptr;
    if (th.groupCount > 0) {
      groups = new (std::nothrow) EpdFontGroup[th.groupCount];
      if (groups == nullptr) {
        delete[] tables;
        return kOom;
      }
    }

    EpdFontData fd{};
    const BlobReject pr = parseTablesSection(tables, sectionLen, bh, &fd, groups, th.groupCount);
    if (pr != kOk) {
      delete[] tables;
      delete[] groups;
      return pr;
    }

    // Verify the bitmap blob integrity by streaming it (no large contiguous alloc).
    uint32_t crc = kCrc32Init;
    uint8_t chunk[256];
    uint32_t pos = static_cast<uint32_t>(bmpOffset);
    uint32_t remaining = bh.bitmapBlobSize;
    while (remaining > 0) {
      const uint32_t n = remaining < sizeof(chunk) ? remaining : static_cast<uint32_t>(sizeof(chunk));
      if (src->read(pos, chunk, n) != static_cast<int>(n)) {
        delete[] tables;
        delete[] groups;
        return kTooSmall;
      }
      crc = crc32Update(crc, chunk, n);
      pos += n;
      remaining -= n;
    }
    if (crc32Finish(crc) != bh.blobCrc32) {
      delete[] tables;
      delete[] groups;
      return kBadCrc;
    }

    tables_ = tables;
    groups_ = groups;
    src_ = src;
    bitmapOffset_ = static_cast<uint32_t>(bmpOffset);
    blobSize_ = bh.bitmapBlobSize;
    font_ = fd;
    font_.bitmapCtx = this;
    font_.readBitmapBytes = &readBitmapTrampoline;
    open_ = true;
    return kOk;
  }

  bool isOpen() const { return open_; }
  const EpdFontData* font() const { return open_ ? &font_ : nullptr; }

  // Releases the heap tables and detaches the source. Safe to call repeatedly.
  void close() {
    delete[] tables_;
    tables_ = nullptr;
    delete[] groups_;
    groups_ = nullptr;
    src_ = nullptr;
    font_ = EpdFontData{};
    bitmapOffset_ = 0;
    blobSize_ = 0;
    open_ = false;
  }

  // Matches EpdFontData::readBitmapBytes. `offset` is relative to the blob start
  // (the compressed-group offset FontDecompressor holds), bounds-checked so a bad
  // group offset cannot read past the blob into neighbouring file data.
  static int readBitmapTrampoline(void* ctx, uint32_t offset, uint8_t* dst, size_t len) {
    auto* self = static_cast<EpdBinTablesFont*>(ctx);
    if (self == nullptr || self->src_ == nullptr) return -1;
    if (static_cast<uint64_t>(offset) + len > self->blobSize_) return -1;
    return self->src_->read(self->bitmapOffset_ + offset, dst, len);
  }

 private:
  uint8_t* tables_ = nullptr;       // owns the serialized table section
  EpdFontGroup* groups_ = nullptr;  // owns the expanded group array
  BlobSource* src_ = nullptr;       // not owned
  EpdFontData font_{};              // arrays alias tables_/groups_; bitmapCtx == this
  uint32_t bitmapOffset_ = 0;       // absolute file offset of the bitmap blob
  uint32_t blobSize_ = 0;
  bool open_ = false;
};

}  // namespace binfont
}  // namespace crosspoint
