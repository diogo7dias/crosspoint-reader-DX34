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
#include "esp_system.h"  // esp_restart()

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

 public:
  explicit Activity(std::string name, GfxRenderer& renderer, MappedInputManager& mappedInput)
      : name(std::move(name)),
        renderer(renderer),
        mappedInput(mappedInput),
        renderingMutex(xSemaphoreCreateMutex()),
        renderDoneSemaphore(xSemaphoreCreateBinary()) {
    // Semaphore creation fails only on heap exhaustion. assert() compiles to
    // nothing in release builds (NDEBUG), so use an explicit check + restart.
    // The device is non-functional without these semaphores.
    if (!renderingMutex || !renderDoneSemaphore) {
      LOG_ERR("ACT", "FATAL: semaphore alloc failed for '%s' — heap exhausted, restarting", name.c_str());
      esp_restart();
    }
  }
  virtual ~Activity() {
    vSemaphoreDelete(renderingMutex);
    renderingMutex = nullptr;
    vSemaphoreDelete(renderDoneSemaphore);
    renderDoneSemaphore = nullptr;
  };
  class RenderLock;
  virtual void onEnter();
  virtual void onExit();
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
