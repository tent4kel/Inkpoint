#include "HttpDownloader.h"

#include <HTTPClient.h>
#include <Logging.h>
#include <StreamString.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <base64.h>

#include <cstring>
#include <memory>

#include "CrossPointSettings.h"
#include "util/UrlUtils.h"

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent) {
  // Use WiFiClientSecure for HTTPS, regular WiFiClient for HTTP
  std::unique_ptr<WiFiClient> client;
  if (UrlUtils::isHttpsUrl(url)) {
    auto* secureClient = new WiFiClientSecure();
    secureClient->setInsecure();
    client.reset(secureClient);
  } else {
    client.reset(new WiFiClient());
  }
  HTTPClient http;

  LOG_DBG("HTTP", "Fetching: %s", url.c_str());

  http.begin(*client, url.c_str());
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);

  // Add Basic HTTP auth if credentials are configured
  if (strlen(SETTINGS.opdsUsername) > 0 && strlen(SETTINGS.opdsPassword) > 0) {
    std::string credentials = std::string(SETTINGS.opdsUsername) + ":" + SETTINGS.opdsPassword;
    String encoded = base64::encode(credentials.c_str());
    http.addHeader("Authorization", "Basic " + encoded);
  }

  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    LOG_ERR("HTTP", "Fetch failed: %d", httpCode);
    http.end();
    return false;
  }

  http.writeToStream(&outContent);

  http.end();

  LOG_DBG("HTTP", "Fetch success");
  return true;
}

bool HttpDownloader::fetchUrl(const std::string& url, std::string& outContent) {
  StreamString stream;
  if (!fetchUrl(url, stream)) {
    return false;
  }
  outContent = stream.c_str();
  return true;
}

