#include "InstapaperActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <HtmlToMarkdown.h>
#include <WiFi.h>

#include <algorithm>

#include "InstapaperCredentialStore.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/StringUtils.h"

namespace {

// Extract language code from URL's country-code TLD (e.g., ".de" → "de")
std::string languageFromUrl(const std::string& url) {
  // Map of ccTLDs to hyphenation language codes
  struct TldLang { const char* tld; const char* lang; };
  static const TldLang mappings[] = {
    {".de/", "de"}, {".at/", "de"}, {".ch/", "de"},
    {".fr/", "fr"}, {".be/", "fr"},
    {".es/", "es"}, {".mx/", "es"}, {".ar/", "es"},
    {".it/", "it"},
    {".ru/", "ru"},
  };

  // Find the host portion: after "://" and before first "/"
  size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos) return "en";
  size_t hostStart = schemeEnd + 3;
  size_t hostEnd = url.find('/', hostStart);
  if (hostEnd == std::string::npos) hostEnd = url.size();
  std::string host = url.substr(hostStart, hostEnd - hostStart) + "/";

  for (const auto& m : mappings) {
    // Check if host ends with the TLD pattern (e.g., "example.de/")
    std::string tld(m.tld);
    if (host.size() >= tld.size() && host.compare(host.size() - tld.size(), tld.size(), tld) == 0) {
      return m.lang;
    }
  }
  return "en";
}

}  // namespace

void InstapaperActivity::taskTrampoline(void* param) {
  auto* self = static_cast<InstapaperActivity*>(param);
  self->displayTaskLoop();
}

void InstapaperActivity::syncTaskTrampoline(void* param) {
  auto* self = static_cast<InstapaperActivity*>(param);
  self->backgroundSyncWork();
  self->syncing = false;
  self->syncTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

void InstapaperActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();
  displayList.clear();
  selectorIndex = 0;
  errorMessage.clear();
  statusMessage.clear();
  syncing = false;
  syncComplete = false;
  syncStatus.clear();

  loadCachedArticles();
  loadBookmarkCache();

  // Sort immediately so the initial display matches the post-sync order
  std::sort(displayList.begin(), displayList.end(), [](const DisplayBookmark& a, const DisplayBookmark& b) {
    if (a.time == 0 && b.time == 0) return false;
    if (a.time == 0) return false;
    if (b.time == 0) return true;
    return a.time > b.time;
  });

  state = State::BROWSING;
  updateRequired = true;

  xTaskCreate(&InstapaperActivity::taskTrampoline, "InstapaperTask", 4096, this, 1, &displayTaskHandle);

  startBackgroundSync();
}

void InstapaperActivity::onExit() {
  ActivityWithSubactivity::onExit();

  xTaskCreate(
      [](void*) {
        vTaskDelay(60000 / portTICK_PERIOD_MS);
        if (WiFi.status() == WL_CONNECTED) {
          WiFi.disconnect(false);
          delay(100);
          WiFi.mode(WIFI_OFF);
        }
        vTaskDelete(nullptr);
      },
      "WiFiOff", 2048, nullptr, 0, nullptr);

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (syncTaskHandle) {
    vTaskDelete(syncTaskHandle);
    syncTaskHandle = nullptr;
    syncing = false;
  }
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
  displayList.clear();
}

void InstapaperActivity::loadCachedArticles() {
  const auto& folder = INSTAPAPER_STORE.getDownloadFolder();
  FsFile dir = Storage.open(folder.c_str());
  if (!dir || !dir.isDirectory()) return;

  char name[128];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (file.isDirectory()) continue;
    file.getName(name, sizeof(name));
    std::string filename(name);

    if (filename.size() > 3 && filename.substr(filename.size() - 3) == ".md") {
      DisplayBookmark bm;
      bm.filename = filename;
      std::string base = filename.substr(0, filename.size() - 3);
      // Strip language code from "Title.xx" pattern
      if (base.size() > 3 && base[base.size() - 3] == '.' &&
          base[base.size() - 2] >= 'a' && base[base.size() - 2] <= 'z' &&
          base[base.size() - 1] >= 'a' && base[base.size() - 1] <= 'z') {
        bm.title = base.substr(0, base.size() - 3);
      } else {
        bm.title = base;
      }
      bm.downloaded = true;
      displayList.push_back(std::move(bm));
    }
  }
  dir.close();

  LOG_DBG("INS", "Loaded %d cached articles from SD", displayList.size());
}

