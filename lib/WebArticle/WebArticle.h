#pragma once

#include <HalStorage.h>

#include <string>

/**
 * WebArticle — wraps a saved .html article file (e.g. from Instapaper)
 * and manages its render cache directory.
 *
 * Mirrors the Markdown class: knows the file path + cache dir,
 * but renders via ChapterHtmlSlimParser (EPUB HTML pipeline) instead of MarkdownParser.
 */
class WebArticle {
  std::string filepath;
  std::string cacheBasePath;
  std::string cachePath;
  bool loaded = false;

 public:
  explicit WebArticle(std::string path, std::string cacheBasePath);

  bool load();
  [[nodiscard]] const std::string& getPath() const { return filepath; }
  [[nodiscard]] const std::string& getCachePath() const { return cachePath; }
  [[nodiscard]] std::string getTitle() const;
  [[nodiscard]] std::string getAuthor() const;  // reads source from .meta sidecar

  bool setupCacheDir() const;
};
