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
  std::string source;       // Domain e.g. "spiegel.de"
  std::string filename;     // Actual filename on SD (e.g., "Title.html")
  long time = 0;            // Unix timestamp when added (for sorting)
  bool downloaded  = false; // File exists on SD
  bool queued      = false; // In queue, not yet active
  bool downloading = false; // Actively downloading
  int  dlCurrent   = 0;     // Bytes received (active only)
  int  dlTotal     = 0;     // Total bytes (0 = unknown)
};

class InstapaperActivity final : public ActivityWithSubactivity {
 public:
  enum class State {
    BROWSING,
    ERROR,
  };

  explicit InstapaperActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                              const std::function<void()>& onGoHome,
                              const std::function<void()>& onGoToSelf,
                              const std::function<void(const std::string&, std::function<void()>, std::function<void()>)>& onOpenBook)
      : ActivityWithSubactivity("Instapaper", renderer, mappedInput),
        onGoHome(onGoHome), onGoToSelf(onGoToSelf), onOpenBook(onOpenBook) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  TaskHandle_t displayTaskHandle = nullptr;
  TaskHandle_t syncTaskHandle = nullptr;
  TaskHandle_t downloadTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  ButtonNavigator buttonNavigator;
  bool updateRequired = false;

  State state = State::BROWSING;
  std::vector<DisplayBookmark> displayList;
  int selectorIndex = 0;
  std::string errorMessage;

  std::vector<int> downloadQueue;       // ordered displayList indices to download
  volatile int activeDownloadIdx = -1;  // displayList index being downloaded (-1 = none)
  volatile bool abortDownload = false;
  bool showStopModal = false;           // "stop downloads + open offline?" overlay
  int pendingOpenIdx = -1;              // index to open when user confirms modal

  bool syncing = false;
  bool syncComplete = false;
  std::string syncStatus;

  const std::function<void()> onGoHome;
  const std::function<void()> onGoToSelf;
  const std::function<void(const std::string&, std::function<void()>, std::function<void()>)> onOpenBook;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;

  void loadCachedArticles();
  void loadBookmarkCache();
  void saveBookmarkCache();
  void saveQueueFile();
  void loadQueueFile();
  static void syncTaskTrampoline(void* param);
  void startBackgroundSync();
  void backgroundSyncWork();

  static void downloadTaskTrampoline(void* param);
  void backgroundDownloadWork();
  bool ensureWifiAndNtp();

  void openArticle(int index);
  void deleteArticle(int index);
  void toggleQueue(int index);
  void queueNewest(int n);
  void downloadSingleArticle(DisplayBookmark& bm, HttpDownloader::ProgressCallback progress = nullptr,
                              std::function<bool()> abortCheck = nullptr);
  std::string getArticlePath(const DisplayBookmark& bm) const;
  bool preventAutoSleep() override { return syncing || activeDownloadIdx >= 0; }
};
