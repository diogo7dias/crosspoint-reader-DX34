/**
 * @file SleepMoveSelection.h
 * @brief Memory-safe random selection of N wallpaper names to move out of /sleep.
 *
 * Backs the "move X random wallpapers to /sleep pause" action. It must pick a
 * uniformly random subset of the folder WITHOUT loading every filename into RAM
 * (the folder can hold 1000+ images, and a full vector of names is exactly the
 * kind of allocation that strains the device heap).
 *
 * reservoirSampleNames streams the folder once and keeps only the chosen N names
 * via Algorithm R reservoir sampling — O(N) memory, independent of folder size.
 * Pure and host-testable: production feeds a walkSleepBmps-backed source and the
 * device random(); tests feed a vector and a deterministic randomFn.
 */
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace crosspoint {
namespace sleep {

// Yields the next candidate name into `out`; returns false at end of stream.
using NameSource = std::function<bool(std::string& out)>;

// Returns a value in [0, n). Production wires ::random(n); tests inject a stub.
using RandomFn = std::function<long(long n)>;

// Incremental Algorithm R reservoir. Feed every candidate via offer() (works for
// both a pull loop and a push-style folder walk callback), then read take().
// Holds at most `count` names regardless of how many are offered.
class Reservoir {
 public:
  Reservoir(size_t count, RandomFn randomFn) : count_(count), randomFn_(std::move(randomFn)) {
    if (count_ > 0) reservoir_.reserve(count_);
  }

  void offer(const std::string& name) {
    if (count_ == 0) {
      ++seen_;
      return;
    }
    if (seen_ < count_) {
      // Fill phase: the first `count` items go straight in.
      reservoir_.push_back(name);
    } else {
      // Replacement phase: item at index `seen` (0-based) takes a slot with
      // probability count/(seen+1). j in [0, seen]; evict if it lands in window.
      const long j = randomFn_(static_cast<long>(seen_) + 1);
      if (j >= 0 && static_cast<size_t>(j) < count_) {
        reservoir_[static_cast<size_t>(j)] = name;
      }
    }
    ++seen_;
  }

  const std::vector<std::string>& take() const { return reservoir_; }

 private:
  size_t count_;
  RandomFn randomFn_;
  std::vector<std::string> reservoir_;
  size_t seen_ = 0;  // number of items offered so far
};

// Pick up to `count` names uniformly at random from `nextName` using Algorithm R
// reservoir sampling. Returns min(count, total) names. O(count) memory: only the
// reservoir is held, never the full stream. The returned order is the reservoir
// order, not the input order (irrelevant for a move operation).
inline std::vector<std::string> reservoirSampleNames(const NameSource& nextName, size_t count,
                                                     const RandomFn& randomFn) {
  Reservoir r(count, randomFn);
  std::string name;
  while (nextName(name)) r.offer(name);
  return r.take();
}

}  // namespace sleep
}  // namespace crosspoint