void InstapaperActivity::loadBookmarkCache() {
  const auto& folder = INSTAPAPER_STORE.getDownloadFolder();
  std::string cachePath = folder + "/.bookmarks";

  FsFile file;
  if (!Storage.openFileForRead("INS", cachePath, file)) return;

  char buf[256];
  std::string line;
  while (file.available()) {
    int bytesRead = file.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf) - 1);
    if (bytesRead <= 0) break;
    buf[bytesRead] = '\0';

    for (int i = 0; i < bytesRead; i++) {
      if (buf[i] == '\n') {
        // Parse line: bookmarkId|title|url|time
        size_t sep = line.find('|');
        if (sep != std::string::npos) {
          std::string bmId = line.substr(0, sep);
          std::string rest = line.substr(sep + 1);
          std::string title, bmUrl;
          long bmTime = 0;
          size_t sep2 = rest.find('|');
          if (sep2 != std::string::npos) {
            title = rest.substr(0, sep2);
            std::string rest2 = rest.substr(sep2 + 1);
            size_t sep3 = rest2.find('|');
            if (sep3 != std::string::npos) {
              bmUrl = rest2.substr(0, sep3);
              bmTime = atol(rest2.substr(sep3 + 1).c_str());
            } else {
              bmUrl = rest2;
            }
          } else {
            title = rest;
          }

          // Check if already in list (from SD scan)
          bool found = false;
          for (auto& existing : displayList) {
            if (existing.title == title) {
              existing.bookmarkId = bmId;
              if (!bmUrl.empty()) existing.url = bmUrl;
              if (bmTime > 0) existing.time = bmTime;
              found = true;
              break;
            }
          }
          if (!found) {
            DisplayBookmark bm;
            bm.bookmarkId = bmId;
            bm.title = title;
            bm.url = bmUrl;
            bm.time = bmTime;
            bm.downloaded = false;
            displayList.push_back(std::move(bm));
          }
        }
        line.clear();
      } else {
        line += buf[i];
      }
    }
  }
  file.close();
  LOG_DBG("INS", "Loaded bookmark cache, %d items total", displayList.size());
}

void InstapaperActivity::saveBookmarkCache() {
  const auto& folder = INSTAPAPER_STORE.getDownloadFolder();
  Storage.mkdir(folder.c_str());
  std::string cachePath = folder + "/.bookmarks";

  FsFile file;
  if (!Storage.openFileForWrite("INS", cachePath, file)) return;

  for (const auto& bm : displayList) {
    if (bm.bookmarkId.empty()) continue;
    std::string line = bm.bookmarkId + "|" + bm.title + "|" + bm.url + "|" + std::to_string(bm.time) + "\n";
    file.write(reinterpret_cast<const uint8_t*>(line.data()), line.size());
  }
  file.close();
  LOG_DBG("INS", "Saved bookmark cache");
}

void InstapaperActivity::startBackgroundSync() {
  if (!INSTAPAPER_STORE.hasCredentials() && !INSTAPAPER_STORE.hasLoginCredentials()) {
    syncStatus = "No credentials";
    syncComplete = true;
    updateRequired = true;
    return;
  }

  syncing = true;
  syncStatus = "Syncing...";
  updateRequired = true;
  xTaskCreate(&InstapaperActivity::syncTaskTrampoline, "InstaSync", 8192, this, 1, &syncTaskHandle);
}

