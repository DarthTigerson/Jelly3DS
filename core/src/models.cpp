#include "jelly/models.hpp"
#include "jelly/util.hpp"

#include <cstdio>

namespace jelly {

namespace {

template <typename T>
T get(const nlohmann::json& j, const char* key, T fallback) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return fallback;
    try {
        return it->get<T>();
    } catch (...) {
        return fallback;
    }
}

}  // namespace

std::string Item::label() const {
    if (type == "Episode") {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "S%02dE%02d", parentIndexNumber, indexNumber);
        return std::string(buf) + " \xC2\xB7 " + name;
    }
    if (productionYear > 0 && (type == "Movie" || type == "Series")) return name + " (" + std::to_string(productionYear) + ")";
    return name;
}

void from_json(const nlohmann::json& j, UserData& v) {
    v.playbackPositionTicks = get<int64_t>(j, "PlaybackPositionTicks", 0);
    v.played = get<bool>(j, "Played", false);
    v.lastPlayedDate = parseIsoDate(get<std::string>(j, "LastPlayedDate", ""));
    v.unplayedItemCount = get<int>(j, "UnplayedItemCount", 0);
}

void from_json(const nlohmann::json& j, MediaStream& v) {
    v.index = get<int>(j, "Index", -1);
    v.type = get<std::string>(j, "Type", "");
    v.language = get<std::string>(j, "Language", "");
    v.displayTitle = get<std::string>(j, "DisplayTitle", "");
    v.codec = get<std::string>(j, "Codec", "");
    v.isDefault = get<bool>(j, "IsDefault", false);
    v.isExternal = get<bool>(j, "IsExternal", false);
}

void to_json(nlohmann::json& j, const MediaStream& v) {
    j = {{"Index", v.index},           {"Type", v.type},   {"Language", v.language},
         {"DisplayTitle", v.displayTitle}, {"Codec", v.codec}, {"IsDefault", v.isDefault},
         {"IsExternal", v.isExternal}};
}

void from_json(const nlohmann::json& j, MediaSource& v) {
    v.id = get<std::string>(j, "Id", "");
    v.container = get<std::string>(j, "Container", "");
    v.size = get<int64_t>(j, "Size", 0);
    v.defaultAudioStreamIndex = get<int>(j, "DefaultAudioStreamIndex", -1);
    v.defaultSubtitleStreamIndex = get<int>(j, "DefaultSubtitleStreamIndex", -1);
    v.streams = get<std::vector<MediaStream>>(j, "MediaStreams", {});
}

void from_json(const nlohmann::json& j, Item& v) {
    v.id = get<std::string>(j, "Id", "");
    v.name = get<std::string>(j, "Name", "");
    v.type = get<std::string>(j, "Type", "");
    v.collectionType = get<std::string>(j, "CollectionType", "");
    v.seriesId = get<std::string>(j, "SeriesId", "");
    v.seriesName = get<std::string>(j, "SeriesName", "");
    v.seasonId = get<std::string>(j, "SeasonId", "");
    v.overview = get<std::string>(j, "Overview", "");
    v.parentIndexNumber = get<int>(j, "ParentIndexNumber", 0);
    v.indexNumber = get<int>(j, "IndexNumber", 0);
    v.productionYear = get<int>(j, "ProductionYear", 0);
    v.runTimeTicks = get<int64_t>(j, "RunTimeTicks", 0);
    v.childCount = get<int>(j, "ChildCount", 0);
    v.isFolder = get<bool>(j, "IsFolder", false);
    auto tags = j.find("ImageTags");
    if (tags != j.end() && tags->is_object()) v.primaryImageTag = get<std::string>(*tags, "Primary", "");
    auto ud = j.find("UserData");
    if (ud != j.end() && ud->is_object()) v.userData = ud->get<UserData>();
    v.mediaSources = get<std::vector<MediaSource>>(j, "MediaSources", {});
}

}  // namespace jelly
