#include "WsUploadSession.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>

namespace crosspoint {
namespace ws {

namespace {

std::string sanitizeFileName(std::string name) {
  // Match prior behavior: strip path separators and ".." sequences so a
  // hostile client cannot traverse out of the target directory.
  for (char bad : {'/', '\\'}) {
    std::string::size_type p;
    while ((p = name.find(bad)) != std::string::npos) name.erase(p, 1);
  }
  std::string::size_type p;
  while ((p = name.find("..")) != std::string::npos) name.erase(p, 2);
  return name;
}

std::string normalizePath(std::string p) {
  if (p.empty()) p = "/";
  if (p.front() != '/') p.insert(p.begin(), '/');
  return p;
}

}  // namespace

WsUploadSession::WsUploadSession(WsUploadDeps deps) : deps_(std::move(deps)) {}

void WsUploadSession::reset() {
  state_ = State::Idle;
  client_ = kNoClient;
  name_.clear();
  path_.clear();
  size_ = 0;
  received_ = 0;
  lastProgress_ = 0;
  lastDataMs_ = 0;
}

bool WsUploadSession::parseStart(const std::string& msg, std::string& name,
                                 size_t& size, std::string& path) {
  // Format: "START:<name>:<size>:<path>"
  constexpr const char* kPrefix = "START:";
  constexpr size_t kPrefixLen = 6;
  if (msg.size() <= kPrefixLen) return false;
  if (msg.compare(0, kPrefixLen, kPrefix) != 0) return false;

  const size_t firstColon = msg.find(':', kPrefixLen);
  if (firstColon == std::string::npos) return false;
  const size_t secondColon = msg.find(':', firstColon + 1);
  if (secondColon == std::string::npos) return false;

  name = msg.substr(kPrefixLen, firstColon - kPrefixLen);
  const std::string sizeStr = msg.substr(firstColon + 1, secondColon - firstColon - 1);
  path = msg.substr(secondColon + 1);

  if (name.empty() || sizeStr.empty()) return false;
  // Exception-free size parse — device build has -fno-exceptions so stoll is
  // unusable. strtoll reports failure via endptr + errno.
  errno = 0;
  char* end = nullptr;
  const long long parsed = std::strtoll(sizeStr.c_str(), &end, 10);
  if (errno != 0 || end == sizeStr.c_str() || *end != '\0' || parsed < 0) {
    return false;
  }
  size = static_cast<size_t>(parsed);
  return true;
}

WsUploadSession::Result WsUploadSession::onStart(uint8_t client,
                                                 const std::string& startMsg) {
  if (state_ == State::Active) {
    if (deps_.sendText) deps_.sendText(client, "ERROR:Upload already in progress");
    return Result::Rejected;
  }

  std::string name;
  std::string path;
  size_t size = 0;
  if (!parseStart(startMsg, name, size, path)) {
    if (deps_.sendText) deps_.sendText(client, "ERROR:Invalid START format");
    return Result::Rejected;
  }

  name = sanitizeFileName(std::move(name));
  path = normalizePath(std::move(path));
  if (name.empty()) {
    if (deps_.sendText) deps_.sendText(client, "ERROR:Invalid filename");
    return Result::Rejected;
  }

  if (!deps_.beginWrite || !deps_.beginWrite(path, name)) {
    if (deps_.sendText) deps_.sendText(client, "ERROR:Failed to create file");
    return Result::Rejected;
  }

  state_ = State::Active;
  client_ = client;
  name_ = std::move(name);
  path_ = std::move(path);
  size_ = size;
  received_ = 0;
  lastProgress_ = 0;
  lastDataMs_ = deps_.nowMs ? deps_.nowMs() : 0;

  if (deps_.sendText) deps_.sendText(client, "READY");
  return Result::Ok;
}

WsUploadSession::Result WsUploadSession::onBinary(uint8_t client,
                                                  const uint8_t* data, size_t len) {
  if (state_ != State::Active || client != client_) {
    if (deps_.sendText) deps_.sendText(client, "ERROR:No upload in progress");
    return Result::Rejected;
  }

  const size_t remaining = size_ - received_;
  if (len > remaining) {
    abort("Upload overflow");
    return Result::Aborted;
  }

  if (!deps_.writeBytes || !deps_.writeBytes(data, len)) {
    abort("Write failed - disk full?");
    return Result::Aborted;
  }
  received_ += len;
  if (deps_.nowMs) lastDataMs_ = deps_.nowMs();

  if (received_ - lastProgress_ >= kProgressIntervalBytes || received_ >= size_) {
    sendProgress();
    lastProgress_ = received_;
  }

  if (received_ >= size_) {
    if (deps_.closeFinalize) deps_.closeFinalize();
    if (deps_.sendText) deps_.sendText(client_, "DONE");
    if (deps_.clearBookCache) deps_.clearBookCache(path_, name_);
    reset();
    return Result::Completed;
  }
  return Result::Ok;
}

void WsUploadSession::onDisconnect(uint8_t client) {
  // Only the owning client can end the session — late DISCONNECTED for a
  // client who never owned the active upload must be ignored.
  if (state_ != State::Active || client != client_) return;
  if (deps_.closeAndDelete) deps_.closeAndDelete();
  reset();
}

void WsUploadSession::tick(uint32_t nowMs) {
  if (state_ != State::Active) return;
  if (nowMs - lastDataMs_ < kIdleTimeoutMs) return;
  abort("Upload idle timeout");
}

void WsUploadSession::abort(const std::string& reason) {
  if (state_ != State::Active) return;
  const uint8_t owner = client_;
  if (deps_.closeAndDelete) deps_.closeAndDelete();
  if (deps_.sendText) deps_.sendText(owner, std::string("ERROR:") + reason);
  reset();
}

void WsUploadSession::sendProgress() {
  if (!deps_.sendText) return;
  char buf[64];
  std::snprintf(buf, sizeof(buf), "PROGRESS:%zu:%zu", received_, size_);
  deps_.sendText(client_, buf);
}

WsUploadSession::Snapshot WsUploadSession::snapshot() const {
  Snapshot s;
  s.active = state_ == State::Active;
  s.received = received_;
  s.total = size_;
  s.filename = name_;
  return s;
}

}  // namespace ws
}  // namespace crosspoint