void InstapaperActivity::backgroundSyncWork() {
  // Connect WiFi if not already connected
  syncStatus = "WiFi...";
  updateRequired = true;
  if (WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
    WiFi.mode(WIFI_STA);
    WiFi.begin();
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 100) {
      vTaskDelay(100 / portTICK_PERIOD_MS);
      attempts++;
    }
    if (WiFi.status() != WL_CONNECTED) {
      syncStatus = "WiFi failed";
      syncComplete = true;
      updateRequired = true;
      return;
    }
    // Brief pause: routing stack needs a moment after DHCP before DNS/NTP work reliably
    vTaskDelay(500 / portTICK_PERIOD_MS);
  }

  // NTP sync (only if time not already set)
  syncStatus = "NTP...";
  updateRequired = true;
  if (time(nullptr) < 1000000000) {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
    int ntpAttempts = 0;
    while (time(nullptr) < 1000000000 && ntpAttempts < 200) {
      vTaskDelay(100 / portTICK_PERIOD_MS);
      ntpAttempts++;
      if (ntpAttempts == 100) {
        // Re-trigger SNTP after 10 s in case the first attempt stalled
        configTime(0, 0, "time.google.com", "pool.ntp.org", "time.nist.gov");
      }
    }
    if (time(nullptr) < 1000000000) {
      syncStatus = "NTP failed";
      syncComplete = true;
      updateRequired = true;
      return;
    }
  }

  // Authenticate if needed
  if (!INSTAPAPER_STORE.hasCredentials() && INSTAPAPER_STORE.hasLoginCredentials()) {
    syncStatus = "Auth...";
    updateRequired = true;
    std::string token, tokenSecret;
    if (InstapaperClient::authenticate(INSTAPAPER_STORE.getUsername(), INSTAPAPER_STORE.getPassword(), token,
                                       tokenSecret)) {
      INSTAPAPER_STORE.setCredentials(token, tokenSecret);
      INSTAPAPER_STORE.saveToFile();
    } else {
      syncStatus = "Auth failed";
      syncComplete = true;
      updateRequired = true;
      return;
    }
  }

  // Fetch bookmarks from API
  syncStatus = "Fetching...";
  updateRequired = true;
  std::vector<InstapaperBookmark> apiBookmarks;
  if (!InstapaperClient::listBookmarks(25, apiBookmarks)) {
    syncStatus = "Fetch failed";
    syncComplete = true;
    updateRequired = true;
    return;
  }

  // Merge API results into displayList
  xSemaphoreTake(renderingMutex, portMAX_DELAY);

  for (const auto& apiBm : apiBookmarks) {
    // Check if we already have this article (match by sanitized filename)
    std::string sanitizedTitle = StringUtils::sanitizeFilename(apiBm.title);
    bool found = false;
    for (auto& existing : displayList) {
      if (existing.title == sanitizedTitle) {
        // Update bookmarkId, URL, and time so we can download/delete via API
        existing.bookmarkId = apiBm.bookmarkId;
        existing.url = apiBm.url;
        existing.time = apiBm.time;
        found = true;
        break;
      }
    }
    if (!found) {
      DisplayBookmark bm;
      bm.title = sanitizedTitle;
      bm.bookmarkId = apiBm.bookmarkId;
      bm.url = apiBm.url;
      bm.time = apiBm.time;
      bm.downloaded = false;
      displayList.push_back(std::move(bm));
    }
  }

  // Sort by time descending (newest first, time=0 at the end)
  std::sort(displayList.begin(), displayList.end(), [](const DisplayBookmark& a, const DisplayBookmark& b) {
    if (a.time == 0 && b.time == 0) return false;
    if (a.time == 0) return false;
    if (b.time == 0) return true;
    return a.time > b.time;
  });

  syncStatus = "Synced (" + std::to_string(displayList.size()) + ")";
  syncComplete = true;
  updateRequired = true;
  saveBookmarkCache();
  xSemaphoreGive(renderingMutex);

  LOG_DBG("INS", "Background sync complete, %d items in list", displayList.size());
}

void InstapaperActivity::loop() {
  if (state == State::ERROR) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      state = State::BROWSING;
      updateRequired = true;
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
    }
    return;
  }

  if (state == State::DOWNLOADING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
    }
    return;
  }

  if (state == State::BROWSING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!displayList.empty()) {
        openArticle(selectorIndex);
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      downloadNewest();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      if (!displayList.empty()) {
        deleteArticle(selectorIndex);
      }
    }

    if (!displayList.empty()) {
      buttonNavigator.onNextRelease([this] {
        selectorIndex = ButtonNavigator::nextIndex(selectorIndex, displayList.size());
        updateRequired = true;
      });

      buttonNavigator.onPreviousRelease([this] {
        selectorIndex = ButtonNavigator::previousIndex(selectorIndex, displayList.size());
        updateRequired = true;
      });

      buttonNavigator.onNextContinuous([this] {
        int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false);
        selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, displayList.size(), pageItems);
        updateRequired = true;
      });

      buttonNavigator.onPreviousContinuous([this] {
        int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false);
        selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, displayList.size(), pageItems);
        updateRequired = true;
      });
    }
  }
}

