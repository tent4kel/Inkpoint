#pragma once

#include <Epub/Page.h>
#include <Markdown.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "anki/AnkiDeck.h"

class AnkiActivity final : public Activity {
 public:
  enum class State { DECK_SUMMARY, FRONT, BACK, SESSION_COMPLETE };

  explicit AnkiActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string csvPath,
                        const std::function<void()>& onGoBack)
      : Activity("Anki", renderer, mappedInput), csvPath(std::move(csvPath)), onGoBack(onGoBack) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool preventAutoSleep() override { return false; }
  bool isReaderActivity() const override { return true; }

 private:
  std::string csvPath;
  std::unique_ptr<AnkiDeck> deck;
  State state = State::DECK_SUMMARY;

  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  bool updateRequired = false;
  int pagesUntilFullRefresh = 0;

  // Rendered card pages
  std::vector<std::unique_ptr<Page>> cardPages;
  int currentCardPage = 0;
  int cardContentHeight = 0;  // Total rendered content height for vertical centering

  // Anki-specific font size (independent of reader settings)
  uint8_t ankiFontSize = 1;  // 0=Small, 1=Medium, 2=Large, 3=XL
  bool ankiPortrait = true;   // true=Portrait, false=Landscape CCW
  bool ankiSwapFrontBack = false;  // Show back side first when true
  bool longPressHandled = false;  // Prevent long press re-trigger
  int cachedFontId = 0;
  int cachedScreenMargin = 0;

  const std::function<void()> onGoBack;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();

  void renderScreen();
  void renderDeckSummary();
  void renderCardSide(const char* label);
  void renderSessionComplete();

  // Build rendered pages from a markdown string
  void buildCardPages(const std::string& mdText);

  // Anki-specific settings persistence
  void loadAnkiSettings();
  void saveAnkiSettings();
  void applyOrientation();
  int getFontIdForAnkiSize() const;
  void cycleFontSize();
  void toggleOrientation();

  // Get card content for the displayed side, respecting swap setting
  const std::string& frontContent() const { return ankiSwapFrontBack ? deck->currentCard()->back : deck->currentCard()->front; }
  const std::string& backContent() const { return ankiSwapFrontBack ? deck->currentCard()->front : deck->currentCard()->back; }
};
