#include "Activity.h"

#include <HalPowerManager.h>
#include <esp_heap_caps.h>

// Declared in main.cpp. Drops the FCM cache synchronously to free
// heap on the failing task. Safe to invoke from any task context.
extern "C" void onHeapAllocFailed(size_t requested, uint32_t caps, const char* function_name);

namespace {
// 8 KB render-task stack + ~4 KB margin for trampoline / FreeRTOS bookkeeping.
constexpr size_t kRenderTaskMinLargestBlock = 12 * 1024;

bool hasRoomForRenderTask() {
  return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) >= kRenderTaskMinLargestBlock;
}
}  // namespace

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
  // Ctor may already have flagged a semaphore failure — don't try to spawn
  // the render task in that case, the caller will swap us out.
  if (entryFailed) {
    LOG_ERR("ACT", "Skipping render task for '%s' — ctor flagged entry failure", name.c_str());
    return;
  }

  // Pre-flight the heap: xTaskCreate needs a contiguous 8 KB block for the
  // stack. On a fragmented heap the largest free block can drop below that
  // even when total free heap looks healthy. If we're tight, drop the FCM
  // (PR #99 path) and re-check before committing to xTaskCreate.
  if (!hasRoomForRenderTask()) {
    LOG_ERR("ACT", "Pre-flight low for '%s' (largest=%u) — flushing caches", name.c_str(),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    onHeapAllocFailed(8192, MALLOC_CAP_8BIT, "Activity::onEnter pre-flight");
    if (!hasRoomForRenderTask()) {
      LOG_ERR("ACT", "Still low after flush for '%s' (largest=%u) — flagging entry failure", name.c_str(),
              static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
      entryFailed = true;
      return;
    }
  }

  xTaskCreate(&renderTaskTrampoline, name.c_str(),
              8192,              // Stack size
              this,              // Parameters
              1,                 // Priority
              &renderTaskHandle  // Task handle
  );
  if (!renderTaskHandle) {
    LOG_ERR("ACT", "Render task alloc failed for '%s' — flagging entry failure", name.c_str());
    entryFailed = true;
    return;
  }
  LOG_DBG("ACT", "Entering activity: %s", name.c_str());
}

void Activity::onExit() {
  // If the ctor flagged entry failure, renderingMutex may be null and no
  // render task was ever spawned. Skip the lock-and-delete dance.
  if (!renderingMutex) {
    LOG_DBG("ACT", "Exiting activity (no semaphores): %s", name.c_str());
    return;
  }
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
