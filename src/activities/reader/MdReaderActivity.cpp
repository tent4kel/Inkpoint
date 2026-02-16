#include "MdReaderActivity.h"

#include <Epub/Page.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <MarkdownParser.h>
#include <Serialization.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr unsigned long goHomeMs = 1000;
constexpr int statusBarMargin = 25;
constexpr int progressBarMarginTop = 1;

// Section cache file format
constexpr uint8_t SECTION_FILE_VERSION = 1;
constexpr uint32_t HEADER_SIZE = sizeof(uint8_t) + sizeof(int) + sizeof(float) + sizeof(bool) + sizeof(uint8_t) +
                                 sizeof(uint16_t) + sizeof(uint16_t) + sizeof(bool) + sizeof(uint16_t) +
                                 sizeof(uint32_t);
}  // namespace

void MdReaderActivity::taskTrampoline(void* param) {
  auto* self = static_cast<MdReaderActivity*>(param);
  self->displayTaskLoop();
}

void MdReaderActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  if (!md) {
    return;
  }

  // Configure screen orientation based on settings
  switch (SETTINGS.orientation) {
    case CrossPointSettings::ORIENTATION::PORTRAIT:
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
      break;
    case CrossPointSettings::ORIENTATION::INVERTED:
      renderer.setOrientation(GfxRenderer::Orientation::PortraitInverted);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
      break;
    default:
      break;
  }

  renderingMutex = xSemaphoreCreateMutex();

  md->setupCacheDir();
  sectionFilePath = md->getCachePath() + "/section.bin";

  // Save current file as last opened and add to recent books
  auto filePath = md->getPath();
  auto fileName = filePath.substr(filePath.rfind('/') + 1);
  APP_STATE.openEpubPath = filePath;
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(filePath, fileName, "", "");

  // Trigger first update
  updateRequired = true;

  xTaskCreate(&MdReaderActivity::taskTrampoline, "MdReaderActivityTask",
              8192,               // Stack size (larger than TXT due to page rendering)
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void MdReaderActivity::onExit() {
  ActivityWithSubactivity::onExit();

  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
  pageLut.clear();
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  md.reset();
}

void MdReaderActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= goHomeMs) {
    onGoBack();
    return;
  }

  // Short press BACK goes directly to home
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && mappedInput.getHeldTime() < goHomeMs) {
    onGoHome();
    return;
  }

  // Page turn handling
  const bool usePressForPageTurn = !SETTINGS.longPressChapterSkip;
  const bool prevTriggered = usePressForPageTurn ? (mappedInput.wasPressed(MappedInputManager::Button::PageBack) ||
                                                    mappedInput.wasPressed(MappedInputManager::Button::Left))
                                                 : (mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
                                                    mappedInput.wasReleased(MappedInputManager::Button::Left));
  const bool powerPageTurn = SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN &&
                             mappedInput.wasReleased(MappedInputManager::Button::Power);
  const bool nextTriggered = usePressForPageTurn
                                 ? (mappedInput.wasPressed(MappedInputManager::Button::PageForward) || powerPageTurn ||
                                    mappedInput.wasPressed(MappedInputManager::Button::Right))
                                 : (mappedInput.wasReleased(MappedInputManager::Button::PageForward) || powerPageTurn ||
                                    mappedInput.wasReleased(MappedInputManager::Button::Right));

  if (!prevTriggered && !nextTriggered) {
    return;
  }

  if (prevTriggered && currentPage > 0) {
    currentPage--;
    updateRequired = true;
  } else if (nextTriggered && currentPage < totalPages - 1) {
    currentPage++;
    updateRequired = true;
  }
}

