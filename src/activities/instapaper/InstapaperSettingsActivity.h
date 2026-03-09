#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

class InstapaperSettingsActivity final : public Activity {
 public:
  explicit InstapaperSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("InstapaperSettings", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;

  int selectedIndex = 0;
  std::string statusMessage;
  std::string pendingUsername;
  std::string pendingPassword;

  void handleSelection();
};
