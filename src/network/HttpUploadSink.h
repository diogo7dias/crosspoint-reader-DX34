#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

namespace crosspoint::net {

// Batches small HTTP multipart-upload chunks into a fixed-size buffer and
// flushes a full buffer through a caller-supplied writer. This is the shared
// core of /upload (books) and /api/fonts/upload (CPBN fonts): both used to
// hand-roll the identical copy-into-buffer-then-flush loop against different
// file types (FsFile vs HalFile) and with subtly different short-write
// handling. The writer callback abstracts the file type away, so the loop
// lives in one place and is pure logic — no SD, no Arduino, no heap policy —
// which makes the buffering edge cases (partial fills, exact-boundary flush,
// short writes) host-testable.
//
// The writer returns the number of bytes it actually wrote; anything less
// than requested is treated as a failure (disk full / SD error) and append /
// flush return false so the caller can abort the upload.
class HttpUploadSink {
 public:
  using Writer = std::function<size_t(const uint8_t* data, size_t len)>;

  explicit HttpUploadSink(size_t capacity) : capacity_(capacity) {}

  // Size the batching buffer (lazily, on UPLOAD_FILE_START). The caller is
  // responsible for any heap pre-flight before calling — under -fno-exceptions
  // a failed resize would abort, so callers gate this with a largest-free-block
  // probe. Returns true once the buffer is at full capacity. Resets position.
  bool ensureCapacity() {
    if (buffer_.size() != capacity_) buffer_.resize(capacity_);
    bufferPos_ = 0;
    return buffer_.size() == capacity_;
  }

  bool hasCapacity() const { return buffer_.size() == capacity_; }

  // Copy len bytes into the buffer, flushing through `write` whenever the
  // buffer fills. Returns false on a short write (caller should abort).
  bool append(const uint8_t* data, size_t len, const Writer& write) {
    while (len > 0) {
      const size_t space = capacity_ - bufferPos_;
      const size_t toCopy = (len < space) ? len : space;
      // buffer_ is guaranteed sized by ensureCapacity(); guard defensively.
      if (buffer_.size() != capacity_) return false;
      std::memcpy(buffer_.data() + bufferPos_, data, toCopy);
      bufferPos_ += toCopy;
      data += toCopy;
      len -= toCopy;
      if (bufferPos_ >= capacity_) {
        if (!flush(write)) return false;
      }
    }
    return true;
  }

  // Flush whatever remains buffered. No-op (success) when empty. On a short
  // write the position is still cleared so a follow-up flush won't re-emit.
  bool flush(const Writer& write) {
    if (bufferPos_ == 0) return true;
    const size_t written = write(buffer_.data(), bufferPos_);
    const bool ok = (written == bufferPos_);
    bufferPos_ = 0;
    return ok;
  }

  // Discard buffered bytes and release the buffer's heap (END / ABORT).
  void reset() {
    std::vector<uint8_t>().swap(buffer_);
    bufferPos_ = 0;
  }

  size_t pending() const { return bufferPos_; }
  size_t capacity() const { return capacity_; }

 private:
  std::vector<uint8_t> buffer_;
  size_t bufferPos_ = 0;
  size_t capacity_;
};

}  // namespace crosspoint::net
