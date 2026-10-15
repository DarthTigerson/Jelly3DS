#include "jelly/config.hpp"
#include "jelly/util.hpp"

#include <algorithm>

namespace jelly {

static const char* profileName(ProfileChoice p) {
    switch (p) {
    case ProfileChoice::New3DS: return "new3ds";
    case ProfileChoice::Old3DS: return "old3ds";
    default: return "auto";
    }
}

bool Config::load(const std::string& path) {
    nlohmann::json j;
    if (!loadJson(path, j) || !j.is_object()) return false;
    serverUrl = j.value("serverUrl", "");
    userId = j.value("userId", "");
    userName = j.value("userName", "");
    accessToken = j.value("accessToken", "");
    deviceId = j.value("deviceId", "");
    std::string p = j.value("profile", "auto");
    profile = p == "new3ds" ? ProfileChoice::New3DS : p == "old3ds" ? ProfileChoice::Old3DS : ProfileChoice::Auto;
    audioLanguage = j.value("audioLanguage", "");
    subtitleLanguage = j.value("subtitleLanguage", "");
    verifyTls = j.value("verifyTls", false);
    volumeBoost = std::max(1, std::min(MAX_VOLUME_BOOST, j.value("volumeBoost", 2)));
    views.clear();
    if (j.contains("views") && j["views"].is_object())
        for (auto& [k, v] : j["views"].items())
            if (v.is_string()) views[k] = v.get<std::string>();
    return true;
}

bool Config::save(const std::string& path) const {
    nlohmann::json j = {
        {"serverUrl", serverUrl},         {"userId", userId},
        {"userName", userName},           {"accessToken", accessToken},
        {"deviceId", deviceId},           {"profile", profileName(profile)},
        {"audioLanguage", audioLanguage}, {"subtitleLanguage", subtitleLanguage},
        {"verifyTls", verifyTls},         {"views", views},
        {"volumeBoost", volumeBoost},
    };
    return saveJson(path, j);
}

}  // namespace jelly
