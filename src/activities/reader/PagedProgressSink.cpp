#include "PagedProgressSink.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstdint>

#include "persist/BackupMirror.h"

namespace crosspoint {
namespace reader {

bool PagedProgressSink::write(const ReaderPosition& p) {
  const uint32_t page = p.page < 0 ? 0u : static_cast<uint32_t>(p.page);

  const std::string progPath = cachePath_ + "/progress.bin";
  const std::string tmpPath = cachePath_ + "/progress_tmp.bin";
  const std::string bakPath = cachePath_ + "/progress.bin.bak";

  if (Storage.exists(tmpPath.c_str())) {
    Storage.remove(tmpPath.c_str());
  }

  HalFile f;
  if (!Storage.openFileForWrite(logTag_, tmpPath, f)) {
    LOG_ERR(logTag_, "Could not save progress!");
    return false;
  }

  uint8_t data[4];
  data[0] = static_cast<uint8_t>(page & 0xFF);
  data[1] = static_cast<uint8_t>((page >> 8) & 0xFF);
  data[2] = static_cast<uint8_t>((page >> 16) & 0xFF);
  data[3] = static_cast<uint8_t>((page >> 24) & 0xFF);
  f.write(data, 4);
  f.close();

  // Rotate current progress.bin to progress.bin.bak before replacing.
  if (Storage.exists(progPath.c_str())) {
    if (Storage.exists(bakPath.c_str())) {
      Storage.remove(bakPath.c_str());
    }
    Storage.rename(progPath.c_str(), bakPath.c_str());
  }
  Storage.rename(tmpPath.c_str(), progPath.c_str());

  LOG_DBG(logTag_, "Progress saved: page %lu", static_cast<unsigned long>(page));
  return true;
}

// static
int PagedProgressSink::load(const std::string& cachePath, const char* logTag) {
  HalFile f;
  const std::string progPath = cachePath + "/progress.bin";
  const std::string bakPath = cachePath + "/progress.bin.bak";

  bool opened = Storage.openFileForRead(logTag, progPath, f);
  if (!opened && Storage.exists(bakPath.c_str())) {
    LOG_INF(logTag, "progress.bin missing, recovering from progress.bin.bak");
    opened = Storage.openFileForRead(logTag, bakPath, f);
  }
  if (!opened) {
    const std::string flatName = backup::flatNameForCacheFile(cachePath, "progress.bin");
    if (backup::restoreFromMirror(flatName, progPath)) {
      LOG_INF(logTag, "progress.bin recovered from mirror %s", flatName.c_str());
      opened = Storage.openFileForRead(logTag, progPath, f);
    }
  }
  if (!opened) {
    return -1;
  }

  uint8_t data[4];
  const int bytesRead = f.read(data, sizeof(data));
  f.close();
  if (bytesRead < 2) {
    return -1;
  }
  int page;
  if (bytesRead >= 4) {
    page = static_cast<int>(static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
                            (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24));
  } else {
    // Backward compatibility with older 2-byte progress files.
    page = data[0] + (data[1] << 8);
  }
  return page < 0 ? 0 : page;
}

}  // namespace reader
}  // namespace crosspoint
