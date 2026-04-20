#pragma once

#include <string>

// Trash: undo window for deleted books, quotes, and wallpapers.
//
// Deleting via the library browser moves the target into a numbered slot
// under /.crosspoint/trash/ instead of hard-removing it. Slots are
// pruned to a fixed cap (oldest first) at boot.
namespace trash {

constexpr size_t kDefaultCap = 50;

/// Move `path` into a new trash slot. Returns true on success.
///
/// If `path` points to a book file (epub/xtc/txt/md), the book's cache
/// dir and its `_QUOTES.txt` sidecar are also moved into the same slot.
/// On failure (partial moves), the slot directory is left in place so
/// the user can inspect it manually; nothing is ever hard-deleted.
bool moveToTrash(const std::string& path);

/// Keep only the most recent `cap` trash slots. Oldest (lowest-numbered)
/// slots are removed. Safe to call at boot before anything else writes.
void pruneToCap(size_t cap = kDefaultCap);

}  // namespace trash
