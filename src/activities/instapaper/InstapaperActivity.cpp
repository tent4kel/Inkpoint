#include "InstapaperActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <functional>

#include "InstapaperCredentialStore.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/StringUtils.h"

// Persistent state across InstapaperActivity instances
static int  s_savedSelector   = 0;
static std::string s_pendingOpenPath;  // non-empty → auto-open this article on next enter

void InstapaperActivity::setPendingOpenPath(const std::string& path) {
  s_pendingOpenPath = path;
}

namespace {
// Remove the HTML file, its sidecar .meta, and the render cache directory.
// Single source of truth for the three-part deletion; used by deleteArticle()
// and the deleteFn lambda in openArticle().
static void deleteArticleFiles(const std::string& path) {
  Storage.remove(path.c_str());
  Storage.remove((path + ".meta").c_str());
  const size_t hash = std::hash<std::string>{}(path);
  Storage.removeDir(("/.crosspoint/html_" + std::to_string(hash)).c_str());
}

static std::string extractDomain(const std::string& url) {
  size_t s = url.find("://");
  if (s == std::string::npos) return "";
  s += 3;
  size_t e = url.find('/', s);
  if (e == std::string::npos) e = url.size();
  std::string host = url.substr(s, e - s);
  size_t colon = host.rfind(':');
  if (colon != std::string::npos) host = host.substr(0, colon);
  if (host.size() > 4 && host.substr(0, 4) == "www.") host = host.substr(4);
  return host;
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

void InstapaperActivity::downloadTaskTrampoline(void* param) {
  auto* self = static_cast<InstapaperActivity*>(param);
  self->backgroundDownloadWork();
  self->downloadTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

void InstapaperActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();
  displayList.clear();
  downloadQueue.clear();
  activeDownloadIdx = -1;
  abortDownload = false;
  showStopModal = false;
  pendingOpenIdx = -1;
  selectorIndex = s_savedSelector;
  errorMessage.clear();
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

  // Clamp selector in case list changed size
  if (!displayList.empty())
    selectorIndex = std::min(selectorIndex, static_cast<int>(displayList.size()) - 1);

  // Restore any queued downloads from the previous session (before starting tasks).
  // loadQueueFile() may start the download task if items are found.
  loadQueueFile();

  state = State::BROWSING;
  updateRequired = true;

  xTaskCreate(&InstapaperActivity::taskTrampoline, "InstapaperTask", 4096, this, 1, &displayTaskHandle);

  startBackgroundSync();
}

void InstapaperActivity::onExit() {
  s_savedSelector = selectorIndex;
  ActivityWithSubactivity::onExit();

  abortDownload = true;  // signal any in-progress download/sync to stop

  // Disconnect WiFi before killing tasks. This forces any blocked http.POST() /
  // readBytes() in the download or sync task to return an error immediately,
  // so those tasks call http.end() (closing the socket cleanly) and exit via
  // their trampoline. Killing a task that owns an open lwip connection leaves
  // a dangling PCB that crashes the lwip timer task on the next tick.
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.disconnect(false);
    delay(100);
    WiFi.mode(WIFI_OFF);
  }

  // Wait up to 10 s for download/sync tasks to exit cleanly on their own
  // (trampoline sets handle to nullptr before vTaskDelete(nullptr)).
  //
  // Why 10 s (not the old 2 s):
  //   • DNS cleanup: WiFi teardown causes a mid-DNS query to fail within 1 s
  //     (DNS_TMR_INTERVAL). The task then unblocks, cleans up, and exits.
  //   • TLS cleanup: if a task is mid-TLS-handshake or mid-HTTP-POST when
  //     WiFi.mode(WIFI_OFF) fires, lwip closes all TCP connections. mbedTLS
  //     sees the socket error on the next read/write and returns an error.
  //     The HTTP client's end()/destructor then frees the ~26 KB TLS context.
  //     This chain takes up to a few seconds. If we vTaskDelete before the
  //     destructor runs, the TLS heap is leaked — causing the next Instapaper
  //     session to start with a severely fragmented heap.
  for (int i = 0; i < 100 && (downloadTaskHandle || syncTaskHandle); i++) {
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  // Persist queued/in-progress items so the next enter auto-restarts downloads.
  saveQueueFile();
  if (downloadTaskHandle) {
    vTaskDelete(downloadTaskHandle);
    downloadTaskHandle = nullptr;
  }
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
  downloadQueue.clear();
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

    if (filename.size() > 5 && filename.substr(filename.size() - 5) == ".html") {
      DisplayBookmark bm;
      bm.filename = filename;
      bm.title = filename.substr(0, filename.size() - 5);
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
        // Parse line: bookmarkId|title|url|time[|source]
        size_t sep = line.find('|');
        if (sep != std::string::npos) {
          std::string bmId = line.substr(0, sep);
          std::string rest = line.substr(sep + 1);
          std::string title, bmUrl, bmSource;
          long bmTime = 0;

          size_t sep2 = rest.find('|');
          if (sep2 != std::string::npos) {
            title = rest.substr(0, sep2);
            std::string rest2 = rest.substr(sep2 + 1);
            size_t sep3 = rest2.find('|');
            if (sep3 != std::string::npos) {
              bmUrl = rest2.substr(0, sep3);
              std::string rest3 = rest2.substr(sep3 + 1);
              size_t sep4 = rest3.find('|');
              if (sep4 != std::string::npos) {
                bmTime = atol(rest3.substr(0, sep4).c_str());
                bmSource = rest3.substr(sep4 + 1);
              } else {
                bmTime = atol(rest3.c_str());
              }
            } else {
              bmUrl = rest2;
            }
          } else {
            title = rest;
          }

          // Fall back to extracting domain from URL if source not stored
          if (bmSource.empty() && !bmUrl.empty()) {
            bmSource = extractDomain(bmUrl);
          }

          // Check if already in list (from SD scan)
          bool found = false;
          for (auto& existing : displayList) {
            if (existing.title == title) {
              existing.bookmarkId = bmId;
              if (!bmUrl.empty()) existing.url = bmUrl;
              if (bmTime > 0) existing.time = bmTime;
              if (!bmSource.empty()) existing.source = bmSource;
              found = true;
              break;
            }
          }
          if (!found) {
            DisplayBookmark bm;
            bm.bookmarkId = bmId;
            bm.title = title;
            bm.url = bmUrl;
            bm.source = bmSource;
            bm.time = bmTime;
            bm.downloaded = false;
            displayList.push_back(std::move(bm));
          }
        }
        line.clear();
      } else if (line.size() < 4096) {
        line += buf[i];  // bounded: silently truncate absurdly long lines
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
    std::string line = bm.bookmarkId + "|" + bm.title + "|" + bm.url + "|" + std::to_string(bm.time) + "|" +
                       bm.source + "\n";
    file.write(reinterpret_cast<const uint8_t*>(line.data()), line.size());
  }
  file.close();
  LOG_DBG("INS", "Saved bookmark cache");
}

void InstapaperActivity::saveQueueFile() {
  const std::string path = INSTAPAPER_STORE.getDownloadFolder() + "/.queue";

  // Collect bookmarkIds of items that were queued or mid-download when we exited.
  // Mid-download items (downloading=true) had abortDownload set, so their download
  // was interrupted — include them so they restart on next enter.
  std::vector<std::string> ids;
  for (const auto& bm : displayList) {
    if ((bm.queued || bm.downloading) && !bm.bookmarkId.empty()) {
      ids.push_back(bm.bookmarkId);
    }
  }

  if (ids.empty()) {
    Storage.remove(path.c_str());
    return;
  }

  FsFile file;
  if (!Storage.openFileForWrite("INS", path, file)) return;
  for (const auto& id : ids) {
    std::string line = id + "\n";
    file.write(reinterpret_cast<const uint8_t*>(line.data()), line.size());
  }
  file.close();
  LOG_DBG("INS", "Saved %d queued bookmark(s)", ids.size());
}

void InstapaperActivity::loadQueueFile() {
  const std::string path = INSTAPAPER_STORE.getDownloadFolder() + "/.queue";

  FsFile file;
  if (!Storage.openFileForRead("INS", path, file)) return;

  // Parse one bookmarkId per line
  std::vector<std::string> ids;
  char buf[128];
  std::string line;
  while (file.available()) {
    const int n = file.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf) - 1);
    if (n <= 0) break;
    buf[n] = '\0';
    for (int i = 0; i < n; i++) {
      if (buf[i] == '\n') {
        if (!line.empty()) { ids.push_back(line); line.clear(); }
      } else {
        line += buf[i];
      }
    }
  }
  if (!line.empty()) ids.push_back(line);
  file.close();

