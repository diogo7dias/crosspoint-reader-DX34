#pragma once
#include <BatteryMonitor.h>

#define BAT_GPIO0 0  // X4 battery voltage divider on ADC GPIO0

// freeink BatteryMonitor: X4 reads the GPIO0 ADC divider exactly as before (its
// ADC path + LiPo polynomial are byte-identical to the community monitor we used
// previously, so X4 readings are unchanged). On the X3 the explicit adcPin is
// ignored — readPercentage() checks BoardConfig::ACTIVE.batteryGauge first, which
// setDisplayX3() populates at runtime, so the X3 reads State-of-Charge from its
// BQ27220 fuel gauge over I2C instead of the unconnected ADC that returned 0%.
static BatteryMonitor battery(BAT_GPIO0);
