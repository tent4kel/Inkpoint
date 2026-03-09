#include "HttpDownloader.h"

#include <Esp.h>
#include <HTTPClient.h>
#include <Logging.h>
#include <NetworkClient.h>
#include <NetworkClientSecure.h>
#include <StreamString.h>
#include <base64.h>

#include <cstring>
#include <memory>
#include <utility>

#include "CrossPointSettings.h"
#include "util/UrlUtils.h"

// Minimum contiguous block required before attempting a TLS connection.
// mbedTLS context + record buffers typically consume 25-40 KB.  If the heap
// is more fragmented than this the allocation would either fail (nothrow) or
// succeed and leave too little room for the response buffer + other tasks,
// resulting in MinFree < 2 KB and a frozen device.
static constexpr uint32_t TLS_MIN_HEAP = 50000;

static bool hasTlsHeap() {
  const uint32_t maxAlloc = ESP.getMaxAllocHeap();
  if (maxAlloc < TLS_MIN_HEAP) {
    LOG_ERR("HTTP", "Insufficient heap for TLS: MaxAlloc=%u", maxAlloc);
    return false;
  }
  return true;
}

namespace {
class FileWriteStream final : public Stream {
 public:
  FileWriteStream(FsFile& file, size_t total, HttpDownloader::ProgressCallback progress)
      : file_(file), total_(total), progress_(std::move(progress)) {}