  // Re-queue matching items that are not already downloaded
  bool anyQueued = false;
  for (const auto& id : ids) {
    for (int i = 0; i < static_cast<int>(displayList.size()); i++) {
      auto& bm = displayList[i];
      if (bm.bookmarkId == id && !bm.downloaded && !bm.queued) {
        bm.queued = true;
        downloadQueue.push_back(i);
        anyQueued = true;
        break;
      }
    }
  }

  if (anyQueued) {
    LOG_DBG("INS", "Restored %d queued download(s) from queue file — task will start via watchdog", downloadQueue.size());
    // Do NOT start the download task here: startBackgroundSync() hasn't run yet,
    // so syncing=false and the while(syncing) guard in backgroundDownloadWork would
    // be bypassed, causing two simultaneous TLS connections (OOM / abort).
    // The loop() watchdog starts the task once the display task is running (which
    // is always after startBackgroundSync() has set syncing=true).
  }
}

void InstapaperActivity::startBackgroundSync() {
  if (!INSTAPAPER_STORE.hasCredentials() && !INSTAPAPER_STORE.hasLoginCredentials()) {
    syncStatus = tr(STR_NO_CREDENTIALS);
    syncComplete = true;
    updateRequired = true;
    return;
  }

  syncing = true;
  syncStatus = tr(STR_SYNCING);
  updateRequired = true;
  xTaskCreate(&InstapaperActivity::syncTaskTrampoline, "InstaSync", 8192, this, 1, &syncTaskHandle);
}

