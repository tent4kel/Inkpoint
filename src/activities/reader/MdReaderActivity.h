#pragma once

#include <Markdown.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <memory>
#include <vector>

#include "CrossPointSettings.h"
#include "activities/ActivityWithSubactivity.h"

class Page;

/**
 * MdReaderActivity - Reader for Markdown files with styled rendering.
 *
 * Follows the same structure as TxtReaderActivity (FreeRTOS display task,
 * input handling, status bar) but renders using cached Pages built by
 * MarkdownParser instead of plain text lines. Single "section" (the whole file).
 */
class MdReaderActivity final : public ActivityWithSubactivity {
  std::unique_ptr<Markdown> md;
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  int currentPage = 0;
  int totalPages = 0;
  int pagesUntilFullRefresh = 0;
  bool updateRequired = false;
  bool initialized = false;
  const std::function<void()> onGoBack;
  const std::function<void()> onGoHome;

  // Cache file for rendered pages (section.bin style)
  std::string sectionFilePath;
  std::vector<uint32_t> pageLut;  // Page offsets in section file

  // Cached settings for cache validation
  int cachedFontId = 0;
  int cachedScreenMargin = 0;
  uint8_t cachedParagraphAlignment = CrossPointSettings::LEFT_ALIGN;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();
  void renderContents(std::unique_ptr<Page> page, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  void renderStatusBar(int orientedMarginRight, int orientedMarginBottom, int orientedMarginLeft) const;

  void initializeReader();
  bool loadSectionCache(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                        uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled);
  bool createSectionCache(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                          uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled);
  std::unique_ptr<Page> loadPageFromCache(int pageIndex);
  void saveProgress() const;
  void loadProgress();

 public:
  explicit MdReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Markdown> md,
                            const std::function<void()>& onGoBack, const std::function<void()>& onGoHome)
      : ActivityWithSubactivity("MdReader", renderer, mappedInput),
        md(std::move(md)),
        onGoBack(onGoBack),
        onGoHome(onGoHome) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
