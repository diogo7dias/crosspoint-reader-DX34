// SimHeap implementation + global operator new/delete replacement.
//
// Exactly one translation unit per test binary may define the replaceable
// global operator new/delete — this is it.
#include "SimHeap.h"

#include <cstdlib>
#include <cstring>
#include <new>

namespace crosspoint {
namespace test {

// Every allocation we hand out is prefixed with this header so operator delete
// can recover the size and whether it counted against the model. kHeaderBytes
// is generously aligned (>= alignof(max_align_t) on common hosts) so user
// pointers stay correctly aligned.
namespace {
constexpr size_t kHeaderBytes = 16;
constexpr uint32_t kMagic = 0x5148'4D50u;  // "SHMP"

struct Header {
  uint32_t magic;
  uint32_t counted;  // 1 if this alloc decremented-able from liveBytes_
  size_t size;
};
static_assert(sizeof(Header) <= kHeaderBytes, "Header must fit in kHeaderBytes");

// Guards against re-entrancy if model bookkeeping ever allocates (it does not
// today, but keep the path robust).
thread_local bool inAlloc_ = false;
}  // namespace

void SimHeap::arm(size_t cap, size_t totalBudget) {
  armed_ = true;
  cap_ = cap;
  totalBudget_ = totalBudget;
  liveBytes_ = 0;
  peakLive_ = 0;
  attempts_ = 0;
  fragFails_ = 0;
  exhaustFails_ = 0;
  wouldAbortThrows_ = 0;
  nothrowNulls_ = 0;
}

void SimHeap::disarm() { armed_ = false; }
bool SimHeap::armed() { return armed_; }
void SimHeap::setCap(size_t cap) { cap_ = cap; }
size_t SimHeap::cap() { return cap_; }

void SimHeap::reset() {
  armed_ = false;
  cap_ = 0;
  totalBudget_ = 0;
  liveBytes_ = 0;
  peakLive_ = 0;
  attempts_ = 0;
  fragFails_ = 0;
  exhaustFails_ = 0;
  wouldAbortThrows_ = 0;
  nothrowNulls_ = 0;
}

unsigned SimHeap::attempts() { return attempts_; }
unsigned SimHeap::fragFails() { return fragFails_; }
unsigned SimHeap::exhaustFails() { return exhaustFails_; }
unsigned SimHeap::wouldAbortThrows() { return wouldAbortThrows_; }
unsigned SimHeap::nothrowNulls() { return nothrowNulls_; }
size_t SimHeap::liveBytes() { return liveBytes_; }
size_t SimHeap::peakLiveBytes() { return peakLive_; }

void* SimHeap::allocate(size_t size, bool isNothrow, bool* outShouldThrow) {
  *outShouldThrow = false;
  const bool model = armed_ && !inAlloc_;

  if (model) {
    inAlloc_ = true;
    ++attempts_;
    bool fail = false;
    if (size > cap_) {
      ++fragFails_;
      fail = true;
    } else if (liveBytes_ + size > totalBudget_) {
      ++exhaustFails_;
      fail = true;
    }
    if (fail) {
      if (isNothrow) {
        ++nothrowNulls_;
      } else {
        ++wouldAbortThrows_;
        *outShouldThrow = true;
      }
      inAlloc_ = false;
      return nullptr;
    }
    inAlloc_ = false;
  }

  void* base = std::malloc(kHeaderBytes + size);
  if (!base) {
    // Genuine host OOM (not the model). Honour the standard contract.
    if (!isNothrow) *outShouldThrow = true;
    return nullptr;
  }
  auto* h = static_cast<Header*>(base);
  h->magic = kMagic;
  h->size = size;
  h->counted = model ? 1u : 0u;
  if (model) {
    liveBytes_ += size;
    if (liveBytes_ > peakLive_) peakLive_ = liveBytes_;
  }
  return static_cast<char*>(base) + kHeaderBytes;
}

void SimHeap::deallocate(void* userPtr) {
  if (!userPtr) return;
  void* base = static_cast<char*>(userPtr) - kHeaderBytes;
  auto* h = static_cast<Header*>(base);
  if (h->magic == kMagic) {
    if (h->counted && liveBytes_ >= h->size) liveBytes_ -= h->size;
    std::free(base);
  } else {
    // Not one of ours (shouldn't happen — operator new is the only producer).
    std::free(userPtr);
  }
}

}  // namespace test
}  // namespace crosspoint

// ---- Global operator new / delete replacement ----------------------------
using crosspoint::test::SimHeap;

void* operator new(std::size_t size) {
  bool shouldThrow = false;
  void* p = SimHeap::allocate(size, /*isNothrow=*/false, &shouldThrow);
  if (!p && shouldThrow) throw std::bad_alloc();
  return p;
}
void* operator new[](std::size_t size) {
  bool shouldThrow = false;
  void* p = SimHeap::allocate(size, /*isNothrow=*/false, &shouldThrow);
  if (!p && shouldThrow) throw std::bad_alloc();
  return p;
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  bool shouldThrow = false;
  return SimHeap::allocate(size, /*isNothrow=*/true, &shouldThrow);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  bool shouldThrow = false;
  return SimHeap::allocate(size, /*isNothrow=*/true, &shouldThrow);
}

void operator delete(void* p) noexcept { SimHeap::deallocate(p); }
void operator delete[](void* p) noexcept { SimHeap::deallocate(p); }
void operator delete(void* p, std::size_t) noexcept { SimHeap::deallocate(p); }
void operator delete[](void* p, std::size_t) noexcept { SimHeap::deallocate(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { SimHeap::deallocate(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { SimHeap::deallocate(p); }