void InstapaperActivity::backgroundSyncWork() {
  // Grace period: wait up to 2 s before touching the network. If the user
  // exits during this window, abortDownload is set and we return without ever
  // registering a DNS callback. This prevents the lwip dns_timeout_cb crash:
  // netconn_gethostbyname() stack-allocates `struct dns_api_msg msg` and
  // passes &msg.sem as the DNS callback arg. vTaskDelete frees the stack, but
  // the DNS timer fires ~1000 ms later and writes to the freed address.
  for (int i = 0; i < 20 && !abortDownload; i++) {
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
  if (abortDownload) return;

  // Connect WiFi if not already connected
  syncStatus = tr(STR_CONNECTING);
  updateRequired = true;
  if (WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
    WiFi.mode(WIFI_STA);
    WiFi.begin();
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 100 && !abortDownload) {
      vTaskDelay(100 / portTICK_PERIOD_MS);
      attempts++;
    }
    if (WiFi.status() != WL_CONNECTED) {
      syncStatus = tr(STR_WIFI_CONN_FAILED);
      syncComplete = true;
      updateRequired = true;
      return;
    }
    // Brief pause: routing stack needs a moment after DHCP before DNS/NTP work reliably
    for (int i = 0; i < 5 && !abortDownload; i++) vTaskDelay(100 / portTICK_PERIOD_MS);
    if (abortDownload) return;
  }

  // NTP sync (only if time not already set)
  syncStatus = tr(STR_NTP);
  updateRequired = true;
  if (time(nullptr) < 1000000000) {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
    int ntpAttempts = 0;
    while (time(nullptr) < 1000000000 && ntpAttempts < 200 && !abortDownload) {
      vTaskDelay(100 / portTICK_PERIOD_MS);
      ntpAttempts++;
      if (ntpAttempts == 100) {
        // Re-trigger SNTP after 10 s in case the first attempt stalled
        configTime(0, 0, "time.google.com", "pool.ntp.org", "time.nist.gov");
      }
    }
    if (time(nullptr) < 1000000000) {
      syncStatus = tr(STR_NTP_FAILED);
      syncComplete = true;
      updateRequired = true;
      return;
    }
  }

  // Authenticate if needed
  if (!INSTAPAPER_STORE.hasCredentials() && INSTAPAPER_STORE.hasLoginCredentials()) {
    syncStatus = tr(STR_AUTHENTICATING);
    updateRequired = true;
    std::string token, tokenSecret;
    if (InstapaperClient::authenticate(INSTAPAPER_STORE.getUsername(), INSTAPAPER_STORE.getPassword(), token,
                                       tokenSecret)) {
      INSTAPAPER_STORE.setCredentials(token, tokenSecret);
      INSTAPAPER_STORE.saveToFile();
    } else {
      syncStatus = tr(STR_AUTH_FAILED);
      syncComplete = true;
      updateRequired = true;
      return;
    }
  }

  // Fetch bookmarks from API
  syncStatus = tr(STR_FETCHING);
  updateRequired = true;
  std::vector<InstapaperBookmark> apiBookmarks;
  if (!InstapaperClient::listBookmarks(25, apiBookmarks)) {
    syncStatus = tr(STR_FETCH_FAILED);
    syncComplete = true;
    updateRequired = true;
    return;
  }

  // Merge API results into displayList
  xSemaphoreTake(renderingMutex, portMAX_DELAY);

  for (const auto& apiBm : apiBookmarks) {
    std::string sanitizedTitle = StringUtils::sanitizeFilename(apiBm.title);
    std::string domain = extractDomain(apiBm.url);
    bool found = false;
    for (auto& existing : displayList) {
      if (existing.title == sanitizedTitle) {
        existing.bookmarkId = apiBm.bookmarkId;
        existing.url = apiBm.url;
        existing.time = apiBm.time;
        if (!domain.empty()) existing.source = domain;
        found = true;
        break;
      }
    }
    if (!found) {
      DisplayBookmark bm;
      bm.title = sanitizedTitle;
      bm.bookmarkId = apiBm.bookmarkId;
      bm.url = apiBm.url;
      bm.source = domain;
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

  syncStatus = std::string(tr(STR_SYNCED)) + " (" + std::to_string(displayList.size()) + ")";
  syncComplete = true;
  updateRequired = true;
  saveBookmarkCache();
  xSemaphoreGive(renderingMutex);

  LOG_DBG("INS", "Background sync complete, %d items in list", displayList.size());
}

void InstapaperActivity::loop() {
  // Auto-open next article (set by advance/delete callbacks from reader)
  if (!s_pendingOpenPath.empty()) {
    std::string pending = std::move(s_pendingOpenPath);
    s_pendingOpenPath.clear();
    for (int i = 0; i < static_cast<int>(displayList.size()); i++) {
      if (displayList[i].downloaded && getArticlePath(displayList[i]) == pending) {
        selectorIndex = i;
        openArticle(i);
        return;
      }
    }
    // Article not found (already deleted or not yet synced) — just show list
  }

  // Modal takes priority over everything
  if (showStopModal) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      showStopModal = false;
      abortDownload = true;
      if (pendingOpenIdx >= 0 && pendingOpenIdx < static_cast<int>(displayList.size())) {
        openArticle(pendingOpenIdx);  // reuse openArticle to build callbacks properly
      }
      pendingOpenIdx = -1;
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
               mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      showStopModal = false;
      pendingOpenIdx = -1;
      updateRequired = true;
    }
    return;
  }

  if (state == State::ERROR) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      state = State::BROWSING;
      updateRequired = true;
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
    }
    return;
  }

  if (state == State::BROWSING) {
    // Restart download task if items are still queued but the task died (e.g., transient WiFi failure).
    // Queued items are preserved on non-abort failure; this watchdog re-launches the task automatically
    // once the user is back in BROWSING state (after dismissing any error or just waiting).
    if (!downloadQueue.empty() && downloadTaskHandle == nullptr && !abortDownload) {
      xTaskCreate(&InstapaperActivity::downloadTaskTrampoline, "InstaDownload", 8192, this, 1, &downloadTaskHandle);
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!displayList.empty()) {
        openArticle(selectorIndex);
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      queueNewest(5);
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
        int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, true);
        selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, displayList.size(), pageItems);
        updateRequired = true;
      });

      buttonNavigator.onPreviousContinuous([this] {
        int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, true);
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
  const auto metrics = UITheme::getInstance().getMetrics();

  // Header with sync status
  std::string title = tr(STR_INSTAPAPER);
  if (!syncStatus.empty()) {
    title += " [" + syncStatus + "]";
  }
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title.c_str());

  if (state == State::ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_ERROR_MSG));
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, errorMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OK_BUTTON), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (showStopModal) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_DOWNLOADS_RUNNING));
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, tr(STR_STOP_DOWNLOADS_CONFIRM));
    const auto labels = mappedInput.mapLabels(tr(STR_NO), tr(STR_STOP_AND_OPEN), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // Browsing state — clamp selectorIndex defensively in case list changed on another task
  const int sel = (!displayList.empty() && selectorIndex >= 0 && selectorIndex < static_cast<int>(displayList.size()))
                      ? selectorIndex
                      : 0;

  // Button hints
  const char* confirmLabel = tr(STR_GET);
  if (!displayList.empty()) {
    const auto& selBm = displayList[sel];
    if (selBm.downloaded) {
      confirmLabel = tr(STR_OPEN);
    } else if (selBm.queued || selBm.downloading) {
      confirmLabel = tr(STR_CANCEL);
    }
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_GET_NEWEST), tr(STR_DELETE));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (contentHeight <= 0) {
    renderer.displayBuffer();
    return;
  }

  if (displayList.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20,
                      syncing ? tr(STR_SYNCING) : tr(STR_NO_ARTICLES));
    renderer.displayBuffer();
    return;
  }

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, displayList.size(), sel,
      [&](int i) -> std::string { return displayList[i].title; },
      [&](int i) -> std::string {
        // Right-aligned indicators are drawn separately below; subtitle = source only.
        // Return "" when the progress bar occupies the subtitle row.
        if (displayList[i].downloading && displayList[i].dlTotal > 0) return "";
        return displayList[i].source;
      },
      nullptr, nullptr);

  // Right-aligned subtitle indicators and progress bar, drawn after drawList.
  // Font, inset, and Y-offset are queried from the theme so Classic and Lyra both align correctly.
  {
    const int subOff   = GUI.getListSubtitleYOffset(); // px from row top to subtitle baseline
    const int subFont  = GUI.getListSubtitleFontId();  // SMALL_FONT_ID in Lyra, UI_10_FONT_ID in Classic
    const int inset    = GUI.getListTextInset();       // 8px in Lyra (hPaddingInSelection), 0 in Classic
    const int rowH     = metrics.listWithSubtitleRowHeight;
    const int pageItems = contentHeight / rowH;
    const int scrollOff = (sel / pageItems) * pageItems;
    const int leftEdge  = metrics.contentSidePadding + inset;
    const int rightEdge = pageWidth - 5 - metrics.contentSidePadding - inset;
    const int subH      = rowH - subOff;
    const int textH     = renderer.getLineHeight(subFont);

    for (int i = scrollOff; i < static_cast<int>(displayList.size()) && i < scrollOff + pageItems; i++) {
      const auto& bm = displayList[i];
      const int visIdx = i - scrollOff;
      const int subY = contentTop + visIdx * rowH + subOff;
      const bool inv = (i != sel);

      if (bm.downloading && bm.dlTotal > 0) {
        const int pct = static_cast<int>(100LL * bm.dlCurrent / bm.dlTotal);
        const std::string pctStr = std::to_string(pct) + "%";
        const int pctW = renderer.getTextWidth(subFont, pctStr.c_str());
        const int barH = 10;
        const int barW = rightEdge - leftEdge - pctW - 6;
        const int barY = subY + (subH - barH) / 2;
        const int textY = subY + (subH - textH) / 2;

        renderer.drawRect(leftEdge, barY, barW, barH);
        const int fillW = (barW - 4) * pct / 100;
        if (fillW > 0) renderer.fillRect(leftEdge + 2, barY + 2, fillW, barH - 4);
        renderer.drawText(subFont, rightEdge - pctW, textY, pctStr.c_str(), inv);
      } else {
        const char* label = nullptr;
        if      (bm.downloading) label = tr(STR_CONNECTING);
        else if (bm.queued)      label = tr(STR_QUEUED);
        else if (bm.downloaded)  label = tr(STR_SAVED);
        if (label) {
          const int w = renderer.getTextWidth(subFont, label);
          renderer.drawText(subFont, rightEdge - w, subY, label, inv);
        }
      }
    }
  }

  renderer.displayBuffer();
}

