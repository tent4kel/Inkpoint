#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class AnkiSettingsActivity final : public Activity {
 public:
  explicit AnkiSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("AnkiSettings", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  size_t selectedIndex = 0;

  void handleSelection();
};
