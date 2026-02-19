#pragma once
#include <map>
#include <string>

namespace InstapaperOAuth {

// Build OAuth 1.0a Authorization header with HMAC-SHA1 signature
std::string sign(const std::string& method, const std::string& url,
                 const std::map<std::string, std::string>& params, const std::string& consumerKey,
                 const std::string& consumerSecret, const std::string& token, const std::string& tokenSecret);

}  // namespace InstapaperOAuth
