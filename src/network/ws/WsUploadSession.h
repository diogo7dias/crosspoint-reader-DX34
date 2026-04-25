#pragma once

// WsUploadSession — state machine for the WebSocket file-upload protocol.
//
// Extracts the 10-file-scope-static tangle from CrossPointWebServer.cpp
// (see RFC #24 / GitHub issue #24). Fixes:
//
//   1. Client-collision race — onDisconnect(n) only resets state if n owns
//      the active session. Stale DISCONNECTED for a retired client is a no-op.
//
//   2. Idle timeout — tick(nowMs) aborts sessions that have not received a
//      binary frame in kIdleTimeoutMs. Half-open TCP no longer wedges future
//      uploads until reboot.
//
// The session holds only logical state (strings + counters). File I/O and
// network I/O flow through Deps lambdas so host tests can inject fakes —
// no Arduino, no SdFat, no WebSockets dependency in this header or its .cpp.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace crosspoint {
namespace ws {

struct WsUploadDeps {
  // Open a file at <path>/<name> for writing. Return true on success, false on
  // storage error. If the file already exists the production impl removes it
  // first; this contract is preserved for backward compatibility.
  std::function<bool(const std::string& path, const std::string& name)> beginWrite;

  // Write a chunk into the currently-open file. Return true if all bytes were
  // written. On false the session aborts (treated as disk-full / write error).
  std::function<bool(const uint8_t* data, size_t len)> writeBytes;

  // Close the open file and remove the partial artifact. Called on abort().
  std::function<void()> closeAndDelete;

  // Close the open file without deletion. Called on successful completion.
  std::function<void()> closeFinalize;

  // Send a WS TEXT frame to <client>.
  std::function<void(uint8_t client, const std::string& text)> sendText;

  // Monotonic milliseconds — injected so host tests can drive timeouts.
  std::function<uint32_t()> nowMs;

  // Clear any cached metadata derived from <path>/<name> (e.g. epub caches).
  // Called after a successful upload so overwritten books re-parse.
  std::function<void(const std::string& path, const std::string& name)> clearBookCache;
};

class WsUploadSession {
 public:
  enum class Result { Ok, Rejected, Completed, Aborted };

  // Generous idle cap so a brief client stall or slow network won't
  // kill a running upload, but a truly crashed browser still
  // eventually releases the SD file descriptor. User-facing session
  // stays up regardless.
  static constexpr uint32_t kIdleTimeoutMs = 10u * 60u * 1000u;  // 10 minutes
  static constexpr size_t kProgressIntervalBytes = 65536;  // 64 KiB
  static constexpr uint8_t kNoClient = 255;

  explicit WsUploadSession(WsUploadDeps deps);

  // Parse "START:<name>:<size>:<path>" and open the target file. Rejects
  // with an error frame if another client already owns the session or the
  // message is malformed.
  Result onStart(uint8_t client, const std::string& startMsg);

  // Append bytes to the open file. On overflow / write error aborts and
  // returns Aborted. On successful completion closes the file, emits DONE,
  // and returns Completed.
  Result onBinary(uint8_t client, const uint8_t* data, size_t len);

  // Only transitions the session to Idle if <client> is the current owner.
  // Late-firing DISCONNECTED for a retired client is explicitly ignored.
  void onDisconnect(uint8_t client);

  // Must be called from the server loop. Aborts an active session that has
  // not received data in the last kIdleTimeoutMs.
  void tick(uint32_t nowMs);

  // Abort the active session. Sends ERROR:<reason> to the owner and cleans
  // up the partial file. No-op if idle.
  void abort(const std::string& reason);

  bool inProgress() const { return state_ == State::Active; }
  uint8_t ownerClient() const { return client_; }

  struct Snapshot {
    bool active = false;
    size_t received = 0;
    size_t total = 0;
    std::string filename;
  };
  Snapshot snapshot() const;

 private:
  enum class State { Idle, Active };

  State state_ = State::Idle;
  uint8_t client_ = kNoClient;
  std::string name_;
  std::string path_;
  size_t size_ = 0;
  size_t received_ = 0;
  size_t lastProgress_ = 0;
  uint32_t lastDataMs_ = 0;

  WsUploadDeps deps_;

  void reset();
  void sendProgress();
  bool parseStart(const std::string& msg, std::string& name, size_t& size, std::string& path);
};

}  // namespace ws
}  // namespace crosspoint
