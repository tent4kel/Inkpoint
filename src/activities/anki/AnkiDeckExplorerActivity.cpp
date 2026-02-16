#include "AnkiDeckExplorerActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "anki/AnkiDeck.h"
#include "anki/AnkiSessionManager.h"
#include "anki/CsvParser.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr const char* DECK_INDEX_PATH = "/.ankix/deck_index.bin";
}

void AnkiDeckExplorerActivity::taskTrampoline(void* param) {
  auto* self = static_cast<AnkiDeckExplorerActivity*>(param);
  self->displayTaskLoop();
}

void AnkiDeckExplorerActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();
  decks.clear();
  selectorIndex = 0;
  scanning = false;
  statusMessage.clear();

  loadDeckIndex();

  // Update totalDue on session manager from cached data
  uint16_t totalDue = 0;
  for (const auto& d : decks) {
    totalDue += d.dueCount;
  }
  ANKI_SESSION.setTotalDue(totalDue);

  updateRequired = true;
  xTaskCreate(&AnkiDeckExplorerActivity::taskTrampoline, "DeckExplorer", 4096, this, 1, &displayTaskHandle);
}

void AnkiDeckExplorerActivity::onExit() {
  Activity::onExit();

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
  decks.clear();
}

void AnkiDeckExplorerActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void AnkiDeckExplorerActivity::loadDeckIndex() {
  if (!Storage.exists("/.ankix")) {
    Storage.mkdir("/.ankix");
  }

  FsFile file;
  if (!Storage.openFileForRead("ANK", DECK_INDEX_PATH, file)) {
    LOG_DBG("ANK", "No deck index cache, scan needed");
    return;
  }

  while (file.available() >= 4) {
    DeckInfo info;
    uint16_t pathLen;
    serialization::readPod(file, pathLen);
    if (pathLen == 0 || pathLen > 512) break;

    info.path.resize(pathLen);
    file.read(reinterpret_cast<uint8_t*>(&info.path[0]), pathLen);
    serialization::readPod(file, info.totalCards);
    serialization::readPod(file, info.dueCount);
    info.title = titleFromPath(info.path);
    decks.push_back(std::move(info));
  }
  file.close();

  LOG_DBG("ANK", "Loaded deck index: %zu decks", decks.size());
}

void AnkiDeckExplorerActivity::saveDeckIndex() {
  if (!Storage.exists("/.ankix")) {
    Storage.mkdir("/.ankix");
  }

  FsFile file;
  if (!Storage.openFileForWrite("ANK", DECK_INDEX_PATH, file)) {
    LOG_ERR("ANK", "Failed to save deck index");
    return;
  }

  for (const auto& d : decks) {
    uint16_t pathLen = static_cast<uint16_t>(d.path.size());
    serialization::writePod(file, pathLen);
    file.write(reinterpret_cast<const uint8_t*>(d.path.data()), pathLen);
    serialization::writePod(file, d.totalCards);
    serialization::writePod(file, d.dueCount);
  }
  file.close();

  LOG_DBG("ANK", "Saved deck index: %zu decks", decks.size());
}

void AnkiDeckExplorerActivity::scanDecks() {
  scanning = true;
  statusMessage = "Scanning...";
  updateRequired = true;

  decks.clear();

  FsFile dir = Storage.open("/anki");
  if (!dir || !dir.isDirectory()) {
    scanning = false;
    statusMessage = "No /anki folder";
    updateRequired = true;
    return;
  }

  char name[128];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (file.isDirectory()) continue;
    file.getName(name, sizeof(name));
    std::string filename(name);

    if (filename.size() > 4 && filename.substr(filename.size() - 4) == ".csv") {
      std::string path = "/anki/" + filename;
      DeckInfo info;
      info.path = path;
      info.title = titleFromPath(path);

      size_t due = AnkiDeck::countDueCards(path);
      info.dueCount = static_cast<uint16_t>(due);

      // Count total cards from the CSV (header row + data rows)
      std::vector<CsvRow> rows;
      if (CsvParser::parseFile(path, rows) && rows.size() > 1) {
        info.totalCards = static_cast<uint16_t>(rows.size() - 1);
      }

      decks.push_back(std::move(info));
    }
  }
  dir.close();

  sortByDueCount();
  saveDeckIndex();

  // Update totalDue on session manager
  uint16_t totalDue = 0;
  for (const auto& d : decks) {
    totalDue += d.dueCount;
  }
  ANKI_SESSION.setTotalDue(totalDue);

  scanning = false;
  char buf[32];
  snprintf(buf, sizeof(buf), "%zu decks", decks.size());
  statusMessage = buf;
  updateRequired = true;

  LOG_DBG("ANK", "Scan complete: %zu decks found", decks.size());
}

void AnkiDeckExplorerActivity::sortByDueCount() {
  std::sort(decks.begin(), decks.end(), [](const DeckInfo& a, const DeckInfo& b) {
    return a.dueCount > b.dueCount;
  });
}

std::string AnkiDeckExplorerActivity::titleFromPath(const std::string& path) {
  size_t lastSlash = path.find_last_of('/');
  std::string filename = (lastSlash != std::string::npos) ? path.substr(lastSlash + 1) : path;
  if (filename.size() > 4 && filename.substr(filename.size() - 4) == ".csv") {
    filename = filename.substr(0, filename.size() - 4);
  }
  return filename;
}

void AnkiDeckExplorerActivity::loop() {
  if (scanning) return;

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoBack();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!decks.empty() && selectorIndex >= 0 && selectorIndex < static_cast<int>(decks.size())) {
      onOpenDeck(decks[selectorIndex].path);
    }
    return;
  }

  // Left button = Scan
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    scanDecks();
    selectorIndex = 0;
    return;
  }

  // Right button = Sort
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    sortByDueCount();
    updateRequired = true;
    return;
  }

  if (!decks.empty()) {
    buttonNavigator.onNextRelease([this] {
      selectorIndex = ButtonNavigator::nextIndex(selectorIndex, decks.size());
      updateRequired = true;
    });

    buttonNavigator.onPreviousRelease([this] {
      selectorIndex = ButtonNavigator::previousIndex(selectorIndex, decks.size());
      updateRequired = true;
    });

    buttonNavigator.onNextContinuous([this] {
      int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false);
      selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, decks.size(), pageItems);
      updateRequired = true;
    });

    buttonNavigator.onPreviousContinuous([this] {
      int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false);
      selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, decks.size(), pageItems);
      updateRequired = true;
    });
  }
}

void AnkiDeckExplorerActivity::render() const {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  auto metrics = UITheme::getInstance().getMetrics();

  // Header
  std::string title = "Flashcards";
  if (!statusMessage.empty()) {
    title += " [" + statusMessage + "]";
  }
  char sessionBuf[32];
  snprintf(sessionBuf, sizeof(sessionBuf), " S%u", ANKI_SESSION.getSession());
  title += sessionBuf;
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title.c_str());

  // Button hints
  const auto labels = mappedInput.mapLabels("< Back", "Open", "Scan", "Sort");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (decks.empty()) {
    const char* msg = scanning ? "Scanning..." : "No decks. Press Scan.";
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, msg);
    renderer.displayBuffer();
    return;
  }

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, decks.size(), selectorIndex,
      [this](int index) { return decks[index].title; },
      nullptr, nullptr,
      [this](int index) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%u/%u", decks[index].dueCount, decks[index].totalCards);
        return std::string(buf);
      });

  renderer.displayBuffer();
}
