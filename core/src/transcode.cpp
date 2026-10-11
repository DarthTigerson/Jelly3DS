#include "jelly/transcode.hpp"
#include "jelly/util.hpp"

namespace jelly {

// New 3DS: hardware H.264 decoder (MVD) handles the full top-screen resolution.
DeviceProfile DeviceProfile::new3ds() { return {"new3ds", 400, 240, 30, 600000, 96000, 32000}; }

// Old 3DS: software decoding on a 268 MHz ARM11, so fewer pixels and frames.
DeviceProfile DeviceProfile::old3ds() { return {"old3ds", 320, 192, 20, 350000, 64000, 32000}; }

DeviceProfile DeviceProfile::byId(const std::string& id) { return id == "old3ds" ? old3ds() : new3ds(); }

DeviceProfile DeviceProfile::resolve(ProfileChoice choice, bool isNew3ds) {
    switch (choice) {
    case ProfileChoice::New3DS: return new3ds();
    case ProfileChoice::Old3DS: return old3ds();
    default: return isNew3ds ? new3ds() : old3ds();
    }
}

std::string buildStreamUrl(const std::string& server, const DeviceProfile& p, const StreamRequest& r) {
    std::vector<std::pair<std::string, std::string>> q = {
        {"static", "false"},
        {"mediaSourceId", r.mediaSourceId.empty() ? r.itemId : r.mediaSourceId},
        {"container", "ts"},
        {"videoCodec", "h264"},
        {"audioCodec", "aac"},
        {"maxWidth", std::to_string(p.maxWidth)},
        {"maxHeight", std::to_string(p.maxHeight)},
        {"maxFramerate", std::to_string(p.maxFramerate)},
        {"videoBitRate", std::to_string(p.videoBitrate)},
        {"audioBitRate", std::to_string(p.audioBitrate)},
        {"audioChannels", "2"},
        {"audioSampleRate", std::to_string(p.audioSampleRate)},
        // Both spellings: older servers read profile/level, newer ones the codec-prefixed form.
        {"profile", "baseline"},
        {"level", "30"},
        {"h264-profile", "baseline"},
        {"h264-level", "30"},
        {"allowVideoStreamCopy", "false"},
        {"allowAudioStreamCopy", "false"},
        {"enableAutoStreamCopy", "false"},
        {"deviceId", r.deviceId},
        {"playSessionId", r.playSessionId},
    };
    if (r.audioStreamIndex >= 0) q.emplace_back("audioStreamIndex", std::to_string(r.audioStreamIndex));
    if (r.subtitleStreamIndex >= 0) {
        q.emplace_back("subtitleStreamIndex", std::to_string(r.subtitleStreamIndex));
        q.emplace_back("subtitleMethod", "Encode");
    }
    if (r.startTimeTicks > 0) q.emplace_back("startTimeTicks", std::to_string(r.startTimeTicks));
    return server + "/Videos/" + r.itemId + "/stream.ts?" + buildQuery(q);
}

static bool sameLanguage(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return false;
    std::string x = a, y = b;
    for (auto& c : x) c = static_cast<char>(tolower(c));
    for (auto& c : y) c = static_cast<char>(tolower(c));
    return x == y;
}

int chooseAudioStream(const MediaSource& src, const std::string& lang) {
    int fallback = -1;
    for (const auto& s : src.streams) {
        if (s.type != "Audio") continue;
        if (sameLanguage(s.language, lang)) return s.index;
        if (fallback < 0) fallback = s.index;
    }
    if (src.defaultAudioStreamIndex >= 0) return src.defaultAudioStreamIndex;
    return fallback;
}

int chooseSubtitleStream(const MediaSource& src, const std::string& lang) {
    if (lang.empty()) return -1;
    for (const auto& s : src.streams)
        if (s.type == "Subtitle" && sameLanguage(s.language, lang)) return s.index;
    return -1;
}

}  // namespace jelly
