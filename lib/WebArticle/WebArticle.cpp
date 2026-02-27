#include "WebArticle.h"

#include <Logging.h>

#include <cstdlib>
#include <cstring>

WebArticle::WebArticle(std::string path, std::string cacheBasePath)
    : filepath(std::move(path)), cacheBasePath(std::move(cacheBasePath)) {
  const size_t hash = std::hash<std::string>{}(filepath);
  cachePath = this->cacheBasePath + "/html_" + std::to_string(hash);
}

bool WebArticle::load() {
  if (loaded) {
    return true;
  }

  if (!Storage.exists(filepath.c_str())) {
    LOG_ERR("WA ", "File does not exist: %s", filepath.c_str());
    return false;
  }

  FsFile file;
  if (!Storage.openFileForRead("WA ", filepath, file)) {
    LOG_ERR("WA ", "Failed to open file: %s", filepath.c_str());
    return false;
  }
  file.close();

  loaded = true;
  LOG_INF("WA ", "Loaded HTML file: %s", filepath.c_str());
  return true;
}

std::string WebArticle::getTitle() const {
  size_t lastSlash = filepath.find_last_of('/');
  std::string filename = (lastSlash != std::string::npos) ? filepath.substr(lastSlash + 1) : filepath;

  // Remove .html extension
  if (filename.length() >= 5 && filename.substr(filename.length() - 5) == ".html") {
    filename = filename.substr(0, filename.length() - 5);
  }

  return filename;
}

std::string WebArticle::getAuthor() const {
  FsFile mf;
  if (!Storage.openFileForRead("WA ", filepath + ".meta", mf)) return "";
  char* buf = static_cast<char*>(malloc(640));
  if (!buf) { mf.close(); return ""; }
  const int n = mf.read(buf, 639);
  mf.close();
  if (n <= 0) { free(buf); return ""; }
  buf[n] = '\0';
  // Format: title|source|timestamp — source is between last two '|'
  const char* s = buf;
  const char* p2 = strrchr(s, '|');
  if (!p2 || p2 == s) { free(buf); return ""; }
  const char* p1 = static_cast<const char*>(memrchr(s, '|', p2 - s));
  if (!p1) { free(buf); return ""; }
  std::string result(p1 + 1, p2 - p1 - 1);
  free(buf);
  return result;
}

bool WebArticle::setupCacheDir() const {
  if (!Storage.exists(cacheBasePath.c_str())) {
    if (!Storage.mkdir(cacheBasePath.c_str())) {
      LOG_ERR("WA ", "Failed to create base cache dir: %s", cacheBasePath.c_str());
      return false;
    }
  }
  if (!Storage.exists(cachePath.c_str())) {
    if (!Storage.mkdir(cachePath.c_str())) {
      LOG_ERR("WA ", "Failed to create cache dir: %s", cachePath.c_str());
      return false;
    }
  }
  return true;
}
