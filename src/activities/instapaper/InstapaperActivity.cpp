#include "InstapaperActivity.h"

#include <Esp.h>
#include <GfxRenderer.h>
#include <esp_sleep.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <functional>

#include "InstapaperCredentialStore.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/StringUtils.h"

// Persistent state across InstapaperActivity instances
static int  s_savedSelector   = 0;
static std::string s_pendingOpenPath;  // non-empty → auto-open this article on next enter
// Set after first successful fetch; cleared only by ESP.restart(). Subsequent
// entries skip listBookmarks (heap too fragmented for 15KB response buffer)
// and only bring up WiFi for downloads.
static bool s_everSynced = false;

// RTC memory survives ESP.restart() but not power-off. Set before restarting
// to have the boot sequence navigate straight back to Instapaper.
extern RTC_DATA_ATTR bool rtcGoToInstapaper;


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
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();
  displayList.clear();
  downloadQueue.clear();
  activeDownloadIdx = -1;
  abortDownload = false;
  showStopModal = false;
  pendingOpenIdx = -1;
  exitingActivity = false;
  pendingRestart = false;
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
  Activity::onExit();

  exitingActivity = true;  // tell download task to re-queue mid-download item (not user-cancel)
  abortDownload = true;    // signal any in-progress download/sync to stop

  // Disconnect from AP — closes open TCP connections and unblocks tasks waiting
  // on network I/O.  Do NOT call WiFi.mode(WIFI_OFF) yet: if a task is
  // mid-write inside lwIP/mbedTLS, powering off the radio from a second task
  // causes a concurrent-lwIP crash.
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.disconnect(false);
    delay(200);  // give lwIP time to send FIN/RST
  }

  // Wait up to 10 s for download/sync tasks to exit cleanly on their own
  // (trampoline sets handle to nullptr before vTaskDelete(nullptr)).
  //
  // Why 10 s:
  //   • DNS cleanup: WiFi teardown causes a mid-DNS query to fail within 1 s
  //     (DNS_TMR_INTERVAL). The task then unblocks, cleans up, and exits.
  //   • TLS cleanup: mbedTLS must free the ~26 KB TLS context before the task
  //     exits. If we vTaskDelete before the destructor runs, that heap is
  //     leaked — causing the next session to start with a fragmented heap.
  for (int i = 0; i < 100 && (downloadTaskHandle || syncTaskHandle); i++) {
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }

  WiFi.mode(WIFI_OFF);

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
  if (!INSTAPAPER_STORE.hasCredentials()) {
    syncStatus = tr(STR_NO_CREDENTIALS);
    syncComplete = true;
    updateRequired = true;
    return;
  }

  syncing = true;
  updateRequired = true;
  xTaskCreate(&InstapaperActivity::syncTaskTrampoline, "InstaSync", 8192, this, 1, &syncTaskHandle);
}

