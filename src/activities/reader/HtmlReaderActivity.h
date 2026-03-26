#pragma once

#include <WebArticle.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <memory>
#include <vector>

#include "CrossPointSettings.h"
#include "activities/Activity.h"
#include <functional>

class Page;

/**
 * HtmlReaderActivity - Reader for HTML article files (e.g. saved Instapaper articles).
 *
 * Uses ChapterHtmlSlimParser (EPUB pipeline) for full hyphenation and justified text.
 * Mirrors MdReaderActivity in structure: FreeRTOS display task + section cache.
 */
class HtmlReaderActivity final : public Activity {
  std::unique_ptr<WebArticle> wa;
  std::function<void()> onBack;
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  int currentPage = 0;
  int totalPages = 0;
  int pagesUntilFullRefresh = 0;
  bool updateRequired = false;
  bool initialized = false;

  std::string sectionFilePath;
  std::vector<uint32_t> pageLut;

  int cachedFontId = 0;
  int cachedScreenMargin = 0;
  uint8_t cachedParagraphAlignment = CrossPointSettings::LEFT_ALIGN;

  bool showEndHints = false;
  bool showDeleteConfirm = false;

  std::function<void()> onDelete;
  std::function<void()> onAdvance;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();
  void renderContents(std::unique_ptr<Page> page, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  void renderStatusBar() const;

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
                              std::unique_ptr<WebArticle> wa, std::function<void()> onBack,
                              std::function<void()> onDelete = nullptr,
                              std::function<void()> onAdvance = nullptr)
      : Activity("HtmlReader", renderer, mappedInput), wa(std::move(wa)), onBack(std::move(onBack)),
        onDelete(std::move(onDelete)), onAdvance(std::move(onAdvance)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool isReaderActivity() const override { return true; }
};
