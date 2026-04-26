/**
 * @file Activity.h
 * @brief Base class for all UI screens (activities) in the application.
 *
 * Each screen (home, reader, settings, etc.) is an Activity subclass.
 * Lifecycle: onEnter() -> loop() [called each main-loop tick] -> onExit().
 * Rendering runs on a dedicated FreeRTOS task to avoid blocking the main loop.
 *
 * RenderLock (RAII) ensures the activity is not deleted mid-render.
 * Use requestUpdate() to trigger a render, requestUpdateAndWait() to block
 * until the frame is displayed (e.g., before entering deep sleep).
 */
#pragma once
#include <HardwareSerial.h>
#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <string>
#include <utility>

#include "GfxRenderer.h"
#include "MappedInputManager.h"

class Activity {
 protected:
  std::string name;
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;

  // Task to render and display the activity
  TaskHandle_t renderTaskHandle = nullptr;
  [[noreturn]] static void renderTaskTrampoline(void* param);
  [[noreturn]] virtual void renderTaskLoop();

  // Mutex to protect rendering operations from being deleted mid-render
  SemaphoreHandle_t renderingMutex = nullptr;
  // Signaled by render task after a frame finishes, used by requestUpdateAndWait().
  SemaphoreHandle_t renderDoneSemaphore = nullptr;

  // Set true when ctor or onEnter() could not bring the activity up
  // (semaphore alloc, pre-flight heap floor, or xTaskCreate failure).
  // Callers in enterNewActivity() check didEntryFail() and route to a
  // graceful OOM screen instead of crashing the device.
  bool entryFailed = false;

 public:
  explicit Activity(std::string name, GfxRenderer& renderer, MappedInputManager& mappedInput)
      : name(std::move(name)),
        renderer(renderer),
        mappedInput(mappedInput),
        renderingMutex(xSemaphoreCreateMutex()),
        renderDoneSemaphore(xSemaphoreCreateBinary()) {
    if (!renderingMutex || !renderDoneSemaphore) {
      LOG_ERR("ACT", "Semaphore alloc failed for '%s' — flagging entry failure", name.c_str());
      entryFailed = true;
    }
  }
  virtual ~Activity() {
    if (renderingMutex) {
      vSemaphoreDelete(renderingMutex);
      renderingMutex = nullptr;
    }
    if (renderDoneSemaphore) {
      vSemaphoreDelete(renderDoneSemaphore);
      renderDoneSemaphore = nullptr;
    }
  };
  class RenderLock;
  virtual void onEnter();
  virtual void onExit();
  // True iff the ctor or onEnter() could not bring the activity up.
  // enterNewActivity() callers MUST check this and replace the failed
  // activity with a graceful OOM screen instead of using it.
  bool didEntryFail() const { return entryFailed; }
  virtual void loop() {}

  virtual void render(RenderLock&&) {}
  virtual void requestUpdate();
  virtual void requestUpdateAndWait();

  virtual bool skipLoopDelay() { return false; }
  virtual bool preventAutoSleep() { return false; }
  virtual bool isReaderActivity() const { return false; }

  // RAII helper to lock rendering mutex for the duration of a scope.
  class RenderLock {
    Activity& activity;

   public:
    explicit RenderLock(Activity& activity);
    RenderLock(const RenderLock&) = delete;
    RenderLock& operator=(const RenderLock&) = delete;
    ~RenderLock();
  };
};