void InstapaperActivity::toggleQueue(int index) {
  if (index < 0 || index >= static_cast<int>(displayList.size())) return;

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  auto& bm = displayList[index];

  if (bm.downloading) {
    // Signal the task to abort; it will clean up downloading/dlCurrent/dlTotal flags
    xSemaphoreGive(renderingMutex);
    abortDownload = true;
    updateRequired = true;
    return;
  }

  if (bm.queued) {
    bm.queued = false;
    auto it = std::find(downloadQueue.begin(), downloadQueue.end(), index);
    if (it != downloadQueue.end()) downloadQueue.erase(it);
    xSemaphoreGive(renderingMutex);
    updateRequired = true;
    return;
  }

  if (!bm.downloaded && !bm.bookmarkId.empty()) {
    bm.queued = true;
    downloadQueue.push_back(index);
    // Advance cursor so user can queue the next article
    int nextIdx = ButtonNavigator::nextIndex(index, displayList.size());
    if (nextIdx != index) selectorIndex = nextIdx;
    bool needStart = (downloadTaskHandle == nullptr);
    xSemaphoreGive(renderingMutex);
    if (needStart) {
      abortDownload = false;
      xTaskCreate(&InstapaperActivity::downloadTaskTrampoline, "InstaDownload", 8192, this, 1, &downloadTaskHandle);
    }
    updateRequired = true;
    return;
  }

  xSemaphoreGive(renderingMutex);
}

