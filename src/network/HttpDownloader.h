#pragma once
#include <HalStorage.h>

#include <functional>
#include <string>

/**
 * HTTP client utility for fetching content and downloading files.
 * Wraps NetworkClientSecure and HTTPClient for HTTPS requests.
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
   * Fetch for proxy use: neutral user-agent, follows one redirect level with
   * sequential (not concurrent) TLS connections to avoid heap OOM on devices
   * where two simultaneous TLS contexts (~34KB each) exceed available contiguous RAM.
   */
  // outRedirectUrl: if set on return, hop 0 got a redirect but hop 1 failed —
  // caller may return this URL to the browser for direct fetching (CORS required).
  static bool fetchUrlProxy(const std::string& url, std::string& outContent,
                             std::string& outRedirectUrl);

  /**
   * Download a file to the SD card.
   * @param url The URL to download
   * @param destPath The destination path on SD card
   * @param progress Optional progress callback
   * @return DownloadError indicating success or failure type
   */
  static DownloadError downloadToFile(const std::string& url, const std::string& destPath,
                                      ProgressCallback progress = nullptr);
};
