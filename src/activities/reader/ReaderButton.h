// Pure mirror of the reader's logical buttons (RFC #165).
//
// Deliberately NOT MappedInputManager::Button: that enum is a member of a
// HAL-coupled class whose header pulls in HalGPIO, which the host test build
// (`[env:test_host]`, which lib_ignores MappedInputManager + hal) cannot link.
// The ReaderInputDispatcher core speaks this enum only; the device-side
// adapter owns the single ReaderButton <-> MappedInputManager::Button mapping
// (guarded by a static_assert on count), so the HAL enum never reaches the
// pure, host-testable core.
#pragma once

#include <cstdint>

namespace crosspoint {
namespace reader {

enum class ReaderButton : uint8_t {
  Back = 0,
  Confirm,
  Left,
  Right,
  Up,
  Down,
  Power,
  PageBack,
  PageForward,
  Count,
};

constexpr int kReaderButtonCount = static_cast<int>(ReaderButton::Count);

}  // namespace reader
}  // namespace crosspoint
