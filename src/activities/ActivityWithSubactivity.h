#pragma once
#include <memory>

#include "Activity.h"

class ActivityWithSubactivity : public Activity {
 protected:
  std::unique_ptr<Activity> subActivity = nullptr;
  void exitActivity();
  void enterNewActivity(Activity* activity);
  [[noreturn]] void renderTaskLoop() override;
  // Hook fired when the OOM screen swapped in by enterNewActivity (after a
  // subactivity entry failure) is dismissed by the user. Default behavior
  // just tears down the OOM screen and restores the parent render task,
  // which leaves the user staring at a blank parent — fine for trivial
  // parents but a dead-end for ReaderActivity (no chrome of its own).
  // Subclasses override to route the user somewhere navigable (library,
  // home, etc.) after the dismissal.
  virtual void onSubactivityEntryFailedFatally() { exitActivity(); }

 public:
  explicit ActivityWithSubactivity(std::string name, GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity(std::move(name), renderer, mappedInput) {}
  void loop() override;
  // Note: when a subactivity is active, parent requestUpdate() calls are ignored;
  // the subactivity should request its own renders. This pauses parent rendering until exit.
  void requestUpdate() override;
  void onExit() override;
};
