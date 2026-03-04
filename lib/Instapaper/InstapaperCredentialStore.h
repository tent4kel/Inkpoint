#pragma once
#include <string>

class InstapaperCredentialStore {
 private:
  static InstapaperCredentialStore instance;
  std::string token;
  std::string tokenSecret;
  std::string downloadFolder;
  bool archiveOldArticles = false;

  InstapaperCredentialStore() : downloadFolder("/instapaper") {}

  void obfuscate(std::string& data) const;

 public:
  InstapaperCredentialStore(const InstapaperCredentialStore&) = delete;
  InstapaperCredentialStore& operator=(const InstapaperCredentialStore&) = delete;

  static InstapaperCredentialStore& getInstance() { return instance; }

  bool saveToFile() const;
  bool loadFromFile();

  void setCredentials(const std::string& tok, const std::string& tokSecret);
  const std::string& getToken() const { return token; }
  const std::string& getTokenSecret() const { return tokenSecret; }

  bool hasCredentials() const;
  void clearCredentials();

  void setDownloadFolder(const std::string& folder) { downloadFolder = folder; }
  const std::string& getDownloadFolder() const { return downloadFolder; }

  void setArchiveOldArticles(bool v) { archiveOldArticles = v; }
  bool getArchiveOldArticles() const { return archiveOldArticles; }
};

#define INSTAPAPER_STORE InstapaperCredentialStore::getInstance()
