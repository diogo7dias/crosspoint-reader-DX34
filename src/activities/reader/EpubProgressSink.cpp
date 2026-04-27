#include "EpubProgressSink.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstdint>

#include "../../persist/AsyncWriter.h"

namespace crosspoint {
namespace reader {

namespace {

// The actual blocking SD work. Runs on either the AsyncWriter task (typical)
// or the caller (boot fallback before AsyncWriter::start()). Storage.* is
// mutex-serialized internally so concurrent main-thread access is safe.
bool writeProgressSync(const std::string& cachePath, int spineCount, ReaderPosition pos) {
  if (spineCount <= 0) {
    return false;
  }

  int32_t spineIndex = pos.spineIndex;
  int32_t page = pos.page;
  int32_t pageCount = pos.pageCount;
  if (spineIndex < 0) {
    spineIndex = 0;
  } else if (spineIndex >= spineCount) {
    spineIndex = spineCount - 1;
    page = UINT16_MAX;  // legacy past-end sentinel preserved for byte-compat
  }
  if (pageCount <= 0) {
    pageCount = 1;
  }

  const std::string progPath = cachePath + "/progress.bin";
  const std::string tmpPath = cachePath + "/progress_tmp.bin";
  const std::string bakPath = cachePath + "/progress.bin.bak";

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
    if (Storage.exists(bakPath.c_str())) {
      Storage.remove(bakPath.c_str());
    }
    Storage.rename(progPath.c_str(), bakPath.c_str());
  }
  Storage.rename(tmpPath.c_str(), progPath.c_str());

  LOG_DBG("ERS", "Progress saved: Chapter %d, Page %d", static_cast<int>(spineIndex), static_cast<int>(page));
  return true;
}

}  // namespace

bool EpubProgressSink::write(const ReaderPosition& pos) {
  // Capture by value so the lambda is self-contained on the AsyncWriter task.
  // EpubProgressSink instances may be destroyed while a write is in flight
  // (e.g. user closes book mid-debounce); copying state avoids dangling refs.
  // Lifecycle drain (AsyncWriter::drainBlocking) at onExit / enterDeepSleep
  // guarantees no in-flight write outlives the device powering down.
  const std::string cachePath = cachePath_;
  const int spineCount = spineCount_;
  ReaderPosition snap = pos;
  ::crosspoint::persist::AsyncWriter::instance().submit(
      [cachePath, spineCount, snap]() { writeProgressSync(cachePath, spineCount, snap); });
  // Optimistic success: tracker advances lastSaved immediately. If the async
  // write later fails, the next observe() with the same position will not
  // re-dirty the tracker — same risk window as previous synchronous
  // best-effort behaviour (we never retried failed writes either).
  return true;
}

}  // namespace reader
}  // namespace crosspoint
