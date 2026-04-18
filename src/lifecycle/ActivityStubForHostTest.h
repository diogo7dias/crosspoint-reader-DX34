#pragma once

// Minimal host-test stub for Activity. Only defines the surface the
// lifecycle::ActivityRouter uses: virtual onExit() and a virtual destructor
// (so `delete slot;` on an Activity* pointer calls the subclass destructor).
//
// Do NOT include this in any device-target translation unit. It is only
// picked up when ActivityRouter.cpp is compiled with -DUNIT_TEST_HOST=1
// under the [env:test_host] PlatformIO environment.

class Activity {
 public:
  virtual ~Activity() = default;
  virtual void onExit() {}
};