void InstapaperActivity::displayTaskLoop() {
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

void InstapaperActivity::render() const {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  auto metrics = UITheme::getInstance().getMetrics();

  // Header with sync status
  std::string title = "Instapaper";
  if (!syncStatus.empty()) {
    title += " [" + syncStatus + "]";
  }
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title.c_str());

  if (state == State::ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, "Error:");
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, errorMessage.c_str());
    const auto labels = mappedInput.mapLabels("« Back", "OK", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == State::DOWNLOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, "Downloading...");
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, statusMessage.c_str());
    if (downloadTotal > 0) {
      const int barWidth = pageWidth - 100;
      constexpr int barHeight = 20;
      constexpr int barX = 50;
      const int barY = pageHeight / 2 + 40;
      GUI.drawProgressBar(renderer, Rect{barX, barY, barWidth, barHeight}, downloadCurrent, downloadTotal);
    }
    renderer.displayBuffer();
    return;
  }

  // Browsing state — clamp selectorIndex defensively in case list changed on another task
  const int sel = (!displayList.empty() && selectorIndex >= 0 && selectorIndex < static_cast<int>(displayList.size()))
                      ? selectorIndex
                      : 0;

  const char* confirmLabel = displayList.empty() ? "Open" : (displayList[sel].downloaded ? "Open" : "Get");
  const auto labels = mappedInput.mapLabels("« Back", confirmLabel, "Get 5", "Delete");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (contentHeight <= 0) {
    renderer.displayBuffer();
    return;
  }

  if (displayList.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20,
                      syncing ? "Syncing..." : "No articles found");
    renderer.displayBuffer();
    return;
  }

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, displayList.size(), sel,
      [this](int index) {
        const auto& bm = displayList[index];
        return bm.downloaded ? "[*] " + bm.title : bm.title;
      },
      nullptr, nullptr, nullptr);

  renderer.displayBuffer();
}

void InstapaperActivity::openArticle(int index) {
  if (index < 0 || index >= static_cast<int>(displayList.size())) return;

  auto& bm = displayList[index];

  if (bm.downloaded) {
    std::string path = getArticlePath(bm);
    onOpenBook(path);
    return;
  }

  // Not downloaded: download only (don't auto-open)
  if (bm.bookmarkId.empty()) {
    state = State::ERROR;
    errorMessage = "No bookmark ID (sync first)";
    updateRequired = true;
    return;
  }

  state = State::DOWNLOADING;
  statusMessage = bm.title;
  downloadCurrent = 0;
  downloadTotal = 0;  // callback fills in byte totals once the HTTP response starts
  updateRequired = true;

  // Ensure WiFi is connected
  if (WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
    WiFi.mode(WIFI_STA);
    WiFi.begin();
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 100) {
      delay(100);
      attempts++;
    }
    if (WiFi.status() != WL_CONNECTED) {
      state = State::ERROR;
      errorMessage = "WiFi not available";
      updateRequired = true;
      return;
    }
    delay(500);  // brief pause for routing stack
    if (time(nullptr) < 1000000000) {
      configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
      int ntpAttempts = 0;
      while (time(nullptr) < 1000000000 && ntpAttempts < 200) {
        delay(100);
        ntpAttempts++;
        if (ntpAttempts == 100) configTime(0, 0, "time.google.com", "pool.ntp.org", "time.nist.gov");
      }
    }
  }

  downloadSingleArticle(bm, [this](size_t cur, size_t total) {
    downloadCurrent = static_cast<int>(cur);
    downloadTotal = static_cast<int>(total);
    updateRequired = true;
  });

  state = State::BROWSING;
  updateRequired = true;

  if (!bm.downloaded) {
    state = State::ERROR;
    errorMessage = "Download failed";
    updateRequired = true;
  }
}

void InstapaperActivity::deleteArticle(int index) {
  xSemaphoreTake(renderingMutex, portMAX_DELAY);

  if (index < 0 || index >= static_cast<int>(displayList.size())) {
    xSemaphoreGive(renderingMutex);
    return;
  }

  auto& bm = displayList[index];
  if (!bm.downloaded) {
    xSemaphoreGive(renderingMutex);
    return;
  }

  // Capture what we need before any structural list change
  std::string path = getArticlePath(bm);
  bool hasBookmarkId = !bm.bookmarkId.empty();

  if (hasBookmarkId) {
    bm.downloaded = false;
  } else {
    displayList.erase(displayList.begin() + index);
    if (selectorIndex >= static_cast<int>(displayList.size()) && selectorIndex > 0) {
      selectorIndex--;
    }
  }

  xSemaphoreGive(renderingMutex);

  // File I/O outside the mutex to keep lock time short
  Storage.remove(path.c_str());
  LOG_DBG("INS", "Deleted: %s", path.c_str());

  updateRequired = true;
}

