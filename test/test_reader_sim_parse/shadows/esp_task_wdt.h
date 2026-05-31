// Host shim for the ESP task watchdog — no-ops.
#pragma once
inline int esp_task_wdt_reset() { return 0; }
inline int esp_task_wdt_add(void*) { return 0; }
inline int esp_task_wdt_delete(void*) { return 0; }
