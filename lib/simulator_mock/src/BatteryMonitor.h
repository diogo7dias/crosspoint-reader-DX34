#pragma once
class BatteryMonitor {
 public:
  BatteryMonitor(int pin, int factor = -1) {}
  void begin() {}
  int getVoltage() { return 4200; }
  int getPercentage() { return 100; }
  int readPercentage() { return 100; }
  int readVoltage() { return 4200; }
};