void InstapaperActivity::downloadNewest() {
  // Find up to 5 undownloaded articles with bookmark IDs
  std::vector<int> toDownload;
  for (int i = 0; i < static_cast<int>(displayList.size()) && toDownload.size() < 5; i++) {
    if (!displayList[i].downloaded && !displayList[i].bookmarkId.empty()) {
      toDownload.push_back(i);
    }
  }

  if (toDownload.empty()) {
    state = State::ERROR;
    errorMessage = "Nothing to download";
    updateRequired = true;
    return;
  }

  state = State::DOWNLOADING;
  downloadCurrent = 0;
  downloadTotal = toDownload.size();
  updateRequired = true;

  const auto& folder = INSTAPAPER_STORE.getDownloadFolder();
  Storage.mkdir(folder.c_str());

  // Need WiFi for downloading
  if (WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
    WiFi.mode(WIFI_STA);
    WiFi.begin();
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 100) {
      delay(100);
      attempts++;
    }
    if (WiFi.status() != WL_CONNECTED) {
      state = State::ERROR;
      errorMessage = "WiFi not available";
      updateRequired = true;
      return;
    }

    delay(500);  // brief pause for routing stack
    if (time(nullptr) < 1000000000) {
      configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
      int ntpAttempts = 0;
      while (time(nullptr) < 1000000000 && ntpAttempts < 200) {
        delay(100);
        ntpAttempts++;
        if (ntpAttempts == 100) configTime(0, 0, "time.google.com", "pool.ntp.org", "time.nist.gov");
      }
    }
  }

  for (int idx : toDownload) {
    auto& bm = displayList[idx];
    statusMessage = bm.title;
    updateRequired = true;

    downloadSingleArticle(bm);

    downloadCurrent++;
    updateRequired = true;
  }

  state = State::BROWSING;
  updateRequired = true;
}

void InstapaperActivity::downloadSingleArticle(DisplayBookmark& bm, HttpDownloader::ProgressCallback progress) {
  std::string html;
  if (!InstapaperClient::getArticleText(bm.bookmarkId, html, progress)) {
    LOG_ERR("INS", "Failed to get text for: %s", bm.title.c_str());
    return;
  }

  // getArticleText() already caps at 64 KB via postUrl(maxBytes). This guard is a safety net
  // in case the cap is ever changed — keeps HTML + markdown peak well under the 380 KB ceiling.
  constexpr size_t MAX_HTML = 32768;  // 32 KB — matches postUrl cap in getArticleText()
  if (html.size() > MAX_HTML) {
    LOG_INF("INS", "Article HTML %zu bytes, truncating to %zu to avoid OOM", html.size(), MAX_HTML);
    html.resize(MAX_HTML);
  }

  std::string markdown = HtmlToMarkdown::convert(html);
  // Free html immediately — no need to hold both in memory while writing
  { std::string tmp; tmp.swap(html); }

  const auto& folder = INSTAPAPER_STORE.getDownloadFolder();
  Storage.mkdir(folder.c_str());

  std::string path = getArticlePath(bm);

  if (Storage.exists(path.c_str())) {
    Storage.remove(path.c_str());
  }

  FsFile file;
  if (!Storage.openFileForWrite("INS", path.c_str(), file)) {
    LOG_ERR("INS", "Failed to write: %s", path.c_str());
    return;
  }

  // Write header and body separately to avoid building a third large concatenated string
  std::string header = "# ";
  header += bm.title;
  header += "\n\n";
  file.write(reinterpret_cast<const uint8_t*>(header.data()), header.size());
  file.write(reinterpret_cast<const uint8_t*>(markdown.data()), markdown.size());
  file.close();

  bm.downloaded = true;
  size_t lastSlash = path.rfind('/');
  bm.filename = (lastSlash != std::string::npos) ? path.substr(lastSlash + 1) : path;
  LOG_DBG("INS", "Saved article: %s (%zu bytes)", path.c_str(), header.size() + markdown.size());
}

std::string InstapaperActivity::getArticlePath(const DisplayBookmark& bm) const {
  const auto& folder = INSTAPAPER_STORE.getDownloadFolder();
  // Use stored filename if available (loaded from SD)
  if (!bm.filename.empty()) {
    return folder + "/" + bm.filename;
  }
  // Generate new filename with language from URL TLD
  std::string lang = bm.url.empty() ? "en" : languageFromUrl(bm.url);
  return folder + "/" + StringUtils::sanitizeFilename(bm.title) + "." + lang + ".md";
}
