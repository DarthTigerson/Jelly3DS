#include "jelly/config.hpp"
#include "jelly/util.hpp"

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
    continueView = j.value("continueView", "shelf");
    return true;
}

bool Config::save(const std::string& path) const {
    nlohmann::json j = {
        {"serverUrl", serverUrl},         {"userId", userId},
        {"userName", userName},           {"accessToken", accessToken},
        {"deviceId", deviceId},           {"profile", profileName(profile)},
        {"audioLanguage", audioLanguage}, {"subtitleLanguage", subtitleLanguage},
        {"verifyTls", verifyTls},         {"continueView", continueView},
    };
    return saveJson(path, j);
}

}  // namespace jelly
