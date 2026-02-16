#include "InstapaperClient.h"

#include <Logging.h>

#include <map>

#include "InstapaperCredentialStore.h"
#include "InstapaperOAuth.h"
#include "InstapaperSecrets.h"
#include "HttpDownloader.h"

namespace {
constexpr char BASE_URL[] = "https://www.instapaper.com";

std::string urlEncode(const std::string& s) {
  std::string result;
  result.reserve(s.size() * 2);
  for (unsigned char c : s) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
        c == '.' || c == '~') {
      result += static_cast<char>(c);
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", c);
      result += buf;
    }
  }
  return result;
}

std::string buildBody(const std::map<std::string, std::string>& params) {
  std::string body;
  for (const auto& p : params) {
    if (!body.empty()) body += "&";
    body += urlEncode(p.first) + "=" + urlEncode(p.second);
  }
  return body;
}

// Parse URL-encoded response: key1=val1&key2=val2
std::map<std::string, std::string> parseUrlEncoded(const std::string& s) {
  std::map<std::string, std::string> result;
  size_t pos = 0;
  while (pos < s.size()) {
    size_t amp = s.find('&', pos);
    if (amp == std::string::npos) amp = s.size();
    size_t eq = s.find('=', pos);
    if (eq != std::string::npos && eq < amp) {
      result[s.substr(pos, eq - pos)] = s.substr(eq + 1, amp - eq - 1);
    }
    pos = amp + 1;
  }
  return result;
}

// Simple JSON string value extractor - finds "key":"value" or "key":number
std::string jsonExtract(const std::string& json, const std::string& key, size_t startPos = 0) {
  std::string search = "\"" + key + "\"";
  size_t pos = json.find(search, startPos);
  if (pos == std::string::npos) return "";

  pos += search.size();
  // Skip whitespace and colon
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':')) pos++;
  if (pos >= json.size()) return "";

  if (json[pos] == '"') {
    // String value
    pos++;
    std::string result;
    while (pos < json.size() && json[pos] != '"') {
      if (json[pos] == '\\' && pos + 1 < json.size()) {
        pos++;
        if (json[pos] == 'u' && pos + 4 < json.size()) {
          // Parse \uXXXX Unicode escape
          uint32_t cp = 0;
          for (int i = 1; i <= 4; i++) {
            char h = json[pos + i];
            cp <<= 4;
            if (h >= '0' && h <= '9') cp |= h - '0';
            else if (h >= 'a' && h <= 'f') cp |= 10 + h - 'a';
            else if (h >= 'A' && h <= 'F') cp |= 10 + h - 'A';
          }
          pos += 4;  // skip the 4 hex digits (pos++ at end of loop handles the 'u')
          // Handle surrogate pairs
          if (cp >= 0xD800 && cp <= 0xDBFF && pos + 2 < json.size() && json[pos + 1] == '\\' && json[pos + 2] == 'u') {
            uint32_t lo = 0;
            for (int i = 3; i <= 6 && pos + i < json.size(); i++) {
              char h = json[pos + i];
              lo <<= 4;
              if (h >= '0' && h <= '9') lo |= h - '0';
              else if (h >= 'a' && h <= 'f') lo |= 10 + h - 'a';
              else if (h >= 'A' && h <= 'F') lo |= 10 + h - 'A';
            }
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              pos += 6;  // skip \uXXXX of low surrogate
            }
          }
          // Encode codepoint as UTF-8
          if (cp < 0x80) {
            result += static_cast<char>(cp);
          } else if (cp < 0x800) {
            result += static_cast<char>(0xC0 | (cp >> 6));
            result += static_cast<char>(0x80 | (cp & 0x3F));
          } else if (cp < 0x10000) {
            result += static_cast<char>(0xE0 | (cp >> 12));
            result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            result += static_cast<char>(0x80 | (cp & 0x3F));
          } else {
            result += static_cast<char>(0xF0 | (cp >> 18));
            result += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            result += static_cast<char>(0x80 | (cp & 0x3F));
          }
        } else if (json[pos] == '"')
          result += '"';
        else if (json[pos] == '\\')
          result += '\\';
        else if (json[pos] == 'n')
          result += '\n';
        else
          result += json[pos];
      } else {
        result += json[pos];
      }
      pos++;
    }
    return result;
  } else {
    // Number or other literal
    size_t end = pos;
    while (end < json.size() && json[end] != ',' && json[end] != '}' && json[end] != ']') end++;
    return json.substr(pos, end - pos);
  }
}
template <typename Fn>
bool withRetries(Fn fn, int maxRetries = 3) {
  for (int i = 0; i < maxRetries; i++) {
    if (fn()) return true;
    if (i < maxRetries - 1) delay(1000 * (i + 1));
  }
  return false;
}
}  // namespace

