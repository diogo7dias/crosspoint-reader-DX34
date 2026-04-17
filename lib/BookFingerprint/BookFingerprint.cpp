#include "BookFingerprint.h"

#include <HalStorage.h>
#include <Logging.h>

#include <functional>

namespace {

constexpr size_t SAMPLE_SIZE = 256;
constexpr int NUM_SAMPLES = 5;

// FNV-1a 64-bit (same algorithm as ZipFile::fnvHash64)
uint64_t fnv1a(const uint8_t* data, size_t len, uint64_t hash = 14695981039346656037ull) {
  for (size_t i = 0; i < len; i++) {
    hash ^= data[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

}  // namespace

uint64_t BookFingerprint::compute(const std::string& filepath) {
  FsFile file;
  if (!Storage.openFileForRead("BFP", filepath, file)) {
    return 0;
  }

  const uint64_t fileSize = file.fileSize();
  if (fileSize == 0) {
    file.close();
    return 0;
  }

  // Start hash with file size (8 LE bytes)
  uint8_t sizeBuf[8];
  for (int i = 0; i < 8; i++) {
    sizeBuf[i] = static_cast<uint8_t>((fileSize >> (i * 8)) & 0xFF);
  }
  uint64_t hash = fnv1a(sizeBuf, 8);

  // Sample at 5 offsets: 0, size/4, size/2, 3*size/4, size-SAMPLE_SIZE
  const uint64_t offsets[NUM_SAMPLES] = {
      0, fileSize / 4, fileSize / 2, (fileSize * 3) / 4, fileSize > SAMPLE_SIZE ? fileSize - SAMPLE_SIZE : 0,
  };

  uint8_t buf[SAMPLE_SIZE];
  for (int i = 0; i < NUM_SAMPLES; i++) {
    const uint64_t offset = offsets[i];
    if (offset >= fileSize) continue;

    if (!file.seekSet(offset)) continue;

    const size_t toRead = std::min(static_cast<size_t>(SAMPLE_SIZE), static_cast<size_t>(fileSize - offset));
    const size_t bytesRead = file.read(buf, toRead);
    if (bytesRead > 0) {
      hash = fnv1a(buf, bytesRead, hash);
    }
  }

  file.close();
  return hash;
}

std::string BookFingerprint::cacheDirName(const char* prefix, const std::string& filepath,
                                          const std::string& cacheBase) {
  // Compute content fingerprint
  const uint64_t fp = compute(filepath);

  std::string newDir;
  if (fp != 0) {
    newDir = cacheBase + "/" + prefix + "_" + std::to_string(fp);
  } else {
    // Fallback: path-hash (file couldn't be read)
    LOG_ERR("BFP", "Cannot fingerprint '%s', falling back to path-hash", filepath.c_str());
    const size_t pathHash = std::hash<std::string>{}(filepath);
    return cacheBase + "/" + prefix + "_" + std::to_string(pathHash);
  }

  // If the new content-hash dir already exists, we're done
  if (Storage.exists(newDir.c_str())) {
    return newDir;
  }

  // Try to migrate from old path-hash directory
  const size_t pathHash = std::hash<std::string>{}(filepath);
  const std::string oldDir = cacheBase + "/" + prefix + "_" + std::to_string(pathHash);

  if (Storage.exists(oldDir.c_str())) {
    if (Storage.rename(oldDir.c_str(), newDir.c_str())) {
      LOG_INF("BFP", "Migrated cache: %s -> %s", oldDir.c_str(), newDir.c_str());
    } else {
      LOG_ERR("BFP", "Failed to migrate cache: %s -> %s", oldDir.c_str(), newDir.c_str());
    }
  }

  return newDir;
}
