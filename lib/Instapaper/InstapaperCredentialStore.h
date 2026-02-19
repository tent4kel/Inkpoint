#pragma once
#include <string>

class InstapaperCredentialStore {
 private:
  static InstapaperCredentialStore instance;
  std::string username;
  std::string password;
  std::string token;
  std::string tokenSecret;
  std::string downloadFolder;

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

  void setUsername(const std::string& user) { username = user; }
  const std::string& getUsername() const { return username; }
  void setPassword(const std::string& pass) { password = pass; }
  const std::string& getPassword() const { return password; }

  bool hasCredentials() const;
  bool hasLoginCredentials() const;
  void clearCredentials();

  void setDownloadFolder(const std::string& folder) { downloadFolder = folder; }
  const std::string& getDownloadFolder() const { return downloadFolder; }
};

#define INSTAPAPER_STORE InstapaperCredentialStore::getInstance()