void InstapaperActivity::queueNewest(int n) {
  int count = 0;
  for (int i = 0; i < static_cast<int>(displayList.size()) && count < n; i++) {
    const auto& bm = displayList[i];
    if (!bm.downloaded && !bm.queued && !bm.downloading && !bm.bookmarkId.empty()) {
      toggleQueue(i);
      count++;
    }
  }
}

void InstapaperActivity::openArticle(int index) {
  if (index < 0 || index >= static_cast<int>(displayList.size())) return;

  const auto& bm = displayList[index];

  if (bm.downloaded) {
    if (activeDownloadIdx >= 0 || !downloadQueue.empty()) {
      showStopModal = true;
      pendingOpenIdx = index;
      updateRequired = true;
    } else {
      // Find next downloaded article for advance/delete callbacks
      std::string nextPath;
      for (int i = index + 1; i < static_cast<int>(displayList.size()); i++) {
        if (displayList[i].downloaded) { nextPath = getArticlePath(displayList[i]); break; }
      }
      const std::string currentPath = getArticlePath(bm);
      auto goBack = onGoToSelf;  // capture by value — safe after this activity exits
      // advanceFn is null when there is no next article — hides the "Read on" hint
      std::function<void()> advanceFn;
      if (!nextPath.empty()) {
        advanceFn = [nextPath, goBack]() {
          s_pendingOpenPath = nextPath;
          goBack();
        };
      }
      auto deleteFn = [currentPath, goBack]() {
        deleteArticleFiles(currentPath);
        goBack();  // return to list — no auto-advance after delete
      };
      onOpenBook(currentPath, std::move(advanceFn), std::move(deleteFn));
    }
    return;
  }

  toggleQueue(index);  // toggleQueue handles queued/downloading/unqueued cases and sets updateRequired
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
  deleteArticleFiles(path);
  LOG_DBG("INS", "Deleted: %s", path.c_str());

  updateRequired = true;
}

