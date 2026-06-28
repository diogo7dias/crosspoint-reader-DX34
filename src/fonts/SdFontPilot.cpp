#include "fonts/SdFontPilot.h"

#ifdef CROSSPOINT_SD_FONTS

#include <EpdBinExport.h>
#include <EpdBinFontLoader.h>
#include <EpdBinFormat.h>
#include <HalStorage.h>
#include <Logging.h>

namespace crosspoint {
namespace fonts {
namespace {

// ByteSink writing to an open HalFile (export path).
class HalFileSink : public binfont::ByteSink {
 public:
  explicit HalFileSink(HalFile* f) : f_(f) {}
  bool write(const uint8_t* data, size_t len) override { return f_->write(data, len) == len; }

 private:
  HalFile* f_;
};

// BlobSource over a HalFile held open for the program's lifetime (consume path).
// One handle for the single pilot font, so no handle-cache is needed yet.
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

// The pilot owns exactly one SD-backed font for the whole program lifetime.
HalFileSource g_src;
binfont::EpdBinFontLoader g_loader;
EpdFontData g_sdFontData;
bool g_active = false;

bool exportIfMissing(const EpdFontData& flashData, const char* path, uint16_t sizePt) {
  if (Storage.exists(path)) return true;
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

}  // namespace

void bootstrapSdFontPilot(EpdFont& font, const EpdFontData& flashData, const char* path, uint16_t sizePt,
                          const EpdFontData* slimFallback) {
  // In a slim build flashData.bitmap is nullptr, so without SD backing the font
  // would render from a null pointer. Substitute a guaranteed flash font.
  auto failSafe = [&]() {
    if (flashData.bitmap == nullptr && slimFallback != nullptr) {
      font.data = slimFallback;
      LOG_ERR("SDF", "SD-backing failed in slim build; fell back to flash font for %s", path);
    }
  };

  if (g_active) return;  // single pilot font for now
  if (!exportIfMissing(flashData, path, sizePt)) {
    failSafe();
    return;
  }

  if (!g_src.open(path)) {
    LOG_ERR("SDF", "open-for-read failed: %s", path);
    failSafe();
    return;
  }

  const binfont::FontBlobExpectation expect{binfont::glyphCountFromIntervals(flashData), flashData.intervalCount,
                                            flashData.groupCount};
  const binfont::BlobReject r = g_loader.open(&g_src, expect);
  if (r != binfont::kOk) {
    LOG_ERR("SDF", "pack rejected (%d) for %s; keeping flash bitmap", static_cast<int>(r), path);
    failSafe();
    return;
  }

  // Stream the bitmap from SD; keep every flash table pointer + metric.
  g_sdFontData = flashData;
  g_sdFontData.bitmap = nullptr;
  g_sdFontData.bitmapCtx = g_loader.bitmapCtx();
  g_sdFontData.readBitmapBytes = &binfont::EpdBinFontLoader::readBitmapTrampoline;
  font.data = &g_sdFontData;
  g_active = true;
  LOG_INF("SDF", "%s now SD-backed (bitmap from SD, tables in flash)", path);
}

}  // namespace fonts
}  // namespace crosspoint

#endif  // CROSSPOINT_SD_FONTS
