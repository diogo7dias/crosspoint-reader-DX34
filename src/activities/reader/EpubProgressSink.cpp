#include "EpubProgressSink.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstdint>

namespace crosspoint {
namespace reader {

bool EpubProgressSink::write(const ReaderPosition& pos) {
  if (spineCount_ <= 0) {
    return false;
  }

  int32_t spineIndex = pos.spineIndex;
  int32_t page = pos.page;
  int32_t pageCount = pos.pageCount;
  if (spineIndex < 0) {
    spineIndex = 0;
  } else if (spineIndex >= spineCount_) {
    spineIndex = spineCount_ - 1;
    page = UINT16_MAX;  // legacy past-end sentinel preserved for byte-compat
  }
  if (pageCount <= 0) {
    pageCount = 1;
  }

  const std::string progPath = cachePath_ + "/progress.bin";
  const std::string tmpPath = cachePath_ + "/progress_tmp.bin";

  if (Storage.exists(tmpPath.c_str())) {
    Storage.remove(tmpPath.c_str());
  }

  HalFile f;
  if (!Storage.openFileForWrite("ERS", tmpPath.c_str(), f)) {
    LOG_ERR("ERS", "Could not save progress!");
    return false;
  }

  uint8_t data[6];
  data[0] = static_cast<uint8_t>(spineIndex & 0xFF);
  data[1] = static_cast<uint8_t>((spineIndex >> 8) & 0xFF);
  data[2] = static_cast<uint8_t>(page & 0xFF);
  data[3] = static_cast<uint8_t>((page >> 8) & 0xFF);
  data[4] = static_cast<uint8_t>(pageCount & 0xFF);
  data[5] = static_cast<uint8_t>((pageCount >> 8) & 0xFF);
  f.write(data, 6);
  f.close();

  if (Storage.exists(progPath.c_str())) {
    Storage.remove(progPath.c_str());
  }
  Storage.rename(tmpPath.c_str(), progPath.c_str());

  LOG_DBG("ERS", "Progress saved: Chapter %d, Page %d", static_cast<int>(spineIndex), static_cast<int>(page));
  return true;
}

}  // namespace reader
}  // namespace crosspoint