void MdReaderActivity::displayTaskLoop() {
  // Initialize reader WITHOUT holding the mutex so that onExit() can still
  // acquire the mutex and vTaskDelete this task if the user presses Back
  // during a long cache build.
  if (!initialized) {
    initializeReader();
    updateRequired = true;
  }

  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      renderScreen();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void MdReaderActivity::initializeReader() {
  if (initialized) {
    return;
  }

  cachedFontId = SETTINGS.getReaderFontId();
  cachedScreenMargin = SETTINGS.screenMargin;
  cachedParagraphAlignment = SETTINGS.paragraphAlignment;

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += cachedScreenMargin;
  orientedMarginLeft += cachedScreenMargin;
  orientedMarginRight += cachedScreenMargin;
  orientedMarginBottom += cachedScreenMargin;

  auto metrics = UITheme::getInstance().getMetrics();

  if (SETTINGS.statusBar != CrossPointSettings::STATUS_BAR_MODE::NONE) {
    const bool showProgressBar = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                                 SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::ONLY_BOOK_PROGRESS_BAR ||
                                 SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
    orientedMarginBottom += statusBarMargin - cachedScreenMargin +
                            (showProgressBar ? (metrics.bookProgressBarHeight + progressBarMarginTop) : 0);
  }

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;

  const float lineCompression = SETTINGS.getReaderLineCompression();
  const bool extraParagraphSpacing = SETTINGS.extraParagraphSpacing;
  const bool hyphenationEnabled = SETTINGS.hyphenationEnabled;

  if (!loadSectionCache(cachedFontId, lineCompression, extraParagraphSpacing, cachedParagraphAlignment, viewportWidth,
                        viewportHeight, hyphenationEnabled)) {
    LOG_DBG("MDR", "Cache not found, building...");
    if (!createSectionCache(cachedFontId, lineCompression, extraParagraphSpacing, cachedParagraphAlignment,
                            viewportWidth, viewportHeight, hyphenationEnabled)) {
      LOG_ERR("MDR", "Failed to build section cache");
      initialized = true;
      return;
    }
  } else {
    LOG_DBG("MDR", "Cache found, %d pages", totalPages);
  }

  loadProgress();
  initialized = true;
}

bool MdReaderActivity::loadSectionCache(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                        const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                        const uint16_t viewportHeight, const bool hyphenationEnabled) {
  FsFile file;
  if (!Storage.openFileForRead("MDR", sectionFilePath, file)) {
    return false;
  }

  // Read and validate header
  uint8_t version;
  serialization::readPod(file, version);
  if (version != SECTION_FILE_VERSION) {
    file.close();
    LOG_DBG("MDR", "Cache version mismatch (%d != %d)", version, SECTION_FILE_VERSION);
    return false;
  }

  int fileFontId;
  float fileLineCompression;
  bool fileExtraParagraphSpacing;
  uint8_t fileParagraphAlignment;
  uint16_t fileViewportWidth, fileViewportHeight;
  bool fileHyphenationEnabled;
  serialization::readPod(file, fileFontId);
  serialization::readPod(file, fileLineCompression);
  serialization::readPod(file, fileExtraParagraphSpacing);
  serialization::readPod(file, fileParagraphAlignment);
  serialization::readPod(file, fileViewportWidth);
  serialization::readPod(file, fileViewportHeight);
  serialization::readPod(file, fileHyphenationEnabled);

  if (fontId != fileFontId || lineCompression != fileLineCompression ||
      extraParagraphSpacing != fileExtraParagraphSpacing || paragraphAlignment != fileParagraphAlignment ||
      viewportWidth != fileViewportWidth || viewportHeight != fileViewportHeight ||
      hyphenationEnabled != fileHyphenationEnabled) {
    file.close();
    LOG_DBG("MDR", "Cache parameters mismatch, rebuilding");
    Storage.remove(sectionFilePath.c_str());
    return false;
  }

  uint16_t pageCount;
  serialization::readPod(file, pageCount);
  totalPages = pageCount;

  uint32_t lutOffset;
  serialization::readPod(file, lutOffset);

  // Read LUT
  pageLut.clear();
  pageLut.reserve(totalPages);
  file.seek(lutOffset);
  for (int i = 0; i < totalPages; i++) {
    uint32_t pos;
    serialization::readPod(file, pos);
    pageLut.push_back(pos);
  }

  file.close();
  return true;
}

bool MdReaderActivity::createSectionCache(const int fontId, const float lineCompression,
                                          const bool extraParagraphSpacing, const uint8_t paragraphAlignment,
                                          const uint16_t viewportWidth, const uint16_t viewportHeight,
                                          const bool hyphenationEnabled) {
  FsFile file;
  if (!Storage.openFileForWrite("MDR", sectionFilePath, file)) {
    LOG_ERR("MDR", "Failed to open section file for writing");
    return false;
  }

  // Write header
  serialization::writePod(file, SECTION_FILE_VERSION);
  serialization::writePod(file, fontId);
  serialization::writePod(file, lineCompression);
  serialization::writePod(file, extraParagraphSpacing);
  serialization::writePod(file, paragraphAlignment);
  serialization::writePod(file, viewportWidth);
  serialization::writePod(file, viewportHeight);
  serialization::writePod(file, hyphenationEnabled);
  serialization::writePod(file, static_cast<uint16_t>(0));   // placeholder for page count
  serialization::writePod(file, static_cast<uint32_t>(0));   // placeholder for LUT offset

  pageLut.clear();
  uint16_t pageCount = 0;

  // Ensure hyphenator has a language set (EPUB sets this per-section, MD needs it too)
  if (hyphenationEnabled) {
    Hyphenator::setPreferredLanguage(md->getLanguage());
  }

  MarkdownParser parser(
      *md, renderer, fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth, viewportHeight,
      hyphenationEnabled,
      [&file, &pageCount, this](std::unique_ptr<Page> page) {
        const uint32_t position = file.position();
        if (page->serialize(file)) {
          pageLut.push_back(position);
          pageCount++;
          LOG_DBG("MDR", "Page %d processed", pageCount);
        }
      },
      [this]() { GUI.drawPopup(renderer, "Indexing..."); });

  if (!parser.parseAndBuildPages()) {
    LOG_ERR("MDR", "Failed to parse markdown");
    file.close();
    Storage.remove(sectionFilePath.c_str());
    return false;
  }

  // Write LUT
  const uint32_t lutOffset = file.position();
  for (const uint32_t& pos : pageLut) {
    serialization::writePod(file, pos);
  }

  // Go back and write page count and LUT offset
  file.seek(HEADER_SIZE - sizeof(uint32_t) - sizeof(uint16_t));
  serialization::writePod(file, pageCount);
  serialization::writePod(file, lutOffset);
  file.close();

  totalPages = pageCount;
  LOG_DBG("MDR", "Built section cache: %d pages", totalPages);
  return true;
}

std::unique_ptr<Page> MdReaderActivity::loadPageFromCache(int pageIndex) {
  if (pageIndex < 0 || pageIndex >= static_cast<int>(pageLut.size())) {
    return nullptr;
  }

  FsFile file;
  if (!Storage.openFileForRead("MDR", sectionFilePath, file)) {
    return nullptr;
  }

  file.seek(pageLut[pageIndex]);
  auto page = Page::deserialize(file);
  file.close();
  return page;
}

void MdReaderActivity::renderScreen() {
  if (!md) {
    return;
  }

  if (totalPages == 0) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, "Empty file", true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (currentPage < 0) currentPage = 0;
  if (currentPage >= totalPages) currentPage = totalPages - 1;

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += cachedScreenMargin;
  orientedMarginLeft += cachedScreenMargin;
  orientedMarginRight += cachedScreenMargin;
  orientedMarginBottom += cachedScreenMargin;

  auto metrics = UITheme::getInstance().getMetrics();
  if (SETTINGS.statusBar != CrossPointSettings::STATUS_BAR_MODE::NONE) {
    const bool showProgressBar = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                                 SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::ONLY_BOOK_PROGRESS_BAR ||
                                 SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
    orientedMarginBottom += statusBarMargin - cachedScreenMargin +
                            (showProgressBar ? (metrics.bookProgressBarHeight + progressBarMarginTop) : 0);
  }

  auto page = loadPageFromCache(currentPage);
  if (!page) {
    LOG_ERR("MDR", "Failed to load page %d from cache", currentPage);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, "Page load error", true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  renderer.clearScreen();
  renderContents(std::move(page), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
  saveProgress();
}

void MdReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                      const int orientedMarginRight, const int orientedMarginBottom,
                                      const int orientedMarginLeft) {
  page->render(renderer, cachedFontId, orientedMarginLeft, orientedMarginTop);
  renderStatusBar(orientedMarginRight, orientedMarginBottom, orientedMarginLeft);

  if (pagesUntilFullRefresh <= 1) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    renderer.displayBuffer();
    pagesUntilFullRefresh--;
  }

  // Grayscale anti-aliasing pass
  if (SETTINGS.textAntiAliasing) {
    renderer.storeBwBuffer();

    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    page->render(renderer, cachedFontId, orientedMarginLeft, orientedMarginTop);
    renderer.copyGrayscaleLsbBuffers();

    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    page->render(renderer, cachedFontId, orientedMarginLeft, orientedMarginTop);
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);

    renderer.restoreBwBuffer();
  }
}

