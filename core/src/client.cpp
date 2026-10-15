#include "jelly/client.hpp"
#include "jelly/util.hpp"

namespace jelly {

namespace {

std::string describe(const HttpResponse& r) {
    if (!r.error.empty()) return r.error;
    if (r.status == 401) return "Not authorized (login expired?)";
    if (r.status == 404) return "Not found on server";
    return "Server returned HTTP " + std::to_string(r.status);
}

nlohmann::json playbackBody(const std::string& itemId, const std::string& mediaSourceId, int64_t positionTicks,
                            const std::string& playSessionId) {
    return {
        {"ItemId", itemId},
        {"MediaSourceId", mediaSourceId.empty() ? itemId : mediaSourceId},
        {"PositionTicks", positionTicks},
        {"PlaySessionId", playSessionId},
        {"PlayMethod", "Transcode"},
        {"CanSeek", true},
    };
}

}  // namespace

Client::Client(IHttp& http, Config& config, std::string deviceName)
    : http_(http), config_(config), deviceName_(std::move(deviceName)) {}

std::string Client::authValue(const std::string& token) const {
    std::string v = std::string("MediaBrowser Client=\"") + APP_NAME + "\", Device=\"" + deviceName_ +
                    "\", DeviceId=\"" + config_.deviceId + "\", Version=\"" + APP_VERSION + "\"";
    if (!token.empty()) v += ", Token=\"" + token + "\"";
    return v;
}

Headers Client::authHeaders() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {{"Authorization", authValue(config_.accessToken)}, {"Accept", "application/json"}};
}

std::string Client::server() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_.serverUrl;
}

std::string Client::deviceId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_.deviceId;
}

std::string Client::fields() const {
    return "Overview,MediaSources,ChildCount,ProductionYear,ParentId,Genres,OfficialRating";
}

Result<nlohmann::json> Client::getJson(const std::string& pathAndQuery) {
    auto r = http_.get(server() + pathAndQuery, authHeaders());
    if (!r.ok()) return Result<nlohmann::json>::failure(describe(r));
    auto j = nlohmann::json::parse(r.body, nullptr, false);
    if (j.is_discarded()) return Result<nlohmann::json>::failure("Invalid JSON from server");
    return Result<nlohmann::json>::success(std::move(j));
}

Result<bool> Client::postJson(const std::string& path, const nlohmann::json& body) {
    auto r = http_.post(server() + path, authHeaders(), body.dump());
    if (!r.ok()) return Result<bool>::failure(describe(r));
    return Result<bool>::success(true);
}

Result<AuthResult> Client::authenticate(const std::string& serverUrl, const std::string& user,
                                        const std::string& password) {
    std::string server = normalizeServerUrl(serverUrl);
    std::string auth;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (config_.deviceId.empty()) config_.deviceId = "jelly3ds-" + randomHex(16);
        auth = authValue("");
    }
    nlohmann::json body = {{"Username", user}, {"Pw", password}};
    auto r = http_.post(server + "/Users/AuthenticateByName", {{"Authorization", auth}}, body.dump());
    if (!r.ok()) {
        if (r.status == 401) return Result<AuthResult>::failure("Wrong username or password");
        return Result<AuthResult>::failure(describe(r));
    }
    auto j = nlohmann::json::parse(r.body, nullptr, false);
    if (j.is_discarded() || !j.contains("AccessToken")) return Result<AuthResult>::failure("Unexpected login reply");

    AuthResult res;
    res.accessToken = j.value("AccessToken", "");
    res.serverId = j.value("ServerId", "");
    if (j.contains("User")) {
        res.user.id = j["User"].value("Id", "");
        res.user.name = j["User"].value("Name", "");
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        config_.serverUrl = server;
        config_.accessToken = res.accessToken;
        config_.userId = res.user.id;
        config_.userName = res.user.name;
    }
    return Result<AuthResult>::success(res);
}

Result<bool> Client::ping() {
    auto r = getJson("/Users/Me");
    if (!r.ok) return Result<bool>::failure(r.error);
    return Result<bool>::success(true);
}

std::string Client::uid() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_.userId;
}

Result<std::vector<Item>> Client::views() {
    auto r = getJson("/UserViews?" + buildQuery({{"userId", uid()}}));
    if (!r.ok) return Result<std::vector<Item>>::failure(r.error);
    return Result<std::vector<Item>>::success(r.value.value("Items", nlohmann::json::array()).get<std::vector<Item>>());
}

