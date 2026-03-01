#pragma once

#include <WebArticle.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <memory>
#include <vector>

#include "CrossPointSettings.h"
#include "activities/ActivityWithSubactivity.h"

class Page;

/**
 * HtmlReaderActivity - Reader for HTML article files (e.g. saved Instapaper articles).
 *
 * Uses ChapterHtmlSlimParser (EPUB pipeline) for full hyphenation and justified text.
 * Mirrors MdReaderActivity in structure: FreeRTOS display task + section cache.
 */
class HtmlReaderActivity final : public ActivityWithSubactivity {
  std::unique_ptr<WebArticle> wa;
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  int currentPage = 0;
  int totalPages = 0;
  int pagesUntilFullRefresh = 0;
  bool updateRequired = false;
  bool initialized = false;
  bool endActionsVisible = false;
  const std::function<void()> onGoBack;
  const std::function<void()> onGoHome;
  const std::function<void()> onAdvanceArticle;    // nullable: last page + next → open next article
  const std::function<void()> onDeleteAndAdvance;  // nullable: last page + confirm → delete + open next

  std::string sectionFilePath;
  std::vector<uint32_t> pageLut;

  int cachedFontId = 0;
  int cachedScreenMargin = 0;
  uint8_t cachedParagraphAlignment = CrossPointSettings::LEFT_ALIGN;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();
  void renderContents(std::unique_ptr<Page> page, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  void renderStatusBar(int orientedMarginRight, int orientedMarginBottom, int orientedMarginLeft);

  void initializeReader();
  bool loadSectionCache(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                        uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled);
  bool createSectionCache(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                          uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled);
  std::unique_ptr<Page> loadPageFromCache(int pageIndex);
  void saveProgress() const;
  void loadProgress();

 public:
  explicit HtmlReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                              std::unique_ptr<WebArticle> wa, const std::function<void()>& onGoBack,
                              const std::function<void()>& onGoHome,
                              std::function<void()> onAdvanceArticle = nullptr,
                              std::function<void()> onDeleteAndAdvance = nullptr)
      : ActivityWithSubactivity("HtmlReader", renderer, mappedInput),
        wa(std::move(wa)),
        onGoBack(onGoBack),
        onGoHome(onGoHome),
        onAdvanceArticle(std::move(onAdvanceArticle)),
        onDeleteAndAdvance(std::move(onDeleteAndAdvance)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool isReaderActivity() const override { return true; }
};