bool HttpDownloader::postUrl(const std::string& url, const std::string& body, const std::string& authHeader,
                             std::string& outContent, size_t maxBytes,
                             ProgressCallback progress) {
  // Capped path (maxBytes > 0): pre-reserve the response string BEFORE creating the TLS context.
  //
  // Why: std::string::append() triggers doubling reallocations that require old+new buffers
  // simultaneously. On a fragmented heap (after multiple TLS alloc/free cycles from retries
  // or concurrent syncs), even 90+ KB total free may have no single contiguous 32 KB block.
  // That triggers __throw_bad_alloc() → abort() on no-exceptions builds (MCAUSE=0x7, MTVAL=0x0).
  //
  // Strategy — two steps, no large allocation after TLS is created:
  //   1. malloc(maxBytes) as a probe: validates a contiguous block exists; null = graceful fail.
  //   2. free(probe) then immediately outContent.reserve(maxBytes): on single-core RTOS there is
  //      no task switch between these two lines, so reserve() reuses the exact same block.
  //   3. TLS context created (~34 KB) from the remaining heap.
  //   4. Stream into outContent.append() — capacity already maxBytes, zero reallocations.
  //   5. http.end() + client.reset() free TLS. No further large malloc needed at all.
  if (maxBytes > 0) {
    char* probe = static_cast<char*>(malloc(maxBytes));
    if (!probe) {
      LOG_ERR("HTTP", "postUrl: no contiguous %zu B block (free: %u)", maxBytes, ESP.getFreeHeap());
      return false;
    }
    free(probe);                  // frees the block; no task switch before reserve() below
    outContent.clear();
    outContent.reserve(maxBytes); // reuses probe's block; capacity = maxBytes, no realloc during stream
  }

  std::unique_ptr<WiFiClient> client;
  if (UrlUtils::isHttpsUrl(url)) {
    auto* secureClient = new WiFiClientSecure();
    secureClient->setInsecure();
    client.reset(secureClient);
  } else {
    client.reset(new WiFiClient());
  }
  HTTPClient http;

  LOG_DBG("HTTP", "POST: %s", url.c_str());

  http.begin(*client, url.c_str());
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  if (!authHeader.empty()) {
    http.addHeader("Authorization", authHeader.c_str());
  }

  const int httpCode = http.POST(body.c_str());
  if (httpCode != HTTP_CODE_OK) {
    String responseBody = http.getString();
    LOG_ERR("HTTP", "POST failed: %d body: %s", httpCode, responseBody.c_str());
    http.end();
    return false;
  }

  const int contentLength = http.getSize();
  if (maxBytes == 0) outContent.clear();

  WiFiClient* stream = http.getStreamPtr();
  if (!stream) {
    LOG_ERR("HTTP", "POST: failed to get stream ptr");
    http.end();
    return false;
  }

  // Heap-allocate the chunk buffer to keep stack frame small (< 256 bytes)
  uint8_t* chunkBuf = static_cast<uint8_t*>(malloc(DOWNLOAD_CHUNK_SIZE));
  if (!chunkBuf) {
    LOG_ERR("HTTP", "POST: failed to alloc chunk buffer");
    http.end();
    return false;
  }

  while (http.connected() &&
         (contentLength <= 0 || outContent.size() < static_cast<size_t>(contentLength))) {
    const size_t available = stream->available();
    if (available == 0) {
      delay(1);
      continue;
    }
    size_t toRead = available < DOWNLOAD_CHUNK_SIZE ? available : DOWNLOAD_CHUNK_SIZE;
    if (maxBytes > 0 && outContent.size() + toRead > maxBytes) {
      toRead = maxBytes - outContent.size();
    }
    if (toRead == 0) break;
    const size_t bytesRead = stream->readBytes(chunkBuf, toRead);
    if (bytesRead == 0) break;
    outContent.append(reinterpret_cast<char*>(chunkBuf), bytesRead);
    if (progress) {
      const size_t total = contentLength > 0 ? static_cast<size_t>(contentLength) : maxBytes;
      progress(outContent.size(), total);
    }
  }

  free(chunkBuf);
  http.end();

  if (maxBytes > 0 && outContent.size() >= maxBytes) {
    LOG_INF("HTTP", "POST response capped at %zu bytes (free: %u)", maxBytes, ESP.getFreeHeap());
  } else {
    LOG_DBG("HTTP", "POST success (%zu bytes)", outContent.size());
  }
  return true;
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress) {
  // Use WiFiClientSecure for HTTPS, regular WiFiClient for HTTP
  std::unique_ptr<WiFiClient> client;
  if (UrlUtils::isHttpsUrl(url)) {
    auto* secureClient = new WiFiClientSecure();
    secureClient->setInsecure();
    client.reset(secureClient);
  } else {
    client.reset(new WiFiClient());
  }
  HTTPClient http;

  LOG_DBG("HTTP", "Downloading: %s", url.c_str());
  LOG_DBG("HTTP", "Destination: %s", destPath.c_str());

  http.begin(*client, url.c_str());
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);

  // Add Basic HTTP auth if credentials are configured
  if (strlen(SETTINGS.opdsUsername) > 0 && strlen(SETTINGS.opdsPassword) > 0) {
    std::string credentials = std::string(SETTINGS.opdsUsername) + ":" + SETTINGS.opdsPassword;
    String encoded = base64::encode(credentials.c_str());
    http.addHeader("Authorization", "Basic " + encoded);
  }

  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    LOG_ERR("HTTP", "Download failed: %d", httpCode);
    http.end();
    return HTTP_ERROR;
  }

  const size_t contentLength = http.getSize();
  LOG_DBG("HTTP", "Content-Length: %zu", contentLength);

  // Remove existing file if present
  if (Storage.exists(destPath.c_str())) {
    Storage.remove(destPath.c_str());
  }

  // Open file for writing
  FsFile file;
  if (!Storage.openFileForWrite("HTTP", destPath.c_str(), file)) {
    LOG_ERR("HTTP", "Failed to open file for writing");
    http.end();
    return FILE_ERROR;
  }

  // Get the stream for chunked reading
  WiFiClient* stream = http.getStreamPtr();
  if (!stream) {
    LOG_ERR("HTTP", "Failed to get stream");
    file.close();
    Storage.remove(destPath.c_str());
    http.end();
    return HTTP_ERROR;
  }

  // Download in chunks
  uint8_t buffer[DOWNLOAD_CHUNK_SIZE];
  size_t downloaded = 0;
  const size_t total = contentLength > 0 ? contentLength : 0;

  while (http.connected() && (contentLength == 0 || downloaded < contentLength)) {
    const size_t available = stream->available();
    if (available == 0) {
      delay(1);
      continue;
    }

    const size_t toRead = available < DOWNLOAD_CHUNK_SIZE ? available : DOWNLOAD_CHUNK_SIZE;
    const size_t bytesRead = stream->readBytes(buffer, toRead);

    if (bytesRead == 0) {
      break;
    }

    const size_t written = file.write(buffer, bytesRead);
    if (written != bytesRead) {
      LOG_ERR("HTTP", "Write failed: wrote %zu of %zu bytes", written, bytesRead);
      file.close();
      Storage.remove(destPath.c_str());
      http.end();
      return FILE_ERROR;
    }

    downloaded += bytesRead;

    if (progress && total > 0) {
      progress(downloaded, total);
    }
  }

  file.close();
  http.end();

  LOG_DBG("HTTP", "Downloaded %zu bytes", downloaded);

  // Verify download size if known
  if (contentLength > 0 && downloaded != contentLength) {
    LOG_ERR("HTTP", "Size mismatch: got %zu, expected %zu", downloaded, contentLength);
    Storage.remove(destPath.c_str());
    return HTTP_ERROR;
  }

  return OK;
}
