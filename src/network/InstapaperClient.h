#pragma once
#include <string>
#include <vector>
#include "HttpDownloader.h"

struct InstapaperBookmark {
  std::string bookmarkId;
  std::string title;
  std::string url;
  long time = 0;
};

class InstapaperClient {
 public:
  // Authenticate via xAuth, returns true and sets outToken/outTokenSecret on success
  static bool authenticate(const std::string& username, const std::string& password, std::string& outToken,
                           std::string& outTokenSecret);

  // List unread bookmarks (requires stored credentials)
  static bool listBookmarks(int limit, std::vector<InstapaperBookmark>& outBookmarks);

  // Get article HTML text
  static bool getArticleText(const std::string& bookmarkId, std::string& outHtml,
                             HttpDownloader::ProgressCallback progress = nullptr);
};
