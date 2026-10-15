#pragma once

#include <map>
#include <string>

namespace jelly {

enum class ProfileChoice { Auto, New3DS, Old3DS };

struct Config {
    std::string serverUrl;
    std::string userId;
    std::string userName;
    std::string accessToken;
    std::string deviceId;
    ProfileChoice profile = ProfileChoice::Auto;
    std::string audioLanguage;     // preferred audio language (ISO 639-2), empty = server default
    std::string subtitleLanguage;  // preferred subtitle language to burn in, empty = none
    bool verifyTls = false;        // home servers commonly use self-signed certificates
    int volumeBoost = 2;           // 1 = normal, 2 = 2x, 3 = 3x (soft-limited so it never crackles)
    /// Cover view chosen per page ("continue", "tvshows", "movies") → "shelf" | "grid" | "hero".
    std::map<std::string, std::string> views;

    std::string view(const std::string& page, const std::string& fallback) const {
        auto it = views.find(page);
        return it == views.end() ? fallback : it->second;
    }

    bool loggedIn() const { return !serverUrl.empty() && !accessToken.empty() && !userId.empty(); }

    bool load(const std::string& path);
    bool save(const std::string& path) const;
};

}  // namespace jelly