bool InstapaperClient::authenticate(const std::string& username, const std::string& password, std::string& outToken,
                                    std::string& outTokenSecret) {
  std::string url = std::string(BASE_URL) + "/api/1/oauth/access_token";

  std::map<std::string, std::string> params;
  params["x_auth_username"] = username;
  params["x_auth_password"] = password;
  params["x_auth_mode"] = "client_auth";

  std::string authHeader = InstapaperOAuth::sign("POST", url, params, InstapaperSecrets::consumerKey(), InstapaperSecrets::consumerSecret(), "", "");
  std::string body = buildBody(params);
  std::string response;

  LOG_DBG("IPC", "Auth URL: %s", url.c_str());
  LOG_DBG("IPC", "Auth body: %s", body.c_str());
  LOG_DBG("IPC", "Auth header: %s", authHeader.c_str());

  if (!HttpDownloader::postUrl(url, body, authHeader, response)) {
    LOG_ERR("IPC", "Authentication failed");
    return false;
  }

  auto parsed = parseUrlEncoded(response);
  auto tokenIt = parsed.find("oauth_token");
  auto secretIt = parsed.find("oauth_token_secret");

  if (tokenIt == parsed.end() || secretIt == parsed.end()) {
    LOG_ERR("IPC", "Auth response missing tokens: %s", response.c_str());
    return false;
  }

  outToken = tokenIt->second;
  outTokenSecret = secretIt->second;
  LOG_DBG("IPC", "Authentication successful");
  return true;
}

bool InstapaperClient::listBookmarks(int limit, std::vector<InstapaperBookmark>& outBookmarks) {
  std::string url = std::string(BASE_URL) + "/api/1/bookmarks/list";

  std::map<std::string, std::string> params;
  params["limit"] = std::to_string(limit);

  const auto& token = INSTAPAPER_STORE.getToken();
  const auto& tokenSecret = INSTAPAPER_STORE.getTokenSecret();

  std::string authHeader = InstapaperOAuth::sign("POST", url, params, InstapaperSecrets::consumerKey(), InstapaperSecrets::consumerSecret(), token, tokenSecret);
  std::string body = buildBody(params);
  std::string response;

  bool ok = withRetries([&]() { return HttpDownloader::postUrl(url, body, authHeader, response); });
  if (!ok) {
    LOG_ERR("IPC", "List bookmarks failed after retries");
    return false;
  }

  // Parse JSON array - find bookmark objects with "type":"bookmark"
  outBookmarks.clear();
  size_t pos = 0;
  while (pos < response.size()) {
    size_t typePos = response.find("\"type\"", pos);
    if (typePos == std::string::npos) break;

    // Find the enclosing object start
    size_t objStart = response.rfind('{', typePos);
    if (objStart == std::string::npos) break;

    // Find the enclosing object end (simple: find matching })
    int depth = 1;
    size_t objEnd = objStart + 1;
    while (objEnd < response.size() && depth > 0) {
      if (response[objEnd] == '{')
        depth++;
      else if (response[objEnd] == '}')
        depth--;
      objEnd++;
    }

    std::string obj = response.substr(objStart, objEnd - objStart);
    std::string type = jsonExtract(obj, "type");

    if (type == "bookmark") {
      InstapaperBookmark bm;
      bm.bookmarkId = jsonExtract(obj, "bookmark_id");
      bm.title = jsonExtract(obj, "title");
      bm.url = jsonExtract(obj, "url");
      bm.time = atol(jsonExtract(obj, "time").c_str());

      if (!bm.bookmarkId.empty()) {
        outBookmarks.push_back(std::move(bm));
      }
    }

    pos = objEnd;
  }

  LOG_DBG("IPC", "Found %d bookmarks", outBookmarks.size());
  return true;
}

bool InstapaperClient::getArticleText(const std::string& bookmarkId, std::string& outHtml) {
  std::string url = std::string(BASE_URL) + "/api/1/bookmarks/get_text";

  std::map<std::string, std::string> params;
  params["bookmark_id"] = bookmarkId;

  const auto& token = INSTAPAPER_STORE.getToken();
  const auto& tokenSecret = INSTAPAPER_STORE.getTokenSecret();

  std::string authHeader = InstapaperOAuth::sign("POST", url, params, InstapaperSecrets::consumerKey(), InstapaperSecrets::consumerSecret(), token, tokenSecret);
  std::string body = buildBody(params);

  bool ok = withRetries([&]() { return HttpDownloader::postUrl(url, body, authHeader, outHtml); });
  if (!ok) {
    LOG_ERR("IPC", "Get article text failed for bookmark %s after retries", bookmarkId.c_str());
    return false;
  }

  LOG_DBG("IPC", "Got article text: %zu bytes", outHtml.size());
  return true;
}
