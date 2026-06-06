/**
 * @file LibraryEnvPort.h
 * @brief Role port for MyLibraryActivity's settings/state/recents access.
 *
 * Narrow, consumer-shaped seam (RFC #175). Exposes only the handful of things
 * the library browser actually needs from the three global singletons
 * (SETTINGS / APP_STATE / RECENT_BOOKS), so the activity declares its
 * dependency explicitly and can later be unit-tested with a small in-memory
 * fake instead of reaching the globals directly. Mirrors the ReaderSession
 * ProdEnvPort idiom (RFC #171).
 *
 * Device wiring is via ProdLibraryEnvPort (forwards to the real singletons);
 * the production default is defaultLibraryEnv(). Host tests pass a fake.
 */
#pragma once
#include <string>

namespace crosspoint {
namespace home {

// Everything MyLibraryActivity touches through the global singletons.
struct ILibraryEnvPort {
  virtual ~ILibraryEnvPort() = default;

  // --- SETTINGS (read) ---
  // SETTINGS.showHiddenFiles — when false, dotfiles are filtered from listings.
  virtual bool showHiddenFiles() const = 0;
  // SETTINGS.booksFolderOrder == 1 — shuffle the /books listing instead of A-Z.
  virtual bool shuffleBooksFolder() const = 0;

  // --- RECENT_BOOKS ---
  // Cached reading percent for a book path (0-100, or -1 if unknown/untracked).
  virtual int cachedPercent(const std::string& path) const = 0;
  // Drop a book from the recents list (e.g. on delete / move).
  virtual void removeRecent(const std::string& path) = 0;

  // --- APP_STATE (write) ---
  // Persist runtime state to disk (debounced). Called after favorite-image
  // path edits that ride along on file rename / move / delete.
  virtual void persistState() = 0;
};

// Production adapter — stateless forwarder to the real singletons.
class ProdLibraryEnvPort final : public ILibraryEnvPort {
 public:
  bool showHiddenFiles() const override;
  bool shuffleBooksFolder() const override;
  int cachedPercent(const std::string& path) const override;
  void removeRecent(const std::string& path) override;
  void persistState() override;
};

// Default production instance (function-local static; lifetime = program).
ILibraryEnvPort& defaultLibraryEnv();

}  // namespace home
}  // namespace crosspoint
