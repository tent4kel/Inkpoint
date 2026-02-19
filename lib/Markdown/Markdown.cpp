#include "Markdown.h"

#include <FsHelpers.h>
#include <JpegToBmpConverter.h>

Markdown::Markdown(std::string path, std::string cacheBasePath)
    : filepath(std::move(path)), cacheBasePath(std::move(cacheBasePath)) {
  const size_t hash = std::hash<std::string>{}(filepath);
  cachePath = this->cacheBasePath + "/md_" + std::to_string(hash);
}

bool Markdown::load() {
  if (loaded) {
    return true;
  }

  if (!Storage.exists(filepath.c_str())) {
    Serial.printf("[%lu] [MD ] File does not exist: %s\n", millis(), filepath.c_str());
    return false;
  }

  FsFile file;
  if (!Storage.openFileForRead("MD ", filepath, file)) {
    Serial.printf("[%lu] [MD ] Failed to open file: %s\n", millis(), filepath.c_str());
    return false;
  }

  fileSize = file.size();
  file.close();

  loaded = true;
  Serial.printf("[%lu] [MD ] Loaded MD file: %s (%zu bytes)\n", millis(), filepath.c_str(), fileSize);
  return true;
}

std::string Markdown::getTitle() const {
  size_t lastSlash = filepath.find_last_of('/');
  std::string filename = (lastSlash != std::string::npos) ? filepath.substr(lastSlash + 1) : filepath;

  // Remove .md or .markdown extension
  if (filename.length() >= 3 && filename.substr(filename.length() - 3) == ".md") {
    filename = filename.substr(0, filename.length() - 3);
  } else if (filename.length() >= 9 && filename.substr(filename.length() - 9) == ".markdown") {
    filename = filename.substr(0, filename.length() - 9);
  }

  // Strip language code suffix (e.g., "Article.de" → "Article")
  if (filename.size() > 3 && filename[filename.size() - 3] == '.' &&
      filename[filename.size() - 2] >= 'a' && filename[filename.size() - 2] <= 'z' &&
      filename[filename.size() - 1] >= 'a' && filename[filename.size() - 1] <= 'z') {
    filename = filename.substr(0, filename.size() - 3);
  }

  return filename;
}

std::string Markdown::getLanguage() const {
  // Extract language from filename pattern: "Title.XX.md" where XX is a 2-letter language code
  size_t lastSlash = filepath.find_last_of('/');
  std::string filename = (lastSlash != std::string::npos) ? filepath.substr(lastSlash + 1) : filepath;

  // Check for .XX.md pattern (e.g., "Article.de.md")
  if (filename.size() > 6) {
    size_t mdPos = filename.size() - 3;  // position of ".md"
    if (filename.substr(mdPos) == ".md" && filename[mdPos - 3] == '.' &&
        filename[mdPos - 2] >= 'a' && filename[mdPos - 2] <= 'z' &&
        filename[mdPos - 1] >= 'a' && filename[mdPos - 1] <= 'z') {
      return filename.substr(mdPos - 2, 2);
    }
  }
  return "en";
}

void Markdown::setupCacheDir() const {
  if (!Storage.exists(cacheBasePath.c_str())) {
    Storage.mkdir(cacheBasePath.c_str());
  }
  if (!Storage.exists(cachePath.c_str())) {
    Storage.mkdir(cachePath.c_str());
  }
}

std::string Markdown::findCoverImage() const {
  size_t lastSlash = filepath.find_last_of('/');
  std::string folder = (lastSlash != std::string::npos) ? filepath.substr(0, lastSlash) : "";
  if (folder.empty()) {
    folder = "/";
  }

  std::string baseName = getTitle();

  const char* extensions[] = {".bmp", ".jpg", ".jpeg", ".png", ".BMP", ".JPG", ".JPEG", ".PNG"};

  // First: image with same name as md file (e.g., myarticle.jpg for myarticle.md)
  for (const auto& ext : extensions) {
    std::string coverPath = folder + "/" + baseName + ext;
    if (Storage.exists(coverPath.c_str())) {
      Serial.printf("[%lu] [MD ] Found matching cover image: %s\n", millis(), coverPath.c_str());
      return coverPath;
    }
  }

  // Fallback: cover.* files
  const char* coverNames[] = {"cover", "Cover", "COVER"};
  for (const auto& name : coverNames) {
    for (const auto& ext : extensions) {
      std::string coverPath = folder + "/" + std::string(name) + ext;
      if (Storage.exists(coverPath.c_str())) {
        Serial.printf("[%lu] [MD ] Found fallback cover image: %s\n", millis(), coverPath.c_str());
        return coverPath;
      }
    }
  }

  return "";
}

std::string Markdown::getCoverBmpPath() const { return cachePath + "/cover.bmp"; }

bool Markdown::generateCoverBmp() const {
  if (Storage.exists(getCoverBmpPath().c_str())) {
    return true;
  }

  std::string coverImagePath = findCoverImage();
  if (coverImagePath.empty()) {
    Serial.printf("[%lu] [MD ] No cover image found for MD file\n", millis());
    return false;
  }

  setupCacheDir();

  const size_t len = coverImagePath.length();
  const bool isJpg =
      (len >= 4 && (coverImagePath.substr(len - 4) == ".jpg" || coverImagePath.substr(len - 4) == ".JPG")) ||
      (len >= 5 && (coverImagePath.substr(len - 5) == ".jpeg" || coverImagePath.substr(len - 5) == ".JPEG"));
  const bool isBmp = len >= 4 && (coverImagePath.substr(len - 4) == ".bmp" || coverImagePath.substr(len - 4) == ".BMP");

  if (isBmp) {
    Serial.printf("[%lu] [MD ] Copying BMP cover image to cache\n", millis());
    FsFile src, dst;
    if (!Storage.openFileForRead("MD ", coverImagePath, src)) {
      return false;
    }
    if (!Storage.openFileForWrite("MD ", getCoverBmpPath(), dst)) {
      src.close();
      return false;
    }
    uint8_t buffer[1024];
    while (src.available()) {
      size_t bytesRead = src.read(buffer, sizeof(buffer));
      dst.write(buffer, bytesRead);
    }
    src.close();
    dst.close();
    return true;
  }

  if (isJpg) {
    Serial.printf("[%lu] [MD ] Generating BMP from JPG cover image\n", millis());
    FsFile coverJpg, coverBmp;
    if (!Storage.openFileForRead("MD ", coverImagePath, coverJpg)) {
      return false;
    }
    if (!Storage.openFileForWrite("MD ", getCoverBmpPath(), coverBmp)) {
      coverJpg.close();
      return false;
    }
    const bool success = JpegToBmpConverter::jpegFileToBmpStream(coverJpg, coverBmp);
    coverJpg.close();
    coverBmp.close();

    if (!success) {
      Serial.printf("[%lu] [MD ] Failed to generate BMP from JPG cover image\n", millis());
      Storage.remove(getCoverBmpPath().c_str());
    }
    return success;
  }

  Serial.printf("[%lu] [MD ] Cover image format not supported (only BMP/JPG/JPEG)\n", millis());
  return false;
}

bool Markdown::readContent(uint8_t* buffer, size_t offset, size_t length) const {
  if (!loaded) {
    return false;
  }

  FsFile file;
  if (!Storage.openFileForRead("MD ", filepath, file)) {
    return false;
  }

  if (!file.seek(offset)) {
    file.close();
    return false;
  }

  size_t bytesRead = file.read(buffer, length);
  file.close();

  return bytesRead > 0;
}