Result<ItemPage> Client::items(const ItemQuery& q) {
    std::vector<std::pair<std::string, std::string>> params = {
        {"userId", uid()},
        {"fields", fields()},
        {"sortBy", q.sortBy},
        {"sortOrder", q.sortOrder},
        {"startIndex", std::to_string(q.startIndex)},
        {"limit", std::to_string(q.limit)},
        {"enableImageTypes", "Primary,Backdrop,Thumb"},
        {"imageTypeLimit", "1"},
    };
    if (!q.filters.empty()) params.emplace_back("filters", q.filters);
    if (!q.parentId.empty()) params.emplace_back("parentId", q.parentId);
    if (!q.includeItemTypes.empty()) params.emplace_back("includeItemTypes", q.includeItemTypes);
    if (q.recursive) params.emplace_back("recursive", "true");
    if (!q.searchTerm.empty()) params.emplace_back("searchTerm", q.searchTerm);
    auto r = getJson("/Items?" + buildQuery(params));
    if (!r.ok) return Result<ItemPage>::failure(r.error);
    ItemPage page;
    page.items = r.value.value("Items", nlohmann::json::array()).get<std::vector<Item>>();
    page.totalRecordCount = r.value.value("TotalRecordCount", static_cast<int>(page.items.size()));
    return Result<ItemPage>::success(std::move(page));
}

Result<std::vector<Item>> Client::seasons(const std::string& seriesId) {
    auto r = getJson("/Shows/" + seriesId + "/Seasons?" + buildQuery({{"userId", uid()}, {"fields", fields()}}));
    if (!r.ok) return Result<std::vector<Item>>::failure(r.error);
    return Result<std::vector<Item>>::success(r.value.value("Items", nlohmann::json::array()).get<std::vector<Item>>());
}

Result<std::vector<Item>> Client::episodes(const std::string& seriesId, const std::string& seasonId) {
    std::vector<std::pair<std::string, std::string>> params = {{"userId", uid()}, {"fields", fields()}};
    if (!seasonId.empty()) params.emplace_back("seasonId", seasonId);
    auto r = getJson("/Shows/" + seriesId + "/Episodes?" + buildQuery(params));
    if (!r.ok) return Result<std::vector<Item>>::failure(r.error);
    return Result<std::vector<Item>>::success(r.value.value("Items", nlohmann::json::array()).get<std::vector<Item>>());
}

Result<std::vector<Item>> Client::resumeItems() {
    auto params = buildQuery({{"userId", uid()}, {"mediaTypes", "Video"}, {"fields", fields()}, {"limit", "20"}});
    auto r = getJson("/UserItems/Resume?" + params);
    if (!r.ok) r = getJson("/Users/" + uid() + "/Items/Resume?" + params);  // servers before 10.9
    if (!r.ok) return Result<std::vector<Item>>::failure(r.error);
    return Result<std::vector<Item>>::success(r.value.value("Items", nlohmann::json::array()).get<std::vector<Item>>());
}

Result<std::vector<Item>> Client::nextUp() {
    auto r = getJson("/Shows/NextUp?" + buildQuery({{"userId", uid()}, {"fields", fields()}, {"limit", "20"}}));
    if (!r.ok) return Result<std::vector<Item>>::failure(r.error);
    return Result<std::vector<Item>>::success(r.value.value("Items", nlohmann::json::array()).get<std::vector<Item>>());
}

Result<std::vector<Item>> Client::latest(const std::string& parentId, int limit) {
    auto r = getJson("/Items/Latest?" + buildQuery({{"userId", uid()},
                                                    {"parentId", parentId},
                                                    {"limit", std::to_string(limit)},
                                                    {"fields", fields()},
                                                    {"groupItems", "true"}}));
    if (!r.ok) return Result<std::vector<Item>>::failure(r.error);
    // This endpoint returns a bare array, not {"Items": [...]}.
    if (!r.value.is_array()) return Result<std::vector<Item>>::success({});
    return Result<std::vector<Item>>::success(r.value.get<std::vector<Item>>());
}

