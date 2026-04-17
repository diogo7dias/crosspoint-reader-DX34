#include "HalGPIO.h"

#include <SDL2/SDL.h>

#include <atomic>

// Defined in HalDisplay.cpp — set here so all SDL event polling lives in one place.
extern std::atomic<bool> quitRequested;

// Keyboard mapping (matches DX34 physical layout):
//   BTN_BACK    (0) → 1
//   BTN_CONFIRM (1) → 2
//   BTN_LEFT    (2) → 3
//   BTN_RIGHT   (3) → 4
//   BTN_UP      (4) → 8  (side up)
//   BTN_DOWN    (5) → 9  (side down)
//   BTN_POWER   (6) → 0

static constexpr int NUM_BUTTONS = 7;

// Simulate a quick physical-button tap: even if the user holds the keyboard
// key longer, treat the button as released after this many ms. Prevents
// accidental long-press triggers (settings adjust, power off) and double
// firing caused by keyboard dwell being much longer than a finger tap.
static constexpr unsigned long SIM_AUTO_RELEASE_MS = 50;

static const SDL_Scancode buttonScancode[NUM_BUTTONS] = {
    SDL_SCANCODE_1,  // BTN_BACK
    SDL_SCANCODE_2,  // BTN_CONFIRM
    SDL_SCANCODE_3,  // BTN_LEFT
    SDL_SCANCODE_4,  // BTN_RIGHT
    SDL_SCANCODE_8,  // BTN_UP
    SDL_SCANCODE_9,  // BTN_DOWN
    SDL_SCANCODE_0,  // BTN_POWER
};

static bool pressedThisFrame[NUM_BUTTONS] = {};
static bool releasedThisFrame[NUM_BUTTONS] = {};
static unsigned long buttonPressTime[NUM_BUTTONS] = {};
// True once the auto-release has fired for this key-down; cleared on next key-up.
// isPressed / getHeldTime treat the button as released while this is set.
static bool virtualReleased[NUM_BUTTONS] = {};
// Set by suppressUntilAllReleased(): swallow press/release edges and report
// isPressed=false until every key is physically released. Prevents the key
// that drove an activity transition from leaking events into the next screen.
static bool suppressActive = false;

static int scancodeToButton(SDL_Scancode sc) {
  for (int i = 0; i < NUM_BUTTONS; i++) {
    if (buttonScancode[i] == sc) return i;
  }
  return -1;
}

void HalGPIO::begin() {}

void HalGPIO::update() {
  // Reset per-frame state
  for (int i = 0; i < NUM_BUTTONS; i++) {
    pressedThisFrame[i] = false;
    releasedThisFrame[i] = false;
  }

  // If we were asked to suppress until all keys released, check the live
  // keyboard state; when nothing is held, the transition-originating press
  // has ended and we can start reporting events again.
  if (suppressActive) {
    const uint8_t* state = SDL_GetKeyboardState(NULL);
    bool anyDown = false;
    for (int i = 0; i < NUM_BUTTONS; i++) {
      if (state[buttonScancode[i]] && !virtualReleased[i]) {
        anyDown = true;
        break;
      }
    }
    if (!anyDown) suppressActive = false;
  }

  // HalGPIO owns all SDL event polling so keyboard and quit events are never
  // split between two callers (HalDisplay::presentIfNeeded only renders).
  SDL_Event e;
  while (SDL_PollEvent(&e) != 0) {
    if (e.type == SDL_QUIT) {
      quitRequested.store(true);
    } else if (e.type == SDL_KEYDOWN && !e.key.repeat) {
      int btn = scancodeToButton(e.key.keysym.scancode);
      if (btn >= 0) {
        pressedThisFrame[btn] = true;
        buttonPressTime[btn] = SDL_GetTicks();
        virtualReleased[btn] = false;
      }
    } else if (e.type == SDL_KEYUP) {
      int btn = scancodeToButton(e.key.keysym.scancode);
      if (btn >= 0) {
        // Only surface a release edge if we didn't already synthesize one.
        if (!virtualReleased[btn]) {
          releasedThisFrame[btn] = true;
        }
        virtualReleased[btn] = false;
        buttonPressTime[btn] = 0;
      }
    }
  }

  // Synthetic auto-release: if a button has been held longer than the tap
  // window, fire a release edge this frame and mark it virtually released
  // so subsequent isPressed() calls return false even while the key stays
  // down on the physical keyboard.
  const unsigned long now = SDL_GetTicks();
  for (int i = 0; i < NUM_BUTTONS; i++) {
    if (!virtualReleased[i] && buttonPressTime[i] > 0 &&
        (now - buttonPressTime[i]) > SIM_AUTO_RELEASE_MS) {
      virtualReleased[i] = true;
      releasedThisFrame[i] = true;
    }
  }

  // While suppression is active, swallow every edge this frame. Leaves
  // pressed/released arrays cleared so wasPressed/wasReleased return false.
  if (suppressActive) {
    for (int i = 0; i < NUM_BUTTONS; i++) {
      pressedThisFrame[i] = false;
      releasedThisFrame[i] = false;
    }
  }
}

void HalGPIO::suppressUntilAllReleased() {
  suppressActive = true;
  for (int i = 0; i < NUM_BUTTONS; i++) {
    pressedThisFrame[i] = false;
    releasedThisFrame[i] = false;
  }
}

bool HalGPIO::isPressed(uint8_t buttonIndex) const {
  if (buttonIndex >= NUM_BUTTONS) return false;
  if (suppressActive) return false;
  if (virtualReleased[buttonIndex]) return false;
  const uint8_t* state = SDL_GetKeyboardState(NULL);
  return state[buttonScancode[buttonIndex]];
}

bool HalGPIO::wasPressed(uint8_t buttonIndex) const {
  if (buttonIndex >= NUM_BUTTONS) return false;
  return pressedThisFrame[buttonIndex];
}

bool HalGPIO::wasReleased(uint8_t buttonIndex) const {
  if (buttonIndex >= NUM_BUTTONS) return false;
  return releasedThisFrame[buttonIndex];
}

bool HalGPIO::wasAnyPressed() const {
  for (int i = 0; i < NUM_BUTTONS; i++) {
    if (pressedThisFrame[i]) return true;
  }
  return false;
}

bool HalGPIO::wasAnyReleased() const {
  for (int i = 0; i < NUM_BUTTONS; i++) {
    if (releasedThisFrame[i]) return true;
  }
  return false;
}

unsigned long HalGPIO::getHeldTime() const {
  // Return the longest held time among all currently pressed buttons.
  // A virtually-released button contributes 0 so long-press logic can't
  // fire while the keyboard key remains physically down.
  unsigned long now = SDL_GetTicks();
  unsigned long maxHeld = 0;
  const uint8_t* state = SDL_GetKeyboardState(NULL);
  for (int i = 0; i < NUM_BUTTONS; i++) {
    if (virtualReleased[i]) continue;
    if (state[buttonScancode[i]] && buttonPressTime[i] > 0) {
      unsigned long held = now - buttonPressTime[i];
      if (held > maxHeld) maxHeld = held;
    }
  }
  return maxHeld;
}

HalGPIO::WakeupReason HalGPIO::getWakeupReason() const { return WakeupReason::Other; }
bool HalGPIO::isUsbConnected() const { return true; }
bool HalGPIO::wasUsbStateChanged() const { return false; }
void HalGPIO::startDeepSleep() {}
void HalGPIO::verifyPowerButtonWakeup(uint16_t /*requiredDurationMs*/, bool /*shortPressAllowed*/) {}

HalGPIO gpio;
