// MemoryPolicy — the single owner of heap-pressure decisions (RFC #163).
//
// Today the question "is it safe to allocate X, and what do I drop if not?"
// is re-answered in seven subsystems with nine hard-wired thresholds (two of
// them duplicate) and a recovery ladder that only runs on hardware. This
// module consolidates the gates and makes the recovery-ladder DECISION a pure
// function, so the fragmentation logic that used to be flash-and-pray becomes
// host-unit-testable.
//
// Step 1 (this file) is purely additive: the pure core + host-test seam. No
// call sites are migrated yet, so behaviour is unchanged. The thresholds below
// are copied verbatim from the current sites — this is consolidation, not a
// retune.
//
// Built on the existing HeapGuard probe (and its UNIT_TEST_HOST override) so
// the largest-free-block source and host seam stay shared and uniform.
#pragma once

#include <cstddef>
#include <cstdint>

#include "HeapGuard.h"

namespace crosspoint {
namespace mem {

// ── Total-free probe (sibling to HeapGuard's largest-free-block probe) ──────
// Most gates reason about the largest CONTIGUOUS block (the currency of a
// non-moving heap); a couple genuinely want TOTAL free (many small allocations
// summing up — the rich-sleep render). Keep both behind the same host seam.
#ifdef UNIT_TEST_HOST
inline size_t& totalFreeOverride_() {
  static size_t v = SIZE_MAX;
  return v;
}
inline void setTotalFreeOverride(size_t bytes) { totalFreeOverride_() = bytes; }
inline void clearTotalFreeOverride() { totalFreeOverride_() = SIZE_MAX; }
inline size_t totalFreeBytes() { return totalFreeOverride_(); }
#else
inline size_t totalFreeBytes() { return heap_caps_get_free_size(MALLOC_CAP_8BIT); }
#endif

// ── Named gates ─────────────────────────────────────────────────────────────
// Each value names a CONCRETE operation about to be attempted. The metric
// (largest-block vs total-free) and the threshold are hidden here so callers
// ask a purpose, not a magic number. The two former duplicate 64 KB sleep
// constants collapse into the single ScanSleepPlaylist row.
enum class Op : uint8_t {
  SpawnRenderTask,        // xTaskCreate 8 KB stack          (largest >= 12 KB)
  BuildSectionLayout,     // CSS index + expat + LUT + words (largest >= 48 KB)
  RebuildSectionFloor,    // hard floor: below = no point    (largest >= 20 KB)
  PrefetchNeighborPages,  // SectionPageCache prev+next      (largest >= 30 KB)
  PrefetchNextPage,       // SectionPageCache forward only   (largest >= 18 KB)
  RenderRichSleepScreen,  // wallpaper/cover bitmap parse    (TOTAL  >= 30 KB)
  // Sleep V2 playlist reconcile/trim peak. listSleepBmpsWithMtime builds an
  // ~18 KB vector, then trimToCap holds ~4× that (favs + nonFav + surviving)
  // before std::move collapses them; surviving.reserve() threw bad_alloc with
  // the residual largest block at ~47 KB. 64 KB clears the trim peak. Below it,
  // callers bypass the playlist and stream-pick a file by directory index.
  ScanSleepPlaylist,      // playlist trim peak              (largest >= 64 KB)
};

namespace detail {
struct Gate {
  bool totalMetric;  // false = largest-block, true = total-free
  size_t threshold;
};
inline Gate gateFor(Op op) {
  switch (op) {
    case Op::SpawnRenderTask:       return {false, 12 * 1024};
    // 48 KB clears the section-load working peak (CSS index + expat read
    // buffer + page LUT + ParsedText word/style vectors) with margin and is
    // what the pre-flight defrag pass targets. PR #95/#100 tried higher
    // rebuild floors (36 KB / 28 KB) but devices with persistent boot-state
    // fragmentation (PR #96 hardware capture) sit at largest ~25 KB
    // indefinitely, turning a would-pass layout into a permanent
    // recovery-screen dead-end; the gate stays at the section-peak value and
    // the rebuild *floor* below absorbs the fragmented-device case.
    case Op::BuildSectionLayout:    return {false, 48 * 1024};
    // Hard floor for attempting a section rebuild: below this even a
    // post-defrag malloc is essentially guaranteed to fail. Reverted to PR
    // #96's 20 KB after #95/#100's higher floors bounced every modest book on
    // this device — the retry-once + auto-revert path plus the OOM recovery
    // screen handle a real mid-layout failure cleanly, so the floor optimizes
    // for "try and let the failure path catch it" over "block proactively".
    case Op::RebuildSectionFloor:   return {false, 20 * 1024};
    case Op::PrefetchNeighborPages: return {false, 30 * 1024};
    // Forward-only single-page prefetch (RFC reading-speed Stage 1a): one
    // deserialized Page's working set is ~half the prev+next pair, so a lower
    // floor keeps the *next* page warm under moderate fragmentation (the common
    // forward-reading case) where the 30 KB neighbour gate would close and force
    // a synchronous on-turn SD load (the intermittent ~1 s stalls).
    case Op::PrefetchNextPage:      return {false, 18 * 1024};
    case Op::RenderRichSleepScreen: return {true,  30 * 1024};
    case Op::ScanSleepPlaylist:     return {false, 64 * 1024};
  }
  return {false, SIZE_MAX};  // unreachable; conservative (gate stays closed)
}
}  // namespace detail

// True if the heap currently satisfies the named operation's gate.
inline bool canAfford(Op op) {
  const detail::Gate g = detail::gateFor(op);
  const size_t have = g.totalMetric ? totalFreeBytes() : crosspoint::heap::largestFreeBlockBytes();
  return have >= g.threshold;
}

// The byte threshold behind a named gate. For diagnostics/logging only — a
// caller should ask canAfford(op) for the decision, never re-derive it from
// this value. Keeps the "one source of truth" guarantee while letting the
// reader's pre-flight log echo the figure it gated on.
inline size_t thresholdFor(Op op) { return detail::gateFor(op).threshold; }

// ── Recovery ladder (pure decision) ─────────────────────────────────────────
// The reader's pre-flight escalation, lifted out of EpubReaderActivity as a
// pure state transition so it can be exercised on the host. The caller runs
// the impure action for each rung (anchor reset / releaseMaxResources /
// silentRestartToReader), re-probes, flips the context flags, and asks again.
inline constexpr size_t kLayoutHardFloorBytes = 20 * 1024;

enum class Recovery : uint8_t {
  ReleaseAnchor,        // drop the layout heap anchor, re-probe
  ReleaseMaxResources,  // drop CSS index + page cache + font cache + status caches
  SilentRestart,        // reboot to clear non-moving fragmentation
  GiveUp,               // budget/floor exhausted -> caller routes to library
  Proceed,              // above the hard floor: attempt layout optimistically
};

struct RecoveryContext {
  bool anchorHeld = false;
  bool maxAlreadyDropped = false;
  bool bookOpen = false;        // silent-restart-to-reader needs an open book
  uint8_t restartBudget = 0;    // remaining auto-silent-restarts (0..2)
  size_t largestAfterStep = 0;  // re-probed largest after the previous rung
};

// Precondition: called only while the layout gate is still closed (the caller
// re-checks canAfford(BuildSectionLayout) between rungs and stops on success).
inline Recovery nextRecoveryStep(const RecoveryContext& c) {
  if (c.anchorHeld) return Recovery::ReleaseAnchor;
  if (!c.maxAlreadyDropped) return Recovery::ReleaseMaxResources;
  if (c.largestAfterStep < kLayoutHardFloorBytes) {
    return (c.bookOpen && c.restartBudget > 0) ? Recovery::SilentRestart : Recovery::GiveUp;
  }
  return Recovery::Proceed;
}

// ── Layout/render degradation thresholds (RFC #164 step 7, Tier A) ──────────
// Largest-free-block bytes below which the layout/render pipeline sheds work
// instead of OOM->restart. Owned here alongside the gates so the heap numbers
// move together; the largest->DegradeLevel mapping itself is the pure
// crosspoint::layout::layoutLevelFor/renderLevelFor (so MemoryPolicy stays
// decoupled from the layout vocabulary). Bands:
//   layout : >=48K Full | 28K..48K NoHyphen (drop hyphenation) | <28K SkipImages
//   render : >=40K Full | <40K TrimPrewarm (warm regular glyphs only)
// 48K mirrors the BuildSectionLayout gate: anything proceeding *below* the
// comfortable layout gate (only reachable via the recovery ladder) degrades.
// Initial values — retune on device.
inline constexpr size_t kLayoutNoHyphenBelowBytes = 48 * 1024;
inline constexpr size_t kLayoutSkipImagesBelowBytes = 28 * 1024;
inline constexpr size_t kRenderTrimPrewarmBelowBytes = 40 * 1024;

// ── Shed-evictor registry + dynamic probe-before-grow ───────────────────────
// SafeAnywhere evictors only: alloc-free, lock-free, callable from the ESP-IDF
// failed-alloc callback (today: FontCacheManager::clearCache). Plain function
// pointers — no std::function (no heap, callback-safe). Fixed capacity.
using ShedFn = void (*)();
inline constexpr size_t kMaxShedEvictors = 8;

namespace detail {
struct ShedRegistry {
  ShedFn fns[kMaxShedEvictors];
  size_t count;
};
inline ShedRegistry& shedRegistry() {
  static ShedRegistry r{};
  return r;
}
}  // namespace detail

inline void registerShedEvictor(ShedFn fn) {
  detail::ShedRegistry& r = detail::shedRegistry();
  if (fn != nullptr && r.count < kMaxShedEvictors) {
    r.fns[r.count++] = fn;
  }
}

inline void clearShedEvictors() { detail::shedRegistry().count = 0; }

// Run every registered evictor once. Alloc-free, lock-free.
inline void shedUnderPressure() noexcept {
  detail::ShedRegistry& r = detail::shedRegistry();
  for (size_t i = 0; i < r.count; ++i) {
    if (r.fns[i] != nullptr) r.fns[i]();
  }
}

// The dominant -fno-exceptions idiom: probe before a vector/string grow. If
// the contiguous block is short, shed once and re-probe. Absorbs the
// hand-rolled evict-and-retry loops.
inline bool roomToGrow(size_t needBytes, size_t headroom = crosspoint::heap::kDefaultHeadroomBytes) {
  if (crosspoint::heap::canAllocateContiguous(needBytes, headroom)) return true;
  shedUnderPressure();
  return crosspoint::heap::canAllocateContiguous(needBytes, headroom);
}

}  // namespace mem
}  // namespace crosspoint
