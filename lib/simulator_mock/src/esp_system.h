#pragma once
#include <cstdint>
#include <cstdlib>
inline void esp_restart() {}
inline uint32_t esp_get_free_heap_size() { return 1000000; }
inline uint32_t esp_random() { return (uint32_t)rand(); }