#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>

#include "activities/ActivityWithSubactivity.h"
#include "util/ButtonNavigator.h"

class InstapaperSettingsActivity final : public ActivityWithSubactivity {
 public:
  explicit InstapaperSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                      const std::function<void()>& onBack)
      : ActivityWithSubactivity("InstapaperSettings", renderer, mappedInput), onBack(onBack) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  ButtonNavigator buttonNavigator;
  bool updateRequired = false;

  int selectedIndex = 0;
  std::string statusMessage;
  std::string pendingUsername;
  std::string pendingPassword;
  const std::function<void()> onBack;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render();
  void handleSelection();
};