bool InstapaperActivity::ensureWifiAndNtp() {
  if (WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
    updateRequired = true;
    WiFi.mode(WIFI_STA);
    WiFi.begin();
    for (int i = 0; i < 100 && WiFi.status() != WL_CONNECTED && !abortDownload; i++) {
      vTaskDelay(100 / portTICK_PERIOD_MS);
    }
    if (WiFi.status() != WL_CONNECTED) return false;
    for (int i = 0; i < 5 && !abortDownload; i++) vTaskDelay(100 / portTICK_PERIOD_MS);
    if (abortDownload) return false;
  }

  if (time(nullptr) < 1000000000) {
    updateRequired = true;
    configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
    for (int i = 0; i < 200 && time(nullptr) < 1000000000 && !abortDownload; i++) {
      vTaskDelay(100 / portTICK_PERIOD_MS);
      if (i == 100) configTime(0, 0, "time.google.com", "pool.ntp.org", "time.nist.gov");
    }
    if (time(nullptr) < 1000000000) return false;
  }

  return true;
}

void InstapaperActivity::backgroundDownloadWork() {
  const auto& folder = INSTAPAPER_STORE.getDownloadFolder();
  Storage.mkdir(folder.c_str());

  // Serialize with the sync task: wait until sync finishes before opening our own
  // TLS connection. Running two simultaneous TLS contexts (~40 KB each) on the
  // 233 KB heap causes near-OOM, heap corruption, and stale DNS callbacks.
  // Sync also sets up WiFi and NTP, so we get those for free.
  while (syncing && !abortDownload) {
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
  if (abortDownload) return;

  if (!ensureWifiAndNtp()) {
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    activeDownloadIdx = -1;
    if (abortDownload) {
      // User explicitly exited — clear the queue so items don't re-trigger on next enter
      for (int qi : downloadQueue) {
        if (qi < static_cast<int>(displayList.size())) displayList[qi].queued = false;
      }
      downloadQueue.clear();
    }
    // Transient failure: leave downloadQueue + queued flags intact.
    // loop() will auto-restart this task once the user dismisses the error.
    if (!abortDownload) {
      state = State::ERROR;
      errorMessage = tr(STR_WIFI_CONN_FAILED);
    }
    xSemaphoreGive(renderingMutex);

    abortDownload = false;
    updateRequired = true;
    return;
  }

  while (!abortDownload) {
    // Pop next item from queue
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    if (downloadQueue.empty()) {
      xSemaphoreGive(renderingMutex);
      break;
    }
    int idx = downloadQueue.front();
    downloadQueue.erase(downloadQueue.begin());

    if (idx >= static_cast<int>(displayList.size())) {
      xSemaphoreGive(renderingMutex);
      continue;
    }

    displayList[idx].queued = false;
    displayList[idx].downloading = true;
    displayList[idx].dlCurrent = 0;
    displayList[idx].dlTotal = 0;
    activeDownloadIdx = idx;
    DisplayBookmark bmCopy = displayList[idx];
    xSemaphoreGive(renderingMutex);
    updateRequired = true;

    downloadSingleArticle(
        bmCopy,
        [this, idx](size_t cur, size_t total) {
          xSemaphoreTake(renderingMutex, portMAX_DELAY);
          if (idx < static_cast<int>(displayList.size())) {
            displayList[idx].dlCurrent = static_cast<int>(cur);
            displayList[idx].dlTotal = static_cast<int>(total);
          }
          xSemaphoreGive(renderingMutex);
          updateRequired = true;
        },
        [this]() { return static_cast<bool>(abortDownload); });

    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    if (idx < static_cast<int>(displayList.size())) {
      displayList[idx].downloading = false;
      displayList[idx].dlCurrent = 0;
      displayList[idx].dlTotal = 0;
      if (bmCopy.downloaded) {
        displayList[idx].downloaded = true;
        displayList[idx].filename = bmCopy.filename;
      }
    }
    activeDownloadIdx = -1;
    xSemaphoreGive(renderingMutex);
    updateRequired = true;

    if (abortDownload) break;
  }

  abortDownload = false;
  activeDownloadIdx = -1;
  updateRequired = true;
}

void InstapaperActivity::downloadSingleArticle(DisplayBookmark& bm, HttpDownloader::ProgressCallback progress,
                                               std::function<bool()> abortCheck) {
  const auto& folder = INSTAPAPER_STORE.getDownloadFolder();
  Storage.mkdir(folder.c_str());

  std::string path = getArticlePath(bm);

  const auto result = InstapaperClient::getArticleToFile(bm.bookmarkId, path, progress, abortCheck);
  if (result != HttpDownloader::DownloadError::OK) {
    LOG_ERR("INS", "Failed to download article to file: %s", bm.title.c_str());
    return;
  }

  bm.downloaded = true;
  const size_t lastSlash = path.rfind('/');
  bm.filename = (lastSlash != std::string::npos) ? path.substr(lastSlash + 1) : path;
  LOG_DBG("INS", "Saved article: %s", path.c_str());

  // Write sidecar metadata: title|source|timestamp
  FsFile metaFile;
  if (Storage.openFileForWrite("INS", path + ".meta", metaFile)) {
    std::string line = bm.title + "|" + bm.source + "|" + std::to_string(bm.time) + "\n";
    metaFile.write(line.c_str(), line.size());
    metaFile.close();
  }
}

std::string InstapaperActivity::getArticlePath(const DisplayBookmark& bm) const {
  const auto& folder = INSTAPAPER_STORE.getDownloadFolder();
  // Use stored filename if available (loaded from SD)
  if (!bm.filename.empty()) {
    return folder + "/" + bm.filename;
  }
  // Generate new .html filename
  return folder + "/" + StringUtils::sanitizeFilename(bm.title) + ".html";
}
