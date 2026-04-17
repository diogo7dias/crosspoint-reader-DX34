#pragma once
// Stub — DX34 BleHidManager.h includes freertos/portmacro.h for critical-section
// macros. Simulator has no BLE or ISR context, but the macros below must be
// defined so the header compiles.
#include "FreeRTOS.h"

#ifndef portMUX_INITIALIZER_UNLOCKED
#define portMUX_INITIALIZER_UNLOCKED {}
#endif

typedef struct { int dummy; } portMUX_TYPE;

#ifndef portENTER_CRITICAL
#define portENTER_CRITICAL(mux) do { (void)(mux); } while (0)
#endif
#ifndef portEXIT_CRITICAL
#define portEXIT_CRITICAL(mux) do { (void)(mux); } while (0)
#endif
