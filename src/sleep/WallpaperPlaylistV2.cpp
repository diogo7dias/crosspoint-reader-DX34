#include "WallpaperPlaylistV2.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace crosspoint {
namespace sleep {
namespace v2 {

namespace {

constexpr const char* kSleepDir = "/sleep";
constexpr const char* kSleepPauseDir = "/sleep pause";

// Headroom over the requested allocation when probing the heap. Covers the
// transient std::string growth peak (capacity often rounded up to the next
// power-of-two on libstdc++) plus small ancillary allocs the caller does
// while the buffer is in flight. Build with -fno-exceptions, so a bad_alloc
// here would abort — we must probe before allocating big buffers.
constexpr size_t kAllocProbeHeadroomBytes = 4 * 1024;

// Defense-in-depth cap on how many NEW files reconcile() materializes in one
// pass. Each new entry is a heap std::string, and an empty/stale order buffer
// makes every /sleep file "new" — an unbounded batch bad_alloc-aborts under
// -fno-exceptions on the low sleep-entry heap (the "can't lock" crash). Excess
// new files are simply absorbed by a later reconcile (a subsequent sleep wake).
constexpr uint16_t kMaxNewFilesPerReconcile = 64;

std::string makeSleepPath(const std::string& filename) {
  if (filename.empty()) return {};
  return std::string(kSleepDir) + "/" + filename;
}

std::string makePausePath(const std::string& filename) {
  if (filename.empty()) return {};
  return std::string(kSleepPauseDir) + "/" + filename;
}

// Parse the "v1 cursor=N\n" header in place. On success, `namesStart` is the
// byte offset where the names region begins (one past the header newline) and
// `cursor` holds the parsed value. The caller strips the header by erasing
// [0, namesStart) and moving the blob straight into buffer_ — no second
// full-size copy of the names region (the old substr path doubled peak heap).
bool parseOrderHeader(const std::string& blob, size_t& namesStart, size_t& cursor) {
  cursor = 0;
  namesStart = 0;
  if (blob.size() < 3 || blob.compare(0, 3, "v1 ") != 0) return false;
  const auto firstNewline = blob.find('\n');
  if (firstNewline == std::string::npos) return false;
  const auto eq = blob.find("cursor=");
  if (eq == std::string::npos || eq >= firstNewline) return false;
  cursor = static_cast<size_t>(std::strtoul(blob.c_str() + eq + 7, nullptr, 10));
  namesStart = firstNewline + 1;
  return true;
}

}  // namespace

WallpaperPlaylistV2& WallpaperPlaylistV2::instance() {
  static WallpaperPlaylistV2 inst;
  return inst;
}

void WallpaperPlaylistV2::setDeps(const Deps& d) {
  deps_ = d;
  loaded_ = false;
  dirty_ = true;
}

void WallpaperPlaylistV2::resetForTest() {
  deps_ = Deps{};
  buffer_.clear();
  cursor_ = 0;
  dirty_ = true;
  loaded_ = false;
  pendingNotice_ = Notice{};
}

WallpaperPlaylistV2::Notice WallpaperPlaylistV2::takeNotice() {
  Notice n = pendingNotice_;
  pendingNotice_ = Notice{};
  return n;
}

bool WallpaperPlaylistV2::ensureLoaded() {
  if (loaded_) return true;
  if (!deps_.fileIO) return false;
  loadFromDisk();
  loaded_ = true;
  if (cursor_ > buffer_.size()) cursor_ = 0;
  return true;
}

bool WallpaperPlaylistV2::loadFromDisk() {
  if (!deps_.fileIO) return false;
  std::string blob = deps_.fileIO->safeRead(deps_.orderFilePath);
  if (blob.empty()) {
    buffer_.clear();
    cursor_ = 0;
    return false;
  }
  size_t namesStart = 0;
  size_t cursor = 0;
  if (!parseOrderHeader(blob, namesStart, cursor)) {
    buffer_.clear();
    cursor_ = 0;
    return false;
  }
  // Strip the header in place (memmove, no allocation) and take ownership of
  // the buffer — peak heap is one blob, not blob + a substr copy.
  blob.erase(0, namesStart);
  buffer_ = std::move(blob);
  cursor_ = cursor;
  return true;
}

bool WallpaperPlaylistV2::saveToDisk() const {
  if (!deps_.fileIO) return false;
  // Stream the write so peak heap is bounded — concatenating header + buffer_
  // into a fresh full-size std::string would double transient memory pressure
  // right when reconcile already holds two buffer copies. safeWriteStreamed
  // hands us a JsonSink-like byte sink wired straight to the .tmp file; no
  // intermediate copy.
  return deps_.fileIO->safeWriteStreamed(deps_.orderFilePath, [this](crosspoint::persist::JsonSink& sink) {
    char header[32];
    const int hn = std::snprintf(header, sizeof(header), "v1 cursor=%zu\n", cursor_);
    if (hn <= 0) return false;
    sink.write(reinterpret_cast<const uint8_t*>(header), static_cast<size_t>(hn));
    if (!buffer_.empty()) {
      sink.write(reinterpret_cast<const uint8_t*>(buffer_.data()), buffer_.size());
    }
    return true;
  });
}

void WallpaperPlaylistV2::writeBuffer(const std::vector<std::string>& names, size_t cursor) {
  size_t total = 0;
  for (const auto& n : names) total += n.size() + 1;
  buffer_.clear();
  cursor_ = 0;
  // Build is compiled with -fno-exceptions; a bad_alloc inside reserve would
  // abort the process. Probe the heap first and leave buffer_ empty if the
  // contiguous block is too small — caller (reshuffle) treats empty buffer
  // as "playlist unavailable", SleepActivity then routes to the direct pick
  // fallback so the user still sees a wallpaper this cycle.
  if (!heapHasContiguous(total)) {
    return;
  }
  buffer_.reserve(total);
  for (const auto& n : names) {
    buffer_.append(n);
    buffer_.push_back('\n');
  }
  cursor_ = cursor;
  saveToDisk();
}

size_t WallpaperPlaylistV2::entryCountForTest() const {
  size_t n = 0;
  for (char c : buffer_) {
    if (c == '\n') ++n;
  }
  return n;
}

std::string WallpaperPlaylistV2::peekAtCursor() const {
  if (cursor_ >= buffer_.size()) return {};
  const auto end = buffer_.find('\n', cursor_);
  if (end == std::string::npos) return buffer_.substr(cursor_);
  return buffer_.substr(cursor_, end - cursor_);
}

void WallpaperPlaylistV2::advanceCursor() {
  if (cursor_ >= buffer_.size()) return;
  const auto end = buffer_.find('\n', cursor_);
  if (end == std::string::npos) {
    cursor_ = buffer_.size();
  } else {
    cursor_ = end + 1;
  }
}

uint16_t WallpaperPlaylistV2::trimToCap(std::vector<SleepBmpEntry>& entries, bool& favoritesCapBlocked) {
  favoritesCapBlocked = false;
  if (entries.size() <= kSleepFolderCap) return 0;

  // RFC #156 Bug A: the old trim partitioned `entries` into three transient
  // vectors (nonFav + favs + surviving), peaking at ~3x the entry vector — the
  // largest sleep-path allocation and the one most likely to bail on (or, if
  // ever called ungated, abort) a fragmented heap. This rewrite works in place
  // on the single `entries` vector, so peak is ~1x. Production reconcile still
  // runs under the sleep-playlist heap gate; we keep a self-safety probe (now
  // sized to that single vector) so an ungated future caller still bails rather
  // than risk a bad_alloc under -fno-exceptions. Bail = no trim this cycle; the
  // next reconcile retries once heap recovers and the over-cap files wait in
  // /sleep meanwhile.
  if (!heapHasContiguous(entries.size() * sizeof(SleepBmpEntry))) {
    return 0;
  }

  // Partition non-favorites to the front (favorites are never moved). isFavorite
  // is evaluated once per entry. std::partition is in place — no temp vector;
  // relative order is not relied upon (the favorites-saturated branch demotes
  // every non-favorite regardless of order, and the normal branch re-sorts the
  // non-favorite prefix by mtime below).
  const auto firstFav = std::partition(entries.begin(), entries.end(), [this](const SleepBmpEntry& e) {
    return !(deps_.isFavorite && deps_.isFavorite(makeSleepPath(e.name)));
  });
  const size_t nonFavCount = static_cast<size_t>(firstFav - entries.begin());
  const size_t favCount = entries.size() - nonFavCount;

  if (deps_.fs) deps_.fs->mkdir(kSleepPauseDir);

  // Favorites alone fill the cap: demote every non-favorite (e.g. a fresh
  // upload that has nowhere to go) and keep only the favorites.
  if (favCount >= kSleepFolderCap) {
    favoritesCapBlocked = true;
    uint16_t moved = 0;
    for (size_t i = 0; i < nonFavCount; ++i) {
      const std::string from = makeSleepPath(entries[i].name);
      const std::string to = makePausePath(entries[i].name);
      if (deps_.fs && deps_.fs->rename(from, to)) {
        if (deps_.onPathRenamed) deps_.onPathRenamed(from, to);
        ++moved;
      }
    }
    entries.erase(entries.begin(), entries.begin() + nonFavCount);
    return moved;
  }

  // Normal branch: demote the oldest-by-mtime non-favorites. Sort only the
  // non-favorite prefix [0, nonFavCount) ascending; favorites in the tail stay
  // put. Renames happen oldest-first, matching the previous behaviour.
  std::sort(entries.begin(), entries.begin() + nonFavCount,
            [](const SleepBmpEntry& a, const SleepBmpEntry& b) { return a.mtime < b.mtime; });
  const size_t excess = entries.size() - kSleepFolderCap;
  const size_t toMove = std::min(excess, nonFavCount);
  uint16_t moved = 0;
  for (size_t i = 0; i < toMove; ++i) {
    const std::string from = makeSleepPath(entries[i].name);
    const std::string to = makePausePath(entries[i].name);
    if (deps_.fs && deps_.fs->rename(from, to)) {
      if (deps_.onPathRenamed) deps_.onPathRenamed(from, to);
      ++moved;
    }
  }
  // Drop the moved entries; favorites + surviving non-favorites remain. Order of
  // the survivors is irrelevant — the caller re-derives newFiles from this set
  // and re-sorts before splicing.
  entries.erase(entries.begin(), entries.begin() + toMove);
  return moved;
}

bool WallpaperPlaylistV2::heapHasContiguous(size_t needBytes) const {
  if (!deps_.largestFreeBlockFn) {
    // No probe wired — caller (host test or pre-RFC-#156 production) is on
    // a heap we cannot interrogate. Assume the reserve will succeed; this
    // matches the previous #ifdef UNIT_TEST_HOST short-circuit.
    return true;
  }
  return deps_.largestFreeBlockFn() >= needBytes + kAllocProbeHeadroomBytes;
}

bool WallpaperPlaylistV2::nameIsInBuffer(const char* name, size_t len) const {
  if (buffer_.empty() || !name || len == 0) return false;
  // Walk the '\n'-delimited names in buffer_ and compare each whole line to
  // `name`. No temporary needle string — the old path allocated a "\nname\n"
  // needle on every call (~500-1000x per over-cap reconcile). Comparing whole
  // lines (bounded by the separators) means prefix collisions like "a.bmp" vs
  // "ab.bmp" can never false-match.
  const char* data = buffer_.data();
  const size_t bsize = buffer_.size();
  size_t start = 0;
  while (start < bsize) {
    size_t nl = buffer_.find('\n', start);
    if (nl == std::string::npos) nl = bsize;  // tolerate a missing final '\n'
    if (nl - start == len && std::memcmp(data + start, name, len) == 0) return true;
    start = nl + 1;
  }
  return false;
}

bool WallpaperPlaylistV2::nameIsInBuffer(const std::string& name) const {
  return nameIsInBuffer(name.data(), name.size());
}

void WallpaperPlaylistV2::reconcile() {
  if (!deps_.fs) return;
  if (!ensureLoaded()) return;
  if (!dirty_) return;

  // A reconcile is committed: the cap state recorded below is fresh, so the
  // facade may safely clear a previously-persisted favorites-cap warning if we
  // no longer trip the cap this pass.
  pendingNotice_.reconciled = true;

  // Streaming walk: only retain NEW files (those not already in buffer_).
  // Heap cost is proportional to the delta (typically 0-3 entries on a normal
  // session), not the full /sleep listing. This is the critical fix for the
  // bad_alloc that hit boot/home-route reconcile when /sleep had ~500 entries
  // (fragmented heap could not fit the transient ~14 KB vector<SleepBmpEntry>).
  std::vector<SleepBmpEntry> newFiles;
  newFiles.reserve(8);  // typical delta upper bound; grows if exceeded
  size_t diskCount = 0;

  // Zero-allocation walk: the callback receives the name straight off the SD
  // layer's stack buffer (no per-file std::string), and nameIsInBuffer scans
  // buffer_ in place. Only genuinely new files (typically 0-3) materialize a
  // string. The former per-file favorite probe here was dead (its count was
  // never read) and cost a makeSleepPath allocation + isFavorite call on every
  // one of the ~500 files — removed.
  bool newFilesTruncated = false;
  deps_.fs->walkSleepBmps([&](const char* name, size_t len, uint32_t mtime) {
    ++diskCount;
    if (newFilesTruncated || nameIsInBuffer(name, len)) return;
    // Bound this batch: cap the count and stop if the heap is tight, so a large
    // all-new /sleep folder can never grow this vector into a bad_alloc-abort on
    // the low sleep-entry heap. Truncated files are picked up by a later
    // reconcile; the wallpaper still rotates from the buffer / direct pick.
    if (newFiles.size() >= kMaxNewFilesPerReconcile ||
        !heapHasContiguous((newFiles.size() + 4) * sizeof(SleepBmpEntry))) {
      newFilesTruncated = true;
      return;
    }
    newFiles.push_back({std::string(name, len), mtime});
  });

  // Trim path. Only enters here if /sleep is over the cap — gates the heavy
  // full-listing materialization on a count-only streaming pass first.
  bool capBlocked = false;
  uint16_t moved = 0;
  // Heap-guard the trim path's full-listing alloc. listSleepBmpsWithMtime
  // materializes one contiguous vector of up to (cap+64) SleepBmpEntry — the
  // biggest allocation on the sleep path. Build is -fno-exceptions, so probe
  // for a contiguous block first; if it won't fit, skip trim this cycle (the
  // over-cap files simply wait in /sleep) and still splice any new files the
  // cheap streaming walk already found. A later wake retries once heap allows.
  if (diskCount > kSleepFolderCap &&
      heapHasContiguous((kSleepFolderCap + 64) * sizeof(SleepBmpEntry))) {
    std::vector<SleepBmpEntry> all = deps_.fs->listSleepBmpsWithMtime(kSleepFolderCap + 64);
    // If the listing itself hit its cap, files beyond it were never seen —
    // stay dirty so a later reconcile finishes the job.
    const bool listingCapHit = all.size() >= kSleepFolderCap + 64;
    moved = trimToCap(all, capBlocked);
    // Record outcome as data; the facade drains it via takeNotice() after
    // advance() and persists it for the next-wake home warning / toast.
    pendingNotice_.favoritesCapBlocked = capBlocked;
    pendingNotice_.movedToPause = moved;
    // Re-derive newFiles after trim — some new arrivals may have been pushed
    // to /sleep pause if the cap was favorites-saturated. Same 64-per-pass cap
    // as the streaming walk above: a stale/lost order file makes every
    // survivor "new" (~500 entries), and an uncapped splice of that batch is
    // exactly the bad_alloc-abort shape the cap exists to prevent. Excess is
    // absorbed by later reconciles via the kept dirty_ flag.
    newFiles.clear();
    newFilesTruncated = listingCapHit;
    for (auto& e : all) {
      if (nameIsInBuffer(e.name)) continue;
      if (newFiles.size() >= kMaxNewFilesPerReconcile ||
          !heapHasContiguous((newFiles.size() + 4) * sizeof(SleepBmpEntry))) {
        newFilesTruncated = true;
        break;
      }
      newFiles.push_back(std::move(e));
    }
  }

  if (newFiles.empty()) {
    // Truncation with an empty batch (heap too tight to retain even one
    // entry): keep dirty_ so the next sleep retries.
    dirty_ = newFilesTruncated;
    return;
  }

  // Sort new files by mtime DESCENDING (newest first) so the freshest upload
  // ends up at byte 0 of the buffer when we splice at the front.
  std::sort(newFiles.begin(), newFiles.end(),
            [](const SleepBmpEntry& a, const SleepBmpEntry& b) { return a.mtime > b.mtime; });

  // Splice insertion: build a single contiguous payload and inject at the
  // FRONT of buffer_. Cursor resets to 0 so the next advance() returns the
  // newest file. "New wallpapers on top" semantics: any fresh upload is the
  // next one shown, ahead of whatever the previous lap was on.
  std::string insertion;
  size_t insLen = 0;
  for (const auto& nf : newFiles) insLen += nf.name.size() + 1;
  // -fno-exceptions: probe before the reserve, like every other big alloc on
  // this path. Bail with dirty_ kept so the next sleep retries.
  if (!heapHasContiguous(insLen)) {
    return;
  }
  insertion.reserve(insLen);
  for (const auto& nf : newFiles) {
    insertion.append(nf.name);
    insertion.push_back('\n');
  }

  // If buffer is empty (boot first reconcile, no prior order file), build a
  // full sequential lap from disk (newest first). NOT a shuffle — shuffles
  // only happen on the user-initiated settings button.
  if (buffer_.empty()) {
    if (!rebuildSequential()) {
      return;  // heap-gated bail — dirty_ stays set, retry next sleep
    }
  } else {
    // Exact-size rebuild rather than buffer_.insert() — std::string::insert()
    // doubles capacity which on a 14 KB buffer triggers a 28 KB single
    // allocation, observed to fail on sleep-entry heap. Allocating
    // new = old + insertion exactly keeps the transient peak as two ~equal
    // allocations rather than one doubled one — fragmentation-tolerant.
    //
    // Build compiled with -fno-exceptions — a bad_alloc inside the rebuild
    // would abort. Probe the heap for the exact-size reserve (old + insertion)
    // before allocating. If it would not fit, bail with old buffer_ intact;
    // dirty_ stays true so the next advance() can retry once heap recovers,
    // and SleepActivity routes around playlist work via the direct pick
    // fallback in the meantime.
    const size_t newSize = buffer_.size() + insertion.size();
    if (!heapHasContiguous(newSize)) {
      return;
    }
    std::string newBuffer;
    newBuffer.reserve(newSize);
    newBuffer.append(insertion);
    newBuffer.append(buffer_);
    buffer_ = std::move(newBuffer);
    cursor_ = 0;
    saveToDisk();
  }

  // A truncated batch means files remain unabsorbed — stay dirty so the next
  // sleep's reconcile continues where this one stopped (pre-fix this cleared
  // unconditionally, stranding the remainder until the next upload or reboot).
  dirty_ = newFilesTruncated;
}

std::string WallpaperPlaylistV2::advance() {
  if (!deps_.fs) return {};
  if (!ensureLoaded()) return {};

  // Reconcile new files / trim overflow on sleep entry. The rich-sleep heap-
  // budget gate (30 KB free) in SleepActivity has already cleared by the time
  // advance() runs, so the listing/diff allocation is safe here. This is the
  // single trigger point for V2 reconcile in production — boot and home-route
  // reconcile are intentionally suppressed (they hit boot heap fragmentation).
  if (dirty_) reconcile();

  if (buffer_.empty()) {
    if (!rebuildSequential()) return {};
  }

  for (int skipBudget = 0; skipBudget < 16; ++skipBudget) {
    if (cursor_ >= buffer_.size()) {
      // End of lap: restart from the top in the same sequential order. NEVER
      // auto-shuffle here — only the user-initiated reshuffle() randomizes.
      cursor_ = 0;
      saveToDisk();
      if (buffer_.empty()) {
        if (!rebuildSequential()) return {};
      }
    }
    const std::string candidate = peekAtCursor();
    if (candidate.empty()) {
      if (!rebuildSequential()) return {};
      continue;
    }
    if (deps_.fs->exists(makeSleepPath(candidate))) {
      advanceCursor();
      saveToDisk();
      if (deps_.lastShownFilename && *deps_.lastShownFilename != candidate) {
        *deps_.lastShownFilename = candidate;
        if (deps_.saveAppState) deps_.saveAppState();
      }
      return candidate;
    }
    advanceCursor();
  }
  return {};
}

bool WallpaperPlaylistV2::rebuildSequential() {
  if (!deps_.fs) return false;
  // Heap-guard the rebuild's full-listing alloc, mirroring the trim path.
  // countSleepBmps is O(1) heap, so size the probe from the real file count
  // before materializing the SleepBmpEntry vector. If the contiguous block is
  // too small, bail WITHOUT touching buffer_ (leave any loaded order intact)
  // and let the caller fall back to the streaming direct pick. For small
  // libraries the probe is a few KB and passes even on a fragmented heap, so
  // the sequential playlist stays the normal path — which is exactly where the
  // early-repeat symptom was most visible.
  const size_t fileCount = deps_.fs->countSleepBmps(kSleepFolderCap);
  if (fileCount == 0) {
    buffer_.clear();
    cursor_ = 0;
    saveToDisk();
    return false;
  }
  if (!heapHasContiguous(fileCount * sizeof(SleepBmpEntry))) {
    return false;
  }
  auto entries = deps_.fs->listSleepBmpsWithMtime(kSleepFolderCap);
  if (entries.empty()) {
    buffer_.clear();
    cursor_ = 0;
    saveToDisk();
    return false;
  }
  // Newest mtime → front so "new wallpapers on top". Stable sort keeps a
  // deterministic order among ties (e.g. a batch import where mtimes match).
  std::stable_sort(entries.begin(), entries.end(),
                   [](const SleepBmpEntry& a, const SleepBmpEntry& b) { return a.mtime > b.mtime; });
  std::vector<std::string> names;
  names.reserve(entries.size());
  for (auto& e : entries) names.push_back(std::move(e.name));
  writeBuffer(names, 0);
  loaded_ = true;
  return !buffer_.empty();
}

bool WallpaperPlaylistV2::reshuffle() {
  if (!deps_.fs) return false;
  // Measured-cost gate, mirroring rebuildSequential: listSleepBmps materializes
  // up to 500 heap std::strings plus the vector spine, and writeBuffer then
  // needs the contiguous order buffer. Measure the real cost with a zero-heap
  // walk and bail BEFORE the listing if the heap can't carry it — reshuffle is
  // user-initiated so a false return just leaves the current order in place.
  size_t count = 0;
  size_t nameBytes = 0;  // == order-buffer size: sum of (filename length + 1)
  deps_.fs->walkSleepBmps([&](const char* /*name*/, size_t len, uint32_t /*mtime*/) {
    if (count >= kSleepFolderCap) return;
    ++count;
    nameBytes += len + 1;
  });
  if (count == 0) {
    buffer_.clear();
    cursor_ = 0;
    saveToDisk();
    return false;
  }
  if (!heapHasContiguous(nameBytes) || !heapHasContiguous(count * sizeof(std::string))) {
    return false;  // keep the existing order; user can retry once heap recovers
  }
  auto names = deps_.fs->listSleepBmps(kSleepFolderCap);
  if (names.empty()) {
    buffer_.clear();
    cursor_ = 0;
    saveToDisk();
    return false;
  }
  if (deps_.randomFn && names.size() > 1) {
    for (size_t i = names.size() - 1; i > 0; --i) {
      const auto j = static_cast<size_t>(deps_.randomFn(static_cast<long>(i + 1)));
      std::swap(names[i], names[j]);
    }
  }
  // Anti-repeat: after Fisher-Yates the just-shown name has a 1/N chance of
  // landing at index 0. With small libraries (4-6 favorites) that produces
  // visible back-to-back repeats almost every lap. Rotate the just-shown name
  // to position 0 and start the cursor past it so the next advance() skips
  // it. Mirrors V1 migrateToSmall (WallpaperPlaylist.cpp).
  size_t startCursor = 0;
  if (deps_.lastShownFilename && !deps_.lastShownFilename->empty() && names.size() > 1) {
    auto it = std::find(names.begin(), names.end(), *deps_.lastShownFilename);
    if (it != names.end()) {
      std::rotate(names.begin(), it, it + 1);
      startCursor = names[0].size() + 1;
    }
  }
  writeBuffer(names, startCursor);
  loaded_ = true;
  // writeBuffer can still bail on its own contiguous probe (heap shrank
  // between the gate above and the reserve); report that honestly so the
  // settings UI doesn't claim a shuffle that never happened.
  return !buffer_.empty();
}

void WallpaperPlaylistV2::rememberRendered(const std::string& fullPath, const std::string& filename) {
  if (!deps_.lastRenderedPath) return;
  bool changed = false;
  if (*deps_.lastRenderedPath != fullPath) {
    *deps_.lastRenderedPath = fullPath;
    changed = true;
  }
  if (!filename.empty() && deps_.lastShownFilename && *deps_.lastShownFilename != filename) {
    *deps_.lastShownFilename = filename;
    changed = true;
  }
  if (changed && deps_.saveAppState) deps_.saveAppState();
}

void WallpaperPlaylistV2::clearRenderedPath() {
  if (!deps_.lastRenderedPath) return;
  if (!deps_.lastRenderedPath->empty()) {
    deps_.lastRenderedPath->clear();
    if (deps_.saveAppState) deps_.saveAppState();
  }
}

}  // namespace v2
}  // namespace sleep
}  // namespace crosspoint
