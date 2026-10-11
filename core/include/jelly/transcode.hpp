#pragma once

#include "jelly/config.hpp"
#include "jelly/models.hpp"

#include <cstdint>
#include <string>

namespace jelly {

/// What Jellyfin must convert a video into before it reaches the console.
struct DeviceProfile {
    std::string id;  // "new3ds" | "old3ds" (stored with each download)
    int maxWidth;
    int maxHeight;
    int maxFramerate;
    int videoBitrate;
    int audioBitrate;
    int audioSampleRate;

    static DeviceProfile new3ds();
    static DeviceProfile old3ds();
    static DeviceProfile byId(const std::string& id);
    static DeviceProfile resolve(ProfileChoice choice, bool isNew3ds);
};

struct StreamRequest {
    std::string itemId;
    std::string mediaSourceId;
    int audioStreamIndex = -1;     // -1 = server default
    int subtitleStreamIndex = -1;  // -1 = no subtitles; otherwise burned into the video
    int64_t startTimeTicks = 0;
    std::string deviceId;
    std::string playSessionId;
};

/// Progressive MPEG-TS (H.264 Constrained Baseline + AAC stereo) transcode URL. Authentication is sent as a header.
std::string buildStreamUrl(const std::string& server, const DeviceProfile& profile, const StreamRequest& req);

/// Picks the audio track: preferred language first, then the source default, then the first audio track.
int chooseAudioStream(const MediaSource& src, const std::string& preferredLanguage);
/// Picks the subtitle track to burn in for a language, -1 for none / not found.
int chooseSubtitleStream(const MediaSource& src, const std::string& preferredLanguage);

}  // namespace jelly
