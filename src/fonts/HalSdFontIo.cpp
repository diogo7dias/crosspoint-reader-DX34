#include "fonts/HalSdFontIo.h"

#ifdef CROSSPOINT_SD_FONTS

#include <new>

#include <EpdBinExport.h>
#include <HalStorage.h>
#include <Logging.h>

namespace crosspoint {
namespace fonts {
namespace {

// ByteSink over an open HalFile (export path).
class HalFileSink : public binfont::ByteSink {
 public:
  explicit HalFileSink(HalFile* f) : f_(f) {}
  bool write(const uint8_t* data, size_t len) override { return f_->write(data, len) == len; }

 private:
  HalFile* f_;
};

// BlobSource over a HalFile held open for the font's active lifetime (read path).
// One of these exists per open weight variant of the active reader font; the
// manager releases it (closing the handle) when the font deactivates.
class HalFileSource : public binfont::BlobSource {
 public:
  bool open(const char* path) {
    if (!Storage.openFileForRead("CPBN", path, file_)) return false;
    if (!file_.isOpen()) return false;
    size_ = static_cast<uint32_t>(file_.size());
    return true;
  }
  int read(uint32_t offset, uint8_t* dst, size_t len) override {
    if (!file_.seek(offset)) return -1;
    return file_.read(dst, len);
  }
  uint32_t size() const override { return size_; }

 private:
  HalFile file_;
  uint32_t size_ = 0;
};

}  // namespace

bool HalSdFontIo::exists(const char* path) { return Storage.exists(path); }

bool HalSdFontIo::exportBlob(const char* path, const EpdFontData& flashData, uint16_t sizePt) {
  Storage.mkdir("/fonts");
  HalFile out;
  if (!Storage.openFileForWrite("CPBN", path, out) || !out.isOpen()) {
    LOG_ERR("SDF", "export: open-for-write failed: %s", path);
    return false;
  }
  HalFileSink sink(&out);
  const bool ok = binfont::exportFontBlob(flashData, /*variant=*/0, sizePt, sink);
  out.close();
  if (!ok) {
    LOG_ERR("SDF", "export: serialise failed, removing %s", path);
    Storage.remove(path);
    return false;
  }
  LOG_INF("SDF", "exported font pack %s", path);
  return true;
}

binfont::BlobSource* HalSdFontIo::openSource(const char* path) {
  auto* src = new (std::nothrow) HalFileSource();
  if (src == nullptr) return nullptr;
  if (!src->open(path)) {
    delete src;
    return nullptr;
  }
  return src;
}

void HalSdFontIo::releaseSource(binfont::BlobSource* src) { delete src; }

}  // namespace fonts
}  // namespace crosspoint

#endif  // CROSSPOINT_SD_FONTS
