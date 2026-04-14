#include "ActivityWithSubactivity.h"

#include <HalPowerManager.h>

void ActivityWithSubactivity::renderTaskLoop() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    {
      HalPowerManager::Lock powerLock;  // Ensure we don't go into low-power mode while rendering
      RenderLock lock(*this);
      if (!subActivity) {
        render(std::move(lock));
      }
      // If subActivity is set, consume the notification but skip parent render
      // Note: the sub-activity will call its render() from its own display task
    }
  }
}

void ActivityWithSubactivity::exitActivity() {
  // No need to lock, since onExit() already acquires its own lock
  if (subActivity) {
    LOG_DBG("ACT", "Exiting subactivity...");
    subActivity->onExit();
    subActivity.reset();
    // Restore parent render task (was suspended to free 8 KB of heap for the
    // subactivity).  Must happen after subActivity is destroyed so only one
    // render task is live at a time.
    if (!renderTaskHandle) {
      xTaskCreate(&renderTaskTrampoline, name.c_str(), 8192, this, 1,
                  &renderTaskHandle);
    }
    // Suppress stale button events so that the press/release that closed the
    // subactivity (e.g. Confirm on a dialog) doesn't leak into the parent's
    // loop() and trigger an unintended action (like opening a book).
    mappedInput.suppressUntilAllReleased();
  }
}

void ActivityWithSubactivity::enterNewActivity(Activity* activity) {
  // Acquire lock to avoid 2 activities rendering at the same time during transition
  RenderLock lock(*this);
  // Delete the parent render task to free 8 KB of stack.  The parent's
  // renderTaskLoop skips rendering while a subactivity is active, so the task
  // is pure overhead.  It is recreated in exitActivity().
  if (renderTaskHandle) {
    vTaskDelete(renderTaskHandle);
    renderTaskHandle = nullptr;
  }
  // Suppress stale button events so the press that opened this subactivity
  // doesn't immediately trigger an action inside it.
  mappedInput.suppressUntilAllReleased();
  subActivity.reset(activity);
  subActivity->onEnter();
}

void ActivityWithSubactivity::loop() {
  if (subActivity) {
    subActivity->loop();
  }
}

void ActivityWithSubactivity::requestUpdate() {
  if (!subActivity) {
    Activity::requestUpdate();
  }
  // Sub-activity should call their own requestUpdate() from their loop() function
}

void ActivityWithSubactivity::onExit() {
  // No need to lock, onExit() already acquires its own lock
  exitActivity();
  Activity::onExit();
}
