/**
 * @file ReaderSession.h
 * @brief Shared "a book is being read" mechanics for the three readers (RFC #171).
 *
 * Owns the progress observe→debounce→flush orchestration (via the host-tested
 * ReaderProgressTracker) and the onEnter skeleton (refresh decision, orientation,
 * bold-swap, recent-book registration) that EpubReaderActivity / TxtReaderActivity
 * / XtcReaderActivity each hand-rolled. The reader supplies its content model
 * through ReaderHooks; every hardware/global dependency is behind a port, so the
 * whole class is host-compilable and the orchestration is host-testable with
 * in-memory fakes (no GfxRenderer, SETTINGS, RECENT_BOOKS, or millis() here).
 *
 * Deliberately does NOT own: the orientation/render-mode toggles (real per-reader
 * nuance — Txt re-lays-out, Xtc is pre-rendered, Epub differs), section/page
 * loading, footnotes, highlight, draw, or the deferred-action queue (per-reader
 * DeferredActionQueue, RFC #167). Those stay in the activity; toggles compose
 * progress() for their flush/seed.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "ReaderProgressTracker.h"

namespace crosspoint {
namespace reader {

// Recent-books + refresh-policy seam. Prod wraps ReaderCommon / RECENT_BOOKS;
// tests record calls into vectors.
struct IReaderEnvPort {
  virtual ~IReaderEnvPort() = default;
  virtual bool shouldFullRefreshOnEnter(const std::string& path) = 0;   // stateful; call once per enter
  virtual bool boldSwap(const std::string& path) const = 0;
  virtual void registerOpened(const std::string& path, const std::string& title, const std::string& author,
                              const std::string& thumb) = 0;
  virtual std::string moveBookToRecents(const std::string& path) = 0;   // "" if not relocated
};

// Display seam. Prod wraps GfxRenderer + ReaderCommon::applyReaderOrientation +
// EpdFontFamily; tests record the calls. applyOrientationFromSettings reads the
// global orientation inside the prod adapter so the core needs no settings port.
struct IReaderDisplayPort {
  virtual ~IReaderDisplayPort() = default;
  virtual void requestRefresh(bool full) = 0;
  virtual void applyOrientationFromSettings() = 0;
  virtual void setBoldSwap(bool enabled) = 0;
};

// The reader's content seam. Single-document readers fill `path` + `position`
// (position returns {0, currentPage, 1}); the optional onEnter inserts are where
// Epub does its heap-anchor / CSS / bookmark work.
struct ReaderHooks {
  std::function<std::string()>    path;            // REQUIRED
  std::function<ReaderPosition()> position;        // REQUIRED
  std::function<void()>           beforeRefresh;   // optional onEnter inserts
  std::function<void()>           afterOrientation;
  std::function<void()>           afterRegister;
  std::function<void(std::string& title, std::string& author, std::string& thumb)> recentMeta;  // optional
};

class ReaderSession {
 public:
  struct Ports {
    IProgressSink&      sink;
    IReaderEnvPort&     env;
    IReaderDisplayPort& display;
  };

  ReaderSession(Ports ports, ReaderHooks hooks,
                uint32_t debounceMs = ReaderProgressTracker::kDefaultDebounceMs)
      : env_(ports.env), display_(ports.display), hooks_(std::move(hooks)), progress_(ports.sink, debounceMs) {}

  // onEnter skeleton: beforeRefresh → refresh decision → orientation → bold-swap
  // → afterOrientation → register recent → afterRegister → moveBookToRecents.
  // Seeds the tracker with `loaded` (no write). Returns the relocated path if the
  // book was moved to /recents/ (caller updates its document + APP_STATE), else "".
  std::string enter(const ReaderPosition& loaded);

  // onExit: force-flush the current position + disable bold-swap.
  void exit(uint32_t nowMs);

  // Per-render heartbeat: observe(hooks.position()) + debounced flush, folded so a
  // render that calls tick() once can never observe-without-flushing. force=true
  // (exit/pre-action) flushes immediately.
  void tick(uint32_t nowMs, bool force = false);

  // Out-of-band reposition (KOReader sync): flush pending, then reseed to `p`.
  void resetTo(const ReaderPosition& p, uint32_t nowMs);

  // Escape hatch: direct tracker access for the toggle/relayout reposition paths
  // (seed after re-layout) and Epub's percent/AsyncWriter flush tail.
  ReaderProgressTracker& progress() { return progress_; }

 private:
  IReaderEnvPort&       env_;
  IReaderDisplayPort&   display_;
  ReaderHooks           hooks_;
  ReaderProgressTracker progress_;
};

}  // namespace reader
}  // namespace crosspoint
