// LayoutArena — a bounded, pre-allocated scratch region for EPUB layout
// (RFC #164). It replaces the unbounded per-paragraph allocation churn
// (`words[]`/`wordStyles[]`/`wordContinues[]` plus ~7 transient vectors and
// their mid-vector inserts) with a single contiguous block carved two ways:
//
//   - alloc<T>(n)  bump-allocates POD scratch (widths, DP arrays, ...) from the
//                  FRONT, growing up.
//   - intern(...)  packs word bytes into a string region from the BACK, growing
//                  down, returning a small {offset,len} handle instead of a
//                  per-word std::string.
//
// They meet in the middle; when they would collide the arena returns
// nullptr / an invalid handle rather than growing or throwing — so a dense
// paragraph degrades to a clean ArenaOverflow instead of a bad_alloc abort on
// the -fno-exceptions build. mark()/rewind() give a LIFO checkpoint: mark at
// paragraph start, rewind after its lines are emitted, and the next paragraph
// reuses the same bytes. highWater() reports the peak for host assertions — the
// bounded-peak invariant the legacy ParsedText path could never assert.
//
// Pure: depends only on <cstdint>/<cstring>/<new>. Host-compilable and
// host-testable. Owns one heap block (nothrow); move-only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

namespace crosspoint {
namespace layout {

class LayoutArena {
 public:
  // Interned-string handle: a byte offset into the arena block + a length.
  // Carries no pointer, so it stays valid across moves of the arena.
  struct Str {
    uint32_t off = 0xFFFFFFFFu;  // 0xFFFFFFFF == invalid (intern failed)
    uint16_t len = 0;
    bool valid() const { return off != 0xFFFFFFFFu; }
  };

  // LIFO checkpoint of both bump cursors.
  struct Mark {
    size_t bump = 0;
    uint32_t textTop = 0;
  };

  LayoutArena() noexcept = default;  // empty, ok()==false
  ~LayoutArena() { delete[] block_; }

  LayoutArena(LayoutArena&& o) noexcept { moveFrom(o); }
  LayoutArena& operator=(LayoutArena&& o) noexcept {
    if (this != &o) {
      delete[] block_;
      moveFrom(o);
    }
    return *this;
  }
  LayoutArena(const LayoutArena&) = delete;
  LayoutArena& operator=(const LayoutArena&) = delete;

  // Allocate `bytes` of scratch. nothrow: ok()==false if the block could not be
  // reserved (the caller then retries at a smaller size or bails).
  static LayoutArena create(size_t bytes) noexcept {
    LayoutArena a;
    if (bytes == 0) return a;
    a.block_ = new (std::nothrow) uint8_t[bytes];
    if (a.block_ != nullptr) {
      a.size_ = bytes;
      a.textTop_ = bytes;
    }
    return a;
  }

  bool ok() const { return block_ != nullptr; }
  size_t capacity() const { return size_; }
  size_t highWater() const { return highWater_; }

  // Bump-allocate n objects of T from the front, aligned for T. Returns nullptr
  // when full or on a degenerate request — never grows, never throws.
  template <typename T>
  T* alloc(size_t n) noexcept {
    if (block_ == nullptr || n == 0) return nullptr;
    if (n > (SIZE_MAX / sizeof(T))) return nullptr;  // n*sizeof(T) would overflow
    const size_t align = alignof(T);
    const size_t start = (bump_ + (align - 1)) & ~(align - 1);
    const size_t need = n * sizeof(T);
    if (start > textTop_ || need > textTop_ - start) return nullptr;  // collides with string region
    bump_ = start + need;
    trackHighWater();
    return reinterpret_cast<T*>(block_ + start);
  }

  // Pack `len` bytes into the back string region (NUL-terminated for caller
  // convenience). Returns an invalid Str when full or len exceeds uint16.
  Str intern(const char* bytes, size_t len) noexcept {
    Str h;
    if (block_ == nullptr || len > 0xFFFFu) return h;
    const size_t need = len + 1;  // + NUL
    if (need > textTop_ || textTop_ - need < bump_) return h;  // collides with alloc region
    textTop_ -= need;
    if (len > 0 && bytes != nullptr) std::memcpy(block_ + textTop_, bytes, len);
    block_[textTop_ + len] = '\0';
    h.off = static_cast<uint32_t>(textTop_);
    h.len = static_cast<uint16_t>(len);
    trackHighWater();
    return h;
  }

  // Resolve a handle to its (NUL-terminated) bytes. Empty string for invalid.
  const char* str(Str h) const {
    if (block_ == nullptr || !h.valid() || h.off >= size_) return "";
    return reinterpret_cast<const char*>(block_ + h.off);
  }

  Mark mark() const { return Mark{bump_, static_cast<uint32_t>(textTop_)}; }

  // Restore both cursors. highWater() is a peak and is intentionally NOT
  // lowered — it records the worst case across the arena's whole life.
  void rewind(Mark m) {
    bump_ = m.bump;
    textTop_ = m.textTop;
  }

 private:
  void moveFrom(LayoutArena& o) noexcept {
    block_ = o.block_;
    size_ = o.size_;
    bump_ = o.bump_;
    textTop_ = o.textTop_;
    highWater_ = o.highWater_;
    o.block_ = nullptr;
    o.size_ = 0;
    o.bump_ = 0;
    o.textTop_ = 0;
    o.highWater_ = 0;
  }

  void trackHighWater() {
    const size_t used = bump_ + (size_ - textTop_);
    if (used > highWater_) highWater_ = used;
  }

  uint8_t* block_ = nullptr;
  size_t size_ = 0;
  size_t bump_ = 0;       // front cursor (alloc), grows up
  size_t textTop_ = 0;    // back cursor (intern), grows down
  size_t highWater_ = 0;  // peak (bump_ + bytes interned), never lowered
};

}  // namespace layout
}  // namespace crosspoint
