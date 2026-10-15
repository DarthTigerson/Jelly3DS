#pragma once

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
    std::string continueView = "shelf";  // Continue tab cover view: shelf | grid | hero

    bool loggedIn() const { return !serverUrl.empty() && !accessToken.empty() && !userId.empty(); }

    bool load(const std::string& path);
    bool save(const std::string& path) const;
};

}  // namespace jelly