Result<std::pair<int64_t, int64_t>> Client::introSegment(const std::string& itemId) {
    auto r = getJson("/MediaSegments/" + itemId + "?" + buildQuery({{"includeSegmentTypes", "Intro"}}));
    if (!r.ok) return Result<std::pair<int64_t, int64_t>>::failure(r.error);
    for (const auto& seg : r.value.value("Items", nlohmann::json::array())) {
        if (seg.value("Type", "") != "Intro") continue;
        int64_t start = seg.value("StartTicks", int64_t{0}), end = seg.value("EndTicks", int64_t{0});
        if (end > start) return Result<std::pair<int64_t, int64_t>>::success({start, end});
    }
    return Result<std::pair<int64_t, int64_t>>::failure("No intro");
}

Result<Item> Client::item(const std::string& id) {
    auto r = getJson("/Items/" + id + "?" + buildQuery({{"userId", uid()}}));
    if (!r.ok) r = getJson("/Users/" + uid() + "/Items/" + id);  // servers before 10.9
    if (!r.ok) return Result<Item>::failure(r.error);
    return Result<Item>::success(r.value.get<Item>());
}

Result<bool> Client::reportStart(const std::string& itemId, const std::string& mediaSourceId, int64_t positionTicks,
                                 const std::string& playSessionId, bool paused) {
    auto body = playbackBody(itemId, mediaSourceId, positionTicks, playSessionId);
    body["IsPaused"] = paused;
    return postJson("/Sessions/Playing", body);
}

Result<bool> Client::reportProgress(const std::string& itemId, const std::string& mediaSourceId, int64_t positionTicks,
                                    const std::string& playSessionId, bool paused) {
    auto body = playbackBody(itemId, mediaSourceId, positionTicks, playSessionId);
    body["IsPaused"] = paused;
    body["EventName"] = "timeupdate";
    return postJson("/Sessions/Playing/Progress", body);
}

Result<bool> Client::reportStopped(const std::string& itemId, const std::string& mediaSourceId, int64_t positionTicks,
                                   const std::string& playSessionId) {
    return postJson("/Sessions/Playing/Stopped", playbackBody(itemId, mediaSourceId, positionTicks, playSessionId));
}

Result<bool> Client::setPlayed(const std::string& itemId, bool played) {
    std::string path = "/UserPlayedItems/" + itemId + "?" + buildQuery({{"userId", uid()}});
    auto r = played ? http_.post(server() + path, authHeaders(), "") : http_.del(server() + path, authHeaders());
    if (!r.ok()) {
        // Servers before 10.9
        std::string legacy = "/Users/" + uid() + "/PlayedItems/" + itemId;
        r = played ? http_.post(server() + legacy, authHeaders(), "") : http_.del(server() + legacy, authHeaders());
    }
    if (!r.ok()) return Result<bool>::failure(describe(r));
    return Result<bool>::success(true);
}

Result<bool> Client::updateUserData(const std::string& itemId, int64_t positionTicks, bool played,
                                    int64_t lastPlayedAt) {
    nlohmann::json body = {
        {"PlaybackPositionTicks", positionTicks},
        {"Played", played},
        {"LastPlayedDate", formatIsoDate(lastPlayedAt)},
    };
    return postJson("/UserItems/" + itemId + "/UserData?" + buildQuery({{"userId", uid()}}), body);
}

Result<std::vector<Item>> Client::itemsByIds(const std::vector<std::string>& ids) {
    std::string joined;
    for (const auto& id : ids) joined += (joined.empty() ? "" : ",") + id;
    auto r = getJson("/Items?" + buildQuery({{"userId", uid()}, {"ids", joined}, {"fields", fields()}}));
    if (!r.ok) return Result<std::vector<Item>>::failure(r.error);
    return Result<std::vector<Item>>::success(r.value.value("Items", nlohmann::json::array()).get<std::vector<Item>>());
}

std::string Client::imageUrl(const std::string& itemId, const std::string& tag, int maxWidth, int maxHeight,
                             const std::string& type) const {
    std::vector<std::pair<std::string, std::string>> q = {
        {"maxWidth", std::to_string(maxWidth)},
        {"maxHeight", std::to_string(maxHeight)},
        {"format", "Jpg"},
        {"quality", "80"},
    };
    if (!tag.empty()) q.emplace_back("tag", tag);
    return server() + "/Items/" + itemId + "/Images/" + type + "?" + buildQuery(q);
}

}  // namespace jelly