void MdReaderActivity::renderStatusBar(const int orientedMarginRight, const int orientedMarginBottom,
                                       const int orientedMarginLeft) const {
  const bool showProgressPercentage = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL;
  const bool showProgressBar = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                               SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::ONLY_BOOK_PROGRESS_BAR;
  const bool showChapterProgressBar = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
  const bool showProgressText = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL ||
                                SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR;
  const bool showBookPercentage = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
  const bool showBattery = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::NO_PROGRESS ||
                           SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL ||
                           SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                           SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
  const bool showTitle = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::NO_PROGRESS ||
                         SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL ||
                         SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                         SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage == CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_NEVER;

  auto metrics = UITheme::getInstance().getMetrics();
  const auto screenHeight = renderer.getScreenHeight();
  const auto textY = screenHeight - orientedMarginBottom - 4;
  int progressTextWidth = 0;

  const float progress = totalPages > 0 ? (currentPage + 1) * 100.0f / totalPages : 0;

  if (showProgressText || showProgressPercentage || showBookPercentage) {
    char progressStr[32];
    if (showProgressPercentage) {
      snprintf(progressStr, sizeof(progressStr), "%d/%d %.0f%%", currentPage + 1, totalPages, progress);
    } else if (showBookPercentage) {
      snprintf(progressStr, sizeof(progressStr), "%.0f%%", progress);
    } else {
      snprintf(progressStr, sizeof(progressStr), "%d/%d", currentPage + 1, totalPages);
    }

    progressTextWidth = renderer.getTextWidth(SMALL_FONT_ID, progressStr);
    renderer.drawText(SMALL_FONT_ID, renderer.getScreenWidth() - orientedMarginRight - progressTextWidth, textY,
                      progressStr);
  }

  if (showProgressBar || showChapterProgressBar) {
    GUI.drawReadingProgressBar(renderer, static_cast<size_t>(progress));
  }

  if (showBattery) {
    GUI.drawBatteryLeft(renderer, Rect{orientedMarginLeft, textY, metrics.batteryWidth, metrics.batteryHeight},
                    showBatteryPercentage);
  }

  if (showTitle) {
    const int titleMarginLeft = 50 + 30 + orientedMarginLeft;
    const int titleMarginRight = progressTextWidth + 30 + orientedMarginRight;
    const int availableTextWidth = renderer.getScreenWidth() - titleMarginLeft - titleMarginRight;

    std::string title = md->getTitle();
    int titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
    if (titleWidth > availableTextWidth) {
      title = renderer.truncatedText(SMALL_FONT_ID, title.c_str(), availableTextWidth);
      titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
    }

    renderer.drawText(SMALL_FONT_ID, titleMarginLeft + (availableTextWidth - titleWidth) / 2, textY, title.c_str());
  }
}

void MdReaderActivity::saveProgress() const {
  FsFile f;
  if (Storage.openFileForWrite("MDR", md->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    data[0] = currentPage & 0xFF;
    data[1] = (currentPage >> 8) & 0xFF;
    data[2] = 0;
    data[3] = 0;
    f.write(data, 4);
    f.close();
  }
}

void MdReaderActivity::loadProgress() {
  FsFile f;
  if (Storage.openFileForRead("MDR", md->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      currentPage = data[0] + (data[1] << 8);
      if (currentPage >= totalPages) {
        currentPage = totalPages - 1;
      }
      if (currentPage < 0) {
        currentPage = 0;
      }
      LOG_DBG("MDR", "Loaded progress: page %d/%d", currentPage, totalPages);
    }
    f.close();
  }
}
