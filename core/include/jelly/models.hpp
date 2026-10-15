#pragma once

#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace jelly {

struct UserData {
    int64_t playbackPositionTicks = 0;
    bool played = false;
    int64_t lastPlayedDate = 0;  // unix seconds, 0 = never
    int unplayedItemCount = 0;
};

struct MediaStream {
    int index = -1;
    std::string type;      // "Video" | "Audio" | "Subtitle"
    std::string language;  // ISO 639-2, may be empty
    std::string displayTitle;
    std::string codec;
    bool isDefault = false;
    bool isExternal = false;
};

struct MediaSource {
    std::string id;
    std::string container;
    int64_t size = 0;
    std::vector<MediaStream> streams;
    int defaultAudioStreamIndex = -1;
    int defaultSubtitleStreamIndex = -1;
};

/// The subset of BaseItemDto Jelly3DS needs.
struct Item {
    std::string id;
    std::string name;
    std::string type;  // Series, Season, Episode, Movie, CollectionFolder, Folder, BoxSet ...
    std::string collectionType;
    std::string seriesId;
    std::string seriesName;
    std::string seasonId;
    std::string overview;
    int parentIndexNumber = 0;  // season number for episodes
    int indexNumber = 0;        // episode number
    int productionYear = 0;
    int64_t runTimeTicks = 0;
    int childCount = 0;
    bool isFolder = false;
    std::string primaryImageTag;
    std::string seriesPrimaryImageTag;  // episodes: the show's poster
    std::string backdropItemId;         // item owning the backdrop (itself, or the series for episodes)
    std::string backdropImageTag;
    UserData userData;
    std::vector<MediaSource> mediaSources;

    bool isPlayable() const { return type == "Episode" || type == "Movie" || type == "Video" || type == "MusicVideo"; }
    /// "S01E02 · Name" for episodes, "Name (2023)" for movies, name otherwise.
    std::string label() const;
};

struct User {
    std::string id;
    std::string name;
};

struct AuthResult {
    User user;
    std::string accessToken;
    std::string serverId;
};

void from_json(const nlohmann::json& j, UserData& v);
void from_json(const nlohmann::json& j, MediaStream& v);
void from_json(const nlohmann::json& j, MediaSource& v);
void from_json(const nlohmann::json& j, Item& v);

void to_json(nlohmann::json& j, const MediaStream& v);

}  // namespace jelly
