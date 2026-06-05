#pragma once

#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

// Nothrow versions of std::make_unique. Return nullptr on allocation failure
// instead of calling abort() (the default when exceptions are disabled on ESP32
// with -fno-exceptions, which is how this firmware is built).
//
// Single object:
//   auto obj = makeUniqueNoThrow<PNG>();
//   if (!obj) { LOG_ERR("TAG", "OOM"); return false; }
//
// Array:
//   auto buf = makeUniqueNoThrow<uint8_t[]>(size);
//   if (!buf) { LOG_ERR("TAG", "OOM"); return false; }
//   buf[0] = 0xFF;
//   someApi(buf.get(), size);
//
// Ported from upstream crosspoint-reader 8377ac9e (PR #1832). DX34's
// memory-hardening branch landed the same nothrow + null-check pattern by
// hand across the large-allocation sites; this helper is the canonical place
// to migrate them to in a future cleanup pass, and the preferred form for
// any new allocation site.

// SFINAE (not a C++20 `requires` clause) so this header compiles on the
// framework's GCC 8 toolchain, which only partially supports C++20 concepts.
template <typename T, typename... Args,
          typename std::enable_if<!std::is_array<T>::value, int>::type = 0>
std::unique_ptr<T> makeUniqueNoThrow(Args&&... args) {
  return std::unique_ptr<T>(new (std::nothrow) T(std::forward<Args>(args)...));
}

template <typename T, typename std::enable_if<std::is_array<T>::value && std::extent<T>::value == 0, int>::type = 0>
std::unique_ptr<T> makeUniqueNoThrow(size_t count) {
  using Elem = typename std::remove_extent<T>::type;
  return std::unique_ptr<T>(new (std::nothrow) Elem[count]());
}

// ── C-interop allocation seam ──────────────────────────────────────────────
// makeUniqueNoThrow covers C++ objects and arrays (the preferred form). A few
// sites must hand a raw buffer to a C decoder library (miniz, picojpeg,
// lodepng) that takes ownership, reallocs, or frees with C free(). For those —
// and ONLY those — this namespace is the single sanctioned place that calls
// std::malloc/std::realloc. Everywhere else in our code, raw malloc/calloc/
// realloc and bare `new` are banned by scripts/check_alloc_seam.py; route
// through makeUniqueNoThrow or these helpers instead.
//
// All three already return nullptr on failure (no exceptions to disable), so
// callers just null-check. Prefer CMallocPtr so the free() is automatic; reach
// for tryMalloc/tryRealloc raw only when a C API owns or reallocs the buffer.
namespace crosspoint {
namespace mem {

[[nodiscard]] inline void* tryMalloc(size_t bytes) { return std::malloc(bytes); }  // alloc-ok

[[nodiscard]] inline void* tryCalloc(size_t count, size_t size) {
  return std::calloc(count, size);  // alloc-ok
}

[[nodiscard]] inline void* tryRealloc(void* ptr, size_t bytes) {
  return std::realloc(ptr, bytes);  // alloc-ok
}

// RAII owner for a C-style malloc'd buffer: frees with std::free on scope exit.
//   auto buf = crosspoint::mem::CMallocPtr<uint8_t>(
//       static_cast<uint8_t*>(crosspoint::mem::tryMalloc(n)));
//   if (!buf) { LOG_ERR("TAG", "OOM"); return false; }
//   cDecoder(buf.get(), n);   // freed automatically
struct CFree final {
  void operator()(void* p) const { std::free(p); }
};
template <typename T>
using CMallocPtr = std::unique_ptr<T, CFree>;

}  // namespace mem
}  // namespace crosspoint

// Helper struct to call a cleanup function on exit from any scope.
// Use with a lambda to avoid unnecessary allocations from std::function/std::bind:
// Example:
//   auto jpeg = makeUniqueNoThrow<JPEGDEC>();
//   ScopedCleanup cleanup{[&jpeg]{ jpeg->close(); }};
//
template <typename F>
struct [[nodiscard]] ScopedCleanup final {
  const F fn;
  explicit ScopedCleanup(F f) : fn{std::move(f)} {}
  ScopedCleanup(const ScopedCleanup&) = delete;
  ScopedCleanup& operator=(const ScopedCleanup&) = delete;
  ScopedCleanup(ScopedCleanup&&) = delete;
  ScopedCleanup& operator=(ScopedCleanup&&) = delete;
  ~ScopedCleanup() { fn(); }
};

template <typename F>
ScopedCleanup(F) -> ScopedCleanup<F>;