  size_t write(uint8_t byte) override { return write(&byte, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    // Write-through stream for HTTPClient::writeToStream with progress tracking.
    const size_t written = file_.write(buffer, size);
    if (written != size) {
      writeOk_ = false;
    }
    downloaded_ += written;
    if (progress_ && total_ > 0) {
      progress_(downloaded_, total_);
    }
    return written;
  }

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override { file_.flush(); }

  size_t downloaded() const { return downloaded_; }
  bool ok() const { return writeOk_; }

 private:
  FsFile& file_;
  size_t total_;
  size_t downloaded_ = 0;
  bool writeOk_ = true;
  HttpDownloader::ProgressCallback progress_;
};
}  // namespace

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent) {
  if (UrlUtils::isHttpsUrl(url) && !hasTlsHeap()) return false;
  std::unique_ptr<NetworkClient> client;
  if (UrlUtils::isHttpsUrl(url)) {
    auto* secureClient = new (std::nothrow) NetworkClientSecure();
    if (!secureClient) {
      LOG_ERR("HTTP", "OOM allocating TLS client");
      return false;
    }
    secureClient->setInsecure();
    client.reset(secureClient);
  } else {
    auto* plainClient = new (std::nothrow) NetworkClient();
    if (!plainClient) {
      LOG_ERR("HTTP", "OOM allocating HTTP client");
      return false;
    }
    client.reset(plainClient);
  }
  HTTPClient http;

  LOG_DBG("HTTP", "Fetching: %s", url.c_str());

  http.begin(*client, url.c_str());
  http.setTimeout(8000);
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
  if (UrlUtils::isHttpsUrl(url) && !hasTlsHeap()) return false;
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

  std::unique_ptr<NetworkClient> client;
  if (UrlUtils::isHttpsUrl(url)) {
    auto* secureClient = new (std::nothrow) NetworkClientSecure();
    if (!secureClient) {
      LOG_ERR("HTTP", "OOM allocating TLS client");
      return false;
    }
    secureClient->setInsecure();
    client.reset(secureClient);
  } else {
    auto* plainClient = new (std::nothrow) NetworkClient();
    if (!plainClient) {
      LOG_ERR("HTTP", "OOM allocating HTTP client");
      return false;
    }
    client.reset(plainClient);
  }
  HTTPClient http;

  LOG_DBG("HTTP", "POST: %s", url.c_str());

  http.begin(*client, url.c_str());
  http.setTimeout(8000);
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

  NetworkClient* stream = http.getStreamPtr();
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

HttpDownloader::DownloadError HttpDownloader::postUrlToFile(const std::string& url, const std::string& body,
                                                             const std::string& authHeader,
                                                             const std::string& destPath,
                                                             ProgressCallback progress,
                                                             std::function<bool()> abortCheck) {
  if (UrlUtils::isHttpsUrl(url) && !hasTlsHeap()) return HTTP_ERROR;
  std::unique_ptr<NetworkClient> client;
  if (UrlUtils::isHttpsUrl(url)) {
    auto* secureClient = new (std::nothrow) NetworkClientSecure();
    if (!secureClient) {
      LOG_ERR("HTTP", "OOM allocating TLS client");
      return HTTP_ERROR;
    }
    secureClient->setInsecure();
    client.reset(secureClient);
  } else {
    auto* plainClient = new (std::nothrow) NetworkClient();
    if (!plainClient) {
      LOG_ERR("HTTP", "OOM allocating HTTP client");
      return HTTP_ERROR;
    }
    client.reset(plainClient);
  }
  HTTPClient http;

  LOG_DBG("HTTP", "POST to file: %s -> %s", url.c_str(), destPath.c_str());

  http.begin(*client, url.c_str());
  http.setTimeout(8000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  if (!authHeader.empty()) {
    http.addHeader("Authorization", authHeader.c_str());
  }

  const int httpCode = http.POST(body.c_str());
  if (httpCode != HTTP_CODE_OK) {
    // Log a snippet of the error body for diagnostics (small, 256-byte buffer)
    uint8_t* errBuf = static_cast<uint8_t*>(malloc(256));
    if (errBuf) {
      NetworkClient* errStream = http.getStreamPtr();
      size_t errRead = 0;
      if (errStream) {
        errRead = errStream->readBytes(errBuf, 255);
      }
      errBuf[errRead] = '\0';
      LOG_ERR("HTTP", "POST to file failed: %d body: %s", httpCode, reinterpret_cast<char*>(errBuf));
      free(errBuf);
    } else {
      LOG_ERR("HTTP", "POST to file failed: %d", httpCode);
    }
    http.end();
    return HTTP_ERROR;
  }

  const size_t contentLength = http.getSize();
  LOG_DBG("HTTP", "POST Content-Length: %zu", contentLength);

  // Remove existing file if present
  if (Storage.exists(destPath.c_str())) {
    Storage.remove(destPath.c_str());
  }

  FsFile file;
  if (!Storage.openFileForWrite("HTTP", destPath.c_str(), file)) {
    LOG_ERR("HTTP", "POST to file: failed to open dest file");
    http.end();
    return FILE_ERROR;
  }

  NetworkClient* stream = http.getStreamPtr();
  if (!stream) {
    LOG_ERR("HTTP", "POST to file: failed to get stream ptr");
    file.close();
    Storage.remove(destPath.c_str());
    http.end();
    return HTTP_ERROR;
  }

  // Heap-allocate chunk buffer — keeps stack frame small
  uint8_t* chunkBuf = static_cast<uint8_t*>(malloc(DOWNLOAD_CHUNK_SIZE));
  if (!chunkBuf) {
    LOG_ERR("HTTP", "POST to file: failed to alloc chunk buffer");
    file.close();
    Storage.remove(destPath.c_str());
    http.end();
    return FILE_ERROR;
  }

  size_t downloaded = 0;
  while (http.connected() && (contentLength == 0 || downloaded < contentLength)) {
    const size_t available = stream->available();
    if (available == 0) {
      delay(1);
      continue;
    }
    const size_t toRead = available < DOWNLOAD_CHUNK_SIZE ? available : DOWNLOAD_CHUNK_SIZE;
    const size_t bytesRead = stream->readBytes(chunkBuf, toRead);
    if (bytesRead == 0) break;

    const size_t written = file.write(chunkBuf, bytesRead);
    if (written != bytesRead) {
      LOG_ERR("HTTP", "POST to file: write failed (%zu of %zu)", written, bytesRead);
      free(chunkBuf);
      file.close();
      Storage.remove(destPath.c_str());
      http.end();
      return FILE_ERROR;
    }
    downloaded += bytesRead;
    if (progress && contentLength > 0) {
      progress(downloaded, contentLength);
    }
    if (abortCheck && abortCheck()) {
      free(chunkBuf);
      file.close();
      Storage.remove(destPath.c_str());
      http.end();
      return ABORTED;
    }
  }

  free(chunkBuf);
  file.close();
  http.end();

  LOG_DBG("HTTP", "POST to file: %zu bytes -> %s", downloaded, destPath.c_str());
  return OK;
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress) {
  if (UrlUtils::isHttpsUrl(url) && !hasTlsHeap()) return HTTP_ERROR;
  // Use NetworkClientSecure for HTTPS, regular NetworkClient for HTTP
  std::unique_ptr<NetworkClient> client;
  if (UrlUtils::isHttpsUrl(url)) {
    auto* secureClient = new (std::nothrow) NetworkClientSecure();
    if (!secureClient) {
      LOG_ERR("HTTP", "OOM allocating TLS client");
      return HTTP_ERROR;
    }
    secureClient->setInsecure();
    client.reset(secureClient);
  } else {
    auto* plainClient = new (std::nothrow) NetworkClient();
    if (!plainClient) {
      LOG_ERR("HTTP", "OOM allocating HTTP client");
      return HTTP_ERROR;
    }
    client.reset(plainClient);
  }
  HTTPClient http;

  LOG_DBG("HTTP", "Downloading: %s", url.c_str());
  LOG_DBG("HTTP", "Destination: %s", destPath.c_str());

  http.begin(*client, url.c_str());
  http.setTimeout(8000);
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

  const int64_t reportedLength = http.getSize();
  const size_t contentLength = reportedLength > 0 ? static_cast<size_t>(reportedLength) : 0;
  if (contentLength > 0) {
    LOG_DBG("HTTP", "Content-Length: %zu", contentLength);
  } else {
    LOG_DBG("HTTP", "Content-Length: unknown");
  }

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

  // Let HTTPClient handle chunked decoding and stream body bytes into the file.
  FileWriteStream fileStream(file, contentLength, progress);
  const int writeResult = http.writeToStream(&fileStream);

  file.close();
  http.end();

  if (writeResult < 0) {
    LOG_ERR("HTTP", "writeToStream error: %d", writeResult);
    Storage.remove(destPath.c_str());
    return HTTP_ERROR;
  }

  const size_t downloaded = fileStream.downloaded();
  LOG_DBG("HTTP", "Downloaded %zu bytes", downloaded);

  // Guard against partial writes even if HTTPClient completes.
  if (!fileStream.ok()) {
    LOG_ERR("HTTP", "Write failed during download");
    Storage.remove(destPath.c_str());
    return FILE_ERROR;
  }

  if (contentLength == 0 && downloaded == 0) {
    LOG_ERR("HTTP", "Download failed: no data received");
    Storage.remove(destPath.c_str());
    return HTTP_ERROR;
  }

  // Verify download size if known
  if (contentLength > 0 && downloaded != contentLength) {
    LOG_ERR("HTTP", "Size mismatch: got %zu, expected %zu", downloaded, contentLength);
    Storage.remove(destPath.c_str());
    return HTTP_ERROR;
  }

  return OK;
}