void InstapaperActivity::backgroundSyncWork() {
  LOG_INF("INS", "Sync start — free=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
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

  // The 2 s grace period above guarantees the cached article list has rendered.
  // Check heap before starting any network work: if MaxAlloc is too small for a
  // reliable TLS session, inform the user and stop. The Left button (Sync) triggers
  // a force-sync reboot that restores a clean heap.
  if (!s_everSynced && ESP.getMaxAllocHeap() < 65000) {
    LOG_INF("INS", "Pre-WiFi heap check: maxAlloc=%u < 65000, skipping sync", ESP.getMaxAllocHeap());
    syncStatus = tr(STR_FETCH_LOW_MEM);
    syncComplete = true;
    updateRequired = true;
    return;
  }

  // Connect WiFi if not already connected
  if (WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
    syncStatus = tr(STR_CONNECTING);
    updateRequired = true;
    LOG_INF("INS", "WiFi: connecting...");
    WiFi.mode(WIFI_STA);
    WiFi.begin();
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 100 && !abortDownload) {
      vTaskDelay(100 / portTICK_PERIOD_MS);
      attempts++;
    }
    if (WiFi.status() != WL_CONNECTED) {
      LOG_ERR("INS", "WiFi: connect failed after %d ms", attempts * 100);
      syncStatus = tr(STR_WIFI_CONN_FAILED);
      syncComplete = true;
      updateRequired = true;
      return;
    }
    LOG_INF("INS", "WiFi: up in %d ms — free=%u maxAlloc=%u", attempts * 100, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    // Brief pause: routing stack needs a moment after DHCP before DNS/NTP work reliably
    for (int i = 0; i < 5 && !abortDownload; i++) vTaskDelay(100 / portTICK_PERIOD_MS);
    if (abortDownload) return;
  } else {
    LOG_INF("INS", "WiFi: already up, skip connect");
  }

  // NTP sync (only if time not already set)
  if (time(nullptr) < 1000000000) {
    syncStatus = tr(STR_NTP);
    updateRequired = true;
    LOG_INF("INS", "NTP: syncing... (t=%lld)", (long long)time(nullptr));
    configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
    int ntpAttempts = 0;
    while (time(nullptr) < 1000000000 && ntpAttempts < 200 && !abortDownload) {
      vTaskDelay(100 / portTICK_PERIOD_MS);
      ntpAttempts++;
      if (ntpAttempts == 100) {
        // Re-trigger SNTP after 10 s in case the first attempt stalled
        LOG_INF("INS", "NTP: 10 s elapsed, retrying with alternate servers");
        configTime(0, 0, "time.google.com", "pool.ntp.org", "time.nist.gov");
      }
    }
    if (time(nullptr) < 1000000000) {
      LOG_ERR("INS", "NTP: failed after %d ms", ntpAttempts * 100);
      syncStatus = tr(STR_NTP_FAILED);
      syncComplete = true;
      updateRequired = true;
      return;
    }
    LOG_INF("INS", "NTP: synced in %d ms (t=%lld)", ntpAttempts * 100, (long long)time(nullptr));
  } else {
    LOG_INF("INS", "NTP: already set (t=%lld), skip", (long long)time(nullptr));
  }

  // On re-entries within the same boot session, skip listBookmarks. Each
  // WiFi ON/OFF cycle fragments the heap ~10-15 KB, leaving too little
  // contiguous memory for the ~15 KB response buffer + TLS handshake.
  // The cached bookmark list is fresh enough; downloads still proceed via
  // postUrlToFile which streams to SD without a large response buffer.
  // A force-sync (Left button → ESP.restart()) gives a clean heap again.
  if (s_everSynced) {
    LOG_INF("INS", "Fetch: skip — already synced this boot (free=%u maxAlloc=%u)",
            ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    syncStatus = std::to_string(displayList.size()) + tr(STR_CACHED_BOOKMARKS);

    syncComplete = true;
    updateRequired = true;
    return;
  }
  s_everSynced = true;  // Set before attempt: don't retry fetch on next entry

  // Fetch bookmarks from API
  syncStatus = tr(STR_FETCHING);
  updateRequired = true;
  LOG_INF("INS", "Fetch: start — free=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  std::vector<InstapaperBookmark> apiBookmarks;
  if (!InstapaperClient::listBookmarks(30, apiBookmarks)) {
    LOG_ERR("INS", "Fetch: failed — free=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    syncStatus = tr(STR_FETCH_FAILED);
    syncComplete = true;
    updateRequired = true;
    return;
  }
  LOG_INF("INS", "Fetch: got %d bookmarks — free=%u maxAlloc=%u", static_cast<int>(apiBookmarks.size()),
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  // Merge API results into displayList
  xSemaphoreTake(renderingMutex, portMAX_DELAY);

  for (const auto& apiBm : apiBookmarks) {
    std::string sanitizedTitle = StringUtils::sanitizeFilename(apiBm.title);
    std::string domain = extractDomain(apiBm.url);
    bool found = false;
    // Match by bookmarkId first — reliable even when the title has changed.
    for (auto& existing : displayList) {
      if (!existing.bookmarkId.empty() && existing.bookmarkId == apiBm.bookmarkId) {
        existing.title = sanitizedTitle;
        existing.url = apiBm.url;
        existing.time = apiBm.time;
        if (!domain.empty()) existing.source = domain;
        found = true;
        break;
      }
    }
    // Fall back to title match for locally-scanned SD articles (no bookmarkId yet).
    if (!found) {
      for (auto& existing : displayList) {
        if (existing.bookmarkId.empty() && existing.title == sanitizedTitle) {
          existing.bookmarkId = apiBm.bookmarkId;
          existing.url = apiBm.url;
          existing.time = apiBm.time;
          if (!domain.empty()) existing.source = domain;
          found = true;
          break;
        }
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

  // Prune entries removed from Instapaper's reading list.
  // Non-downloaded entries are silently dropped; downloaded ones are queued for
  // archive or deletion according to the user's "Removed articles" setting.
  {
    std::vector<std::string> apiIds;
    apiIds.reserve(apiBookmarks.size());
    for (const auto& apiBm : apiBookmarks) apiIds.push_back(apiBm.bookmarkId);

    bool doArchive = INSTAPAPER_STORE.getArchiveOldArticles();
    std::vector<std::string> toCleanup;  // paths of downloaded non-API articles
    int pruned = 0;

    auto it = displayList.begin();
    while (it != displayList.end()) {
      if (!it->bookmarkId.empty()) {
        bool inApi = false;
        for (const auto& id : apiIds) { if (id == it->bookmarkId) { inApi = true; break; } }
        if (!inApi) {
          if (it->downloaded) {
            toCleanup.push_back(getArticlePath(*it));
          } else {
            pruned++;
          }
          it = displayList.erase(it);
          continue;
        }
      }
      ++it;
    }

    if (selectorIndex >= static_cast<int>(displayList.size()))
      selectorIndex = std::max(0, static_cast<int>(displayList.size()) - 1);

    if (pruned > 0 || !toCleanup.empty()) {
      // Rebuild downloadQueue — indices are stale after erase
      downloadQueue.clear();
      for (int i = 0; i < static_cast<int>(displayList.size()); i++) {
        if (displayList[i].queued) downloadQueue.push_back(i);
      }
      LOG_INF("INS", "Pruned %d unread + %d downloaded non-API entries",
              pruned, static_cast<int>(toCleanup.size()));
    }

    // Sort by time descending (newest first, time=0 at the end)
    std::sort(displayList.begin(), displayList.end(), [](const DisplayBookmark& a, const DisplayBookmark& b) {
      if (a.time == 0 && b.time == 0) return false;
      if (a.time == 0) return false;
      if (b.time == 0) return true;
      return a.time > b.time;
    });

    syncStatus = std::to_string(displayList.size()) + tr(STR_SYNCED_BOOKMARKS);
    syncComplete = true;
    updateRequired = true;
    saveBookmarkCache();
    xSemaphoreGive(renderingMutex);

    // File operations — outside mutex, after saveBookmarkCache has persisted the pruned list
    if (!toCleanup.empty()) {
      const std::string archiveFolder = INSTAPAPER_STORE.getDownloadFolder() + "/archive";
      if (doArchive) Storage.mkdir(archiveFolder.c_str());
      for (const auto& path : toCleanup) {
        if (doArchive) {
          const size_t slash = path.rfind('/');
          const std::string fname = (slash != std::string::npos) ? path.substr(slash) : ("/" + path);
          Storage.rename(path.c_str(), (archiveFolder + fname).c_str());
        } else {
          Storage.remove(path.c_str());
        }
        // Sidecar and render cache always purged (derived data)
        Storage.remove((path + ".meta").c_str());
        const size_t hash = std::hash<std::string>{}(path);
        Storage.removeDir(("/.crosspoint/html_" + std::to_string(hash)).c_str());
      }
    }
  }

  LOG_INF("INS", "Sync done: %d items — free=%u maxAlloc=%u", static_cast<int>(displayList.size()),
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());
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
      const int toOpen = pendingOpenIdx;
      showStopModal = false;
      pendingOpenIdx = -1;
      abortDownload = true;
      // Open directly — do NOT call openArticle() here. openArticle() checks
      // activeDownloadIdx/downloadQueue, which are still set because the task
      // hasn't exited yet (abortDownload is just a signal). Calling it would
      // re-trigger this same modal in an infinite loop.
      if (toOpen >= 0 && toOpen < static_cast<int>(displayList.size()) &&
          displayList[toOpen].downloaded) {
        activityManager.goToReader(getArticlePath(displayList[toOpen]));
      }
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
      activityManager.goHome();
    }
    return;
  }

  if (state == State::BROWSING) {
    // Force-sync restart in progress: wait for tasks to exit cleanly (so SD
    // files are properly closed) before calling ESP.restart(). Block all
    // button handling while waiting — restart is imminent.
    if (pendingRestart) {
      if (downloadTaskHandle == nullptr && syncTaskHandle == nullptr) {
        // RTC_DATA_ATTR is only preserved across deep sleep on ESP32-C3, NOT
        // across ESP.restart() (software reset re-initialises .rtc.data).
        // A 1ms timer-wakeup deep sleep gives a clean heap AND preserves the
        // rtcGoToInstapaper flag so setup() routes straight back to Instapaper.
        LOG_INF("INS", "Force-sync: tasks done, saving queue and entering deep sleep");
        saveQueueFile();
        esp_sleep_enable_timer_wakeup(1000);  // 1 ms
        esp_deep_sleep_start();
      }
      return;
    }

    // Restart download task if items are still queued but the task died (e.g., transient WiFi failure).
    // Queued items are preserved on non-abort failure; this watchdog re-launches the task automatically
    // once the user is back in BROWSING state (after dismissing any error or just waiting).
    // Guard on !syncing (set in trampoline after backgroundSyncWork returns), not just syncComplete.
    // syncComplete is set while renderingMutex and the SPI bus mutex are still held by the sync task;
    // spawning the download task at that moment causes Storage.mkdir() to contend for the SPI mutex,
    // corrupting FreeRTOS priority-inheritance bookkeeping → xTaskPriorityDisinherit assert.
    if (!downloadQueue.empty() && downloadTaskHandle == nullptr && !abortDownload &&
        syncComplete && !syncing) {
      xTaskCreate(&InstapaperActivity::downloadTaskTrampoline, "InstaDownload", 8192, this, 1, &downloadTaskHandle);
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      activityManager.goHome();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!displayList.empty()) {
        openArticle(selectorIndex);
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      // Force sync: restart to clear fragmented heap, then re-enter Instapaper
      // with clean RAM so listBookmarks has enough contiguous memory for TLS.
      // Signal abort so tasks notice and close any open SD files before we
      // restart. pendingRestart=true causes loop() to poll for task exit and
      // then call ESP.restart() — avoiding a hard reset mid-SD-write.
      LOG_INF("INS", "Force-sync: pending restart — dl=%p sync=%p", downloadTaskHandle, syncTaskHandle);
      rtcGoToInstapaper = true;
      exitingActivity = true;  // re-queue mid-download items (same as Back-button exit)
      abortDownload = true;
      pendingRestart = true;
      updateRequired = true;
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
    bool needStart = (downloadTaskHandle == nullptr && !syncing);
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
      activityManager.goToReader(getArticlePath(bm));
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


void InstapaperActivity::backgroundDownloadWork() {
  LOG_INF("INS", "Download task start — free=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
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

  // WiFi is owned by the sync task.  After sync completes, the connection
  // should already be live.  If it is not (sync failed to connect, no
  // credentials, etc.) downloads cannot proceed — leave the queue intact so
  // the next session retries, and bail.
  if (WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
    LOG_ERR("INS", "Download: WiFi not connected after sync — deferring queue (free=%u maxAlloc=%u)",
            ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    return;
  }
  LOG_INF("INS", "Download: WiFi ready — free=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());

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

    LOG_INF("INS", "Article '%s' done — free=%u maxAlloc=%u", bmCopy.title.c_str(), ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    if (idx < static_cast<int>(displayList.size())) {
      displayList[idx].downloading = false;
      displayList[idx].dlCurrent = 0;
      displayList[idx].dlTotal = 0;
      if (bmCopy.downloaded) {
        displayList[idx].downloaded = true;
        displayList[idx].filename = bmCopy.filename;
      } else if (!abortDownload) {
        // Transient failure (heap OOM, network) — re-mark as queued so
        // saveQueueFile() persists it and the next session retries automatically.
        displayList[idx].queued = true;
        downloadQueue.push_back(idx);
      } else if (exitingActivity) {
        // Activity is exiting (user pressed Back) — re-queue so saveQueueFile()
        // persists it. This is NOT a user-directed cancel (toggleQueue sets
        // abortDownload=true without exitingActivity, which correctly drops the item).
        displayList[idx].queued = true;
        downloadQueue.push_back(idx);
      }
    }
    activeDownloadIdx = -1;
    xSemaphoreGive(renderingMutex);
    updateRequired = true;

    // Safety: if heap is unexpectedly low after a failed download, stop early.
    // Clear downloadQueue so the loop() watchdog does not immediately respawn
    // this task — the fragmented heap would just fail again. Items remain
    // queued in displayList (queued=true) and are persisted by saveQueueFile()
    // on exit, so the next session (clean heap) will retry them.
    if (!abortDownload && !bmCopy.downloaded && ESP.getMaxAllocHeap() < 50000) {
      LOG_ERR("INS", "Download: heap too low (maxAlloc=%u), deferring %d item(s) to next session",
              ESP.getMaxAllocHeap(), static_cast<int>(downloadQueue.size()));
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      downloadQueue.clear();
      xSemaphoreGive(renderingMutex);
      break;
    }

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
