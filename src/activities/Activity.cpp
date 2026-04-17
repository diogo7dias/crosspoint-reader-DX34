#include "Activity.h"

#include <HalPowerManager.h>

void Activity::renderTaskTrampoline(void* param) {
  auto* self = static_cast<Activity*>(param);
  self->renderTaskLoop();
}

void Activity::renderTaskLoop() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    {
      HalPowerManager::Lock powerLock;  // Ensure we don't go into low-power mode while rendering
      RenderLock lock(*this);
      render(std::move(lock));
    }
    if (renderDoneSemaphore) {
      xSemaphoreGive(renderDoneSemaphore);
    }
  }
}

void Activity::onEnter() {
  xTaskCreate(&renderTaskTrampoline, name.c_str(),
              8192,              // Stack size
              this,              // Parameters
              1,                 // Priority
              &renderTaskHandle  // Task handle
  );
  if (!renderTaskHandle) {
    LOG_ERR("ACT", "FATAL: render task alloc failed for '%s' — heap exhausted, restarting", name.c_str());
    esp_restart();
  }
  LOG_DBG("ACT", "Entering activity: %s", name.c_str());
}

void Activity::onExit() {
  RenderLock lock(*this);  // Ensure we don't delete the task while it's rendering
  if (renderTaskHandle) {
    vTaskDelete(renderTaskHandle);
    renderTaskHandle = nullptr;
  }

  LOG_DBG("ACT", "Exiting activity: %s", name.c_str());
}

void Activity::requestUpdate() {
  // Using direct notification to signal the render task to update.
  // Increment counter so multiple rapid calls won't be lost.
  // Local copy avoids TOCTOU if onExit() nulls the handle concurrently.
  TaskHandle_t handle = renderTaskHandle;
  if (handle) {
    xTaskNotify(handle, 1, eIncrement);
  }
}

void Activity::requestUpdateAndWait() {
  if (!renderTaskHandle || !renderDoneSemaphore) {
    return;
  }

  // Drain stale completion signals so we wait for the frame we are about to request.
  while (xSemaphoreTake(renderDoneSemaphore, 0) == pdTRUE) {
  }

  requestUpdate();
  xSemaphoreTake(renderDoneSemaphore, pdMS_TO_TICKS(250));
}

// RenderLock

Activity::RenderLock::RenderLock(Activity& activity) : activity(activity) {
  xSemaphoreTake(activity.renderingMutex, portMAX_DELAY);
}

Activity::RenderLock::~RenderLock() { xSemaphoreGive(activity.renderingMutex); }
