/**
 * @file PersistentStore.h
 * @brief Template store: owns a schema T, its atomic I/O, and debounce/coalesce.
 *
 * Design (RFC #20 Path A — code-only, zero on-disk schema change):
 *   - get()/set()/mutate()/touch()/unsafeMut() — caller API
 *   - flushNow()/flushSoon()/tickPersist(nowMs) — write machinery
 *   - load()/rollback() — read machinery with LoadReport
 *
 * Test seams:
 *   - IFileIO& injected at construction (prod: SdFatFileIO, test: InMemoryFileIO)
 *   - Serializer is a free function pair (toJson/fromJson) passed as fn pointers
 *   - Clock is pluggable via tickPersist(nowMs) parameter
 */
#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include "IFileIO.h"

namespace crosspoint {
namespace persist {

struct LoadReport {
  enum Status {
    kOk,                // real file parsed cleanly
    kMissing,           // no real, no .bak — defaults used
    kCorrupt,           // real corrupt AND .bak corrupt/missing — defaults used
    kRecoveredFromBak,  // real corrupt or missing, .bak parsed
  };
  Status status = kMissing;
  const char* detail = "";  // static string, no heap churn
  const char* name = "";    // store name for logging
};

template <typename T>
class PersistentStore {
 public:
  using Serialize = std::string (*)(const T&);
  using Deserialize = bool (*)(const std::string&, T&);
  // Stream serializer: writes payload bytes directly to a sink without
  // building a full intermediate std::string. Used for schemas that can
  // grow large (e.g. APP_STATE with sleep playlist) where the std::string
  // peak would trip `new` under tight heap.
  using StreamSerialize = void (*)(const T&, JsonSink&);

  PersistentStore(const char* name, const char* path, IFileIO& io, Serialize ser, Deserialize deser)
      : name_(name), path_(path), io_(io), ser_(ser), deser_(deser) {}

  // Attach a stream serializer. When set, flushNow uses streamed write path
  // (no std::string intermediate). `ser` remains required (used by any
  // legacy callers / fallback); pass nullptr to disable the string path
  // explicitly — flushNow will only stream.
  void setStreamSerializer(StreamSerialize ss) { stream_ser_ = ss; }

  // --- Read API ---
  const T& get() const { return data_; }
  T& unsafeMut() { return data_; }  // legacy hot-loop sites

  // --- Write API ---
  template <typename M, typename V>
  void set(M T::* field, V&& v) {
    data_.*field = std::forward<V>(v);
    markDirty_();
  }

  template <typename F>
  void mutate(F&& fn) {
    fn(data_);
    markDirty_();
  }

  void touch() { markDirty_(); }

  // --- Flush ---
  // Force synchronous write. Returns true on success. Clears dirty on success.
  // Uses streamed write when a stream serializer is attached (peak-heap
  // safe for large payloads) — otherwise falls back to build-string-then-write.
  bool flushNow() {
    bool ok = false;
    if (stream_ser_) {
      ok = io_.safeWriteStreamed(path_, [this](JsonSink& sink) {
        stream_ser_(data_, sink);
        return true;
      });
    } else {
      const std::string payload = ser_(data_);
      ok = io_.safeWrite(path_, payload);
    }
    if (ok) {
      dirty_ = false;
      flushCount_++;
    }
    return ok;
  }

  // Mark dirty + schedule debounced flush. Actual write happens in tickPersist.
  void flushSoon() { markDirty_(); }

  // Call every main-loop tick with current ms. Flushes if dirty AND
  // debounce window elapsed. Returns true if a flush was performed.
  bool tickPersist(uint32_t nowMs) {
    lastTickMs_ = nowMs;
    if (!dirty_) return false;
    if ((nowMs - dirtyAtMs_) < debounceMs_) return false;
    return flushNow();
  }

  bool isDirty() const { return dirty_; }
  size_t flushCount() const { return flushCount_; }
  const char* name() const { return name_; }
  const char* path() const { return path_; }

  void setDebounce(uint32_t ms) { debounceMs_ = ms; }

  // --- Load ---
  // Read + deserialize from disk. Real → .bak fallback via IFileIO::safeRead.
  // LoadReport names which tier the value came from.
  LoadReport load() {
    LoadReport r{LoadReport::kMissing, "no file; defaults used", name_};
    const std::string content = io_.safeRead(path_);
    if (content.empty()) {
      // Nothing to parse; caller keeps default-constructed T.
      return r;
    }
    T parsed;
    if (deser_(content, parsed)) {
      data_ = parsed;
      r.status = LoadReport::kOk;
      r.detail = "ok";
      return r;
    }
    // Real file present but unparseable. safeRead already tries .bak/.tmp —
    // if we got content that didn't parse, it's corrupt; defaults remain.
    r.status = LoadReport::kCorrupt;
    r.detail = "corrupt; defaults used";
    return r;
  }

  // Explicit .bak rollback (overwrites current data with .bak contents).
  // Used by diagnostic paths / manual recovery. Returns true if .bak parsed.
  bool rollback() {
    const std::string bak = std::string(path_) + ".bak";
    const std::string content = io_.safeRead(bak);
    if (content.empty()) return false;
    T parsed;
    if (!deser_(content, parsed)) return false;
    data_ = parsed;
    return true;
  }

 private:
  void markDirty_() {
    dirty_ = true;
    dirtyAtMs_ = lastTickMs_;  // debounce window starts from last tick observation
  }

  const char* name_;
  const char* path_;
  IFileIO& io_;
  Serialize ser_;
  Deserialize deser_;
  StreamSerialize stream_ser_ = nullptr;
  T data_{};
  bool dirty_ = false;
  uint32_t dirtyAtMs_ = 0;
  uint32_t debounceMs_ = 1500;
  uint32_t lastTickMs_ = 0;  // captured so markDirty_ anchors to a known clock
  size_t flushCount_ = 0;

  // Expose a write-friendly path for external clock injection during tests.
  template <typename U>
  friend void setStoreClockForTest(PersistentStore<U>&, uint32_t);
};

// Test helper — lets fixtures advance the debounce-anchor clock without
// calling tickPersist (which would also flush).
template <typename T>
inline void setStoreClockForTest(PersistentStore<T>& s, uint32_t nowMs) {
  s.lastTickMs_ = nowMs;
}

}  // namespace persist
}  // namespace crosspoint
