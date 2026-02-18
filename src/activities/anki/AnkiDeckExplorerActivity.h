#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

struct DeckInfo {
  std::string path;
  std::string title;
  uint16_t totalCards = 0;
  uint16_t dueCount = 0;
  uint32_t lastOpened = 0;  // globalSession value when deck was last opened
};

class AnkiDeckExplorerActivity final : public Activity {
 public:
  explicit AnkiDeckExplorerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                     const std::function<void()>& onGoBack,
                                     const std::function<void(const std::string&)>& onOpenDeck)
      : Activity("AnkiExplorer", renderer, mappedInput), onGoBack(onGoBack), onOpenDeck(onOpenDeck) {}

  enum class SortMode { DUE, LAST_OPENED, NAME };

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  ButtonNavigator buttonNavigator;
  bool updateRequired = false;

  std::vector<DeckInfo> decks;
  int selectorIndex = 0;
  bool scanning = false;
  std::string statusMessage;
  SortMode sortMode = SortMode::DUE;

  const std::function<void()> onGoBack;
  const std::function<void(const std::string&)> onOpenDeck;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;

  void loadDeckIndex();
  void saveDeckIndex();
  void scanDecks();
  void sortDecks();
  void refreshDueCounts();

  static std::string titleFromPath(const std::string& path);
  const char* sortModeLabel() const;
};
