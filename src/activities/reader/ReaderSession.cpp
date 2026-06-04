#include "ReaderSession.h"

namespace crosspoint {
namespace reader {

std::string ReaderSession::enter(const ReaderPosition& loaded) {
  const std::string path = hooks_.path();

  if (hooks_.beforeRefresh) hooks_.beforeRefresh();

  // Refresh decision must precede the first draw (ghost-pixel compromise — see
  // ReaderCommon::shouldFullRefreshOnEnter). Stateful: called exactly once.
  display_.requestRefresh(env_.shouldFullRefreshOnEnter(path));
  if (applyOrientationOnEnter_) display_.applyOrientationFromSettings();
  display_.setBoldSwap(env_.boldSwap(path));

  if (hooks_.afterOrientation) hooks_.afterOrientation();

  std::string title, author, thumb;
  if (hooks_.recentMeta) hooks_.recentMeta(title, author, thumb);
  env_.registerOpened(path, title, author, thumb);

  if (hooks_.afterRegister) hooks_.afterRegister();

  // Path may relocate to /recents/ on first open from another location; the
  // caller updates its document object + APP_STATE.openEpubPath from the return.
  const std::string moved = env_.moveBookToRecents(path);

  progress_.seed(loaded);
  return moved;
}

void ReaderSession::exit(uint32_t nowMs) {
  if (hooks_.position) {
    progress_.observe(hooks_.position(), nowMs);
    progress_.flush(nowMs, /*force=*/true);
  }
  display_.setBoldSwap(false);
}

void ReaderSession::tick(uint32_t nowMs, bool force) {
  progress_.observe(hooks_.position(), nowMs);
  progress_.flush(nowMs, force);
}

void ReaderSession::resetTo(const ReaderPosition& p, uint32_t nowMs) { progress_.snapshotForReset(p, nowMs); }

}  // namespace reader
}  // namespace crosspoint
