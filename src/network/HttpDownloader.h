#pragma once
#include <HalStorage.h>

#include <functional>
#include <string>

/**
 * HTTP client utility for fetching content and downloading files.
 * Wraps WiFiClientSecure and HTTPClient for HTTPS requests.
 */
class HttpDownloader {
 public:
  using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;

  enum DownloadError {
    OK = 0,
    HTTP_ERROR,
    FILE_ERROR,
    ABORTED,
  };

  /**
   * Fetch text content from a URL.
   * @param url The URL to fetch
   * @param outContent The fetched content (output)
   * @return true if fetch succeeded, false on error
   */
  static bool fetchUrl(const std::string& url, std::string& outContent);

  static bool fetchUrl(const std::string& url, Stream& stream);

  /**
   * POST to a URL and get the response body.
   * @param url The URL to POST to
   * @param body URL-encoded POST body
   * @param authHeader Authorization header value (e.g. "OAuth ...")
   * @param outContent The response content (output)
   * @param maxBytes If > 0, stop reading after this many bytes (prevents OOM on large responses)
   * @return true if request succeeded, false on error
   */
  static bool postUrl(const std::string& url, const std::string& body, const std::string& authHeader,
                      std::string& outContent, size_t maxBytes = 0,
                      ProgressCallback progress = nullptr);

  /**
   * Download a file to the SD card.
   * @param url The URL to download
   * @param destPath The destination path on SD card
   * @param progress Optional progress callback
   * @return DownloadError indicating success or failure type
   */
  static DownloadError downloadToFile(const std::string& url, const std::string& destPath,
                                      ProgressCallback progress = nullptr);

 private:
  static constexpr size_t DOWNLOAD_CHUNK_SIZE = 1024;
};
