#pragma once
#include "network/InstapaperClient.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <vector>

#include "../ActivityWithSubactivity.h"
#include "util/ButtonNavigator.h"

struct DisplayBookmark {
  std::string title;
  std::string bookmarkId;  // Empty if only known from SD (no API match)
  std::string url;          // Original article URL (for language detection)
  std::string filename;     // Actual filename on SD (e.g., "Title.de.md")
  long time = 0;            // Unix timestamp when added (for sorting)
  bool downloaded;          // File exists on SD
};

class InstapaperActivity final : public ActivityWithSubactivity {
 public:
  enum class State {
    BROWSING,
    DOWNLOADING,
    ERROR,
  };

  explicit InstapaperActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                              const std::function<void()>& onGoHome,
                              const std::function<void(const std::string&)>& onOpenBook)
      : ActivityWithSubactivity("Instapaper", renderer, mappedInput), onGoHome(onGoHome), onOpenBook(onOpenBook) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  TaskHandle_t displayTaskHandle = nullptr;
  TaskHandle_t syncTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  ButtonNavigator buttonNavigator;
  bool updateRequired = false;

  State state = State::BROWSING;
  std::vector<DisplayBookmark> displayList;
  int selectorIndex = 0;
  std::string errorMessage;
  std::string statusMessage;
  int downloadCurrent = 0;
  int downloadTotal = 0;

  bool syncing = false;
  bool syncComplete = false;
  std::string syncStatus;

  const std::function<void()> onGoHome;
  const std::function<void(const std::string&)> onOpenBook;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;

  void loadCachedArticles();
  void loadBookmarkCache();
  void saveBookmarkCache();
  static void syncTaskTrampoline(void* param);
  void startBackgroundSync();
  void backgroundSyncWork();

  void openArticle(int index);
  void deleteArticle(int index);
  void downloadNewest();
  void downloadSingleArticle(DisplayBookmark& bm);
  std::string getArticlePath(const DisplayBookmark& bm) const;
  bool preventAutoSleep() override { return syncing || state == State::DOWNLOADING; }
};
