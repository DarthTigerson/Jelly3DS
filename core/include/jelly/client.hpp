#pragma once

#include "jelly/config.hpp"
#include "jelly/models.hpp"
#include "jelly/platform.hpp"

#include <mutex>
#include <string>
#include <vector>

namespace jelly {

constexpr const char* APP_NAME = "Jelly3DS";
constexpr const char* APP_VERSION = "0.1.0";

template <typename T>
struct Result {
    bool ok = false;
    T value{};
    std::string error;

    static Result success(T v) { return {true, std::move(v), {}}; }
    static Result failure(std::string e) { return {false, T{}, std::move(e)}; }
};

struct ItemQuery {
    std::string parentId;
    std::string includeItemTypes;  // comma separated, e.g. "Series,Movie"
    bool recursive = false;
    std::string searchTerm;
    int startIndex = 0;
    int limit = 200;
};

struct ItemPage {
    std::vector<Item> items;
    int totalRecordCount = 0;
};

/// Thread-safe Jellyfin REST client. Credentials are read from the shared Config under a mutex.
class Client {
public:
    Client(IHttp& http, Config& config, std::string deviceName);

    /// Logs in and stores server/user/token into the config on success.
    Result<AuthResult> authenticate(const std::string& server, const std::string& user, const std::string& password);
    /// True when the server answers and the stored token is still valid.
    Result<bool> ping();

    Result<std::vector<Item>> views();
    Result<ItemPage> items(const ItemQuery& q);
    Result<std::vector<Item>> seasons(const std::string& seriesId);
    Result<std::vector<Item>> episodes(const std::string& seriesId, const std::string& seasonId = "");
    Result<std::vector<Item>> resumeItems();
    Result<std::vector<Item>> nextUp();
    Result<Item> item(const std::string& id);

    Result<bool> reportStart(const std::string& itemId, const std::string& mediaSourceId, int64_t positionTicks,
                             const std::string& playSessionId, bool paused);
    Result<bool> reportProgress(const std::string& itemId, const std::string& mediaSourceId, int64_t positionTicks,
                                const std::string& playSessionId, bool paused);
    Result<bool> reportStopped(const std::string& itemId, const std::string& mediaSourceId, int64_t positionTicks,
                               const std::string& playSessionId);
    Result<bool> setPlayed(const std::string& itemId, bool played);
    /// Writes resume position / played flag with the real (offline) watch time. Jellyfin 10.9+.
    Result<bool> updateUserData(const std::string& itemId, int64_t positionTicks, bool played, int64_t lastPlayedAt);
    Result<std::vector<Item>> itemsByIds(const std::vector<std::string>& ids);

    std::string imageUrl(const std::string& itemId, const std::string& tag, int maxWidth, int maxHeight) const;
    std::string server() const;
    std::string deviceId() const;
    Headers authHeaders() const;

private:
    std::string authValue(const std::string& token) const;
    Result<nlohmann::json> getJson(const std::string& pathAndQuery);
    Result<bool> postJson(const std::string& path, const nlohmann::json& body);
    std::string fields() const;
    std::string uid() const;

    IHttp& http_;
    Config& config_;
    std::string deviceName_;
    mutable std::mutex mutex_;
};

}  // namespace jelly
