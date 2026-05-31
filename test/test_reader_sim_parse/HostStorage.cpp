// stdio-backed implementation of the host HalStorage shadow.
#include <HalStorage.h>

#include <cstdio>
#include <utility>

HalFile::~HalFile() { close(); }

HalFile::HalFile(HalFile&& other) noexcept : f_(other.f_), size_(other.size_) {
  other.f_ = nullptr;
  other.size_ = 0;
}

HalFile& HalFile::operator=(HalFile&& other) noexcept {
  if (this != &other) {
    close();
    f_ = other.f_;
    size_ = other.size_;
    other.f_ = nullptr;
    other.size_ = 0;
  }
  return *this;
}

bool HalFile::openForRead(const char* path) {
  close();
  f_ = std::fopen(path, "rb");
  if (!f_) return false;
  std::fseek(f_, 0, SEEK_END);
  size_ = static_cast<size_t>(std::ftell(f_));
  std::fseek(f_, 0, SEEK_SET);
  return true;
}

bool HalFile::openForWrite(const char* path) {
  close();
  f_ = std::fopen(path, "wb");
  size_ = 0;
  return f_ != nullptr;
}

void HalFile::flush() {
  if (f_) std::fflush(f_);
}

size_t HalFile::size() { return size_; }

bool HalFile::seek(size_t pos) { return f_ && std::fseek(f_, static_cast<long>(pos), SEEK_SET) == 0; }
bool HalFile::seekSet(size_t offset) { return seek(offset); }
bool HalFile::seekCur(int64_t offset) { return f_ && std::fseek(f_, static_cast<long>(offset), SEEK_CUR) == 0; }

int HalFile::available() const {
  if (!f_) return 0;
  const long pos = std::ftell(f_);
  if (pos < 0) return 0;
  return static_cast<int>(size_ - static_cast<size_t>(pos));
}

size_t HalFile::position() const {
  if (!f_) return 0;
  const long pos = std::ftell(f_);
  return pos < 0 ? 0 : static_cast<size_t>(pos);
}

int HalFile::read(void* buf, size_t count) {
  if (!f_) return -1;
  return static_cast<int>(std::fread(buf, 1, count, f_));
}

int HalFile::read() {
  if (!f_) return -1;
  return std::fgetc(f_);
}

size_t HalFile::write(const void* buf, size_t count) {
  if (!f_) return 0;
  return std::fwrite(buf, 1, count, f_);
}

size_t HalFile::write(uint8_t b) { return write(&b, 1); }

size_t HalFile::getName(char*, size_t) { return 0; }

bool HalFile::close() {
  if (!f_) return true;
  const bool ok = std::fclose(f_) == 0;
  f_ = nullptr;
  size_ = 0;
  return ok;
}

HalStorage& HalStorage::getInstance() {
  static HalStorage instance;
  return instance;
}

bool HalStorage::openFileForRead(const char*, const char* path, HalFile& file) { return file.openForRead(path); }

bool HalStorage::openFileForRead(const char* mod, const std::string& path, HalFile& file) {
  return openFileForRead(mod, path.c_str(), file);
}

HalFile HalStorage::open(const char* path, oflag_t) {
  HalFile f;
  f.openForRead(path);
  return f;
}

bool HalStorage::openFileForWrite(const char*, const char* path, HalFile& file) { return file.openForWrite(path); }

bool HalStorage::openFileForWrite(const char* mod, const std::string& path, HalFile& file) {
  return openFileForWrite(mod, path.c_str(), file);
}

bool HalStorage::exists(const char* path) {
  FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  std::fclose(f);
  return true;
}

bool HalStorage::remove(const char* path) { return std::remove(path) == 0; }
