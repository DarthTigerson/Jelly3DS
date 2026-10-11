#include "jelly/downloads.hpp"
#include "jelly/util.hpp"

#include <algorithm>
#include <cstdio>

namespace jelly {

namespace {

// Anything smaller than this is an error page, not a video.
constexpr int64_t MIN_VIDEO_BYTES = 64 * 1024;

DownloadStatus statusFromString(const std::string& s) {
    if (s == "Completed") return DownloadStatus::Completed;
    if (s == "Failed") return DownloadStatus::Failed;
    return DownloadStatus::Queued;  // Downloading after a restart resumes as Queued
}

nlohmann::json toJson(const DownloadItem& d) {
    return {
        {"itemId", d.itemId},
        {"mediaSourceId", d.mediaSourceId},
        {"name", d.name},
        {"type", d.type},
        {"seriesId", d.seriesId},
        {"seriesName", d.seriesName},
        {"seasonId", d.seasonId},
        {"seasonIndex", d.seasonIndex},
        {"episodeIndex", d.episodeIndex},
        {"productionYear", d.productionYear},
        {"runTimeTicks", d.runTimeTicks},
        {"overview", d.overview},
        {"profileId", d.profileId},
        {"audioStreamIndex", d.audioStreamIndex},
        {"subtitleStreamIndex", d.subtitleStreamIndex},
        {"audioLabel", d.audioLabel},
        {"subtitleLabel", d.subtitleLabel},
        {"status", statusName(d.status)},
        {"bytes", d.bytes},
        {"error", d.error},
        {"addedAt", d.addedAt},
    };
}

DownloadItem fromJson(const nlohmann::json& j) {
    DownloadItem d;
    d.itemId = j.value("itemId", "");
    d.mediaSourceId = j.value("mediaSourceId", "");
    d.name = j.value("name", "");
    d.type = j.value("type", "");
    d.seriesId = j.value("seriesId", "");
    d.seriesName = j.value("seriesName", "");
    d.seasonId = j.value("seasonId", "");
    d.seasonIndex = j.value("seasonIndex", 0);
    d.episodeIndex = j.value("episodeIndex", 0);
    d.productionYear = j.value("productionYear", 0);
    d.runTimeTicks = j.value("runTimeTicks", int64_t{0});
    d.overview = j.value("overview", "");
    d.profileId = j.value("profileId", "new3ds");
    d.audioStreamIndex = j.value("audioStreamIndex", -1);
    d.subtitleStreamIndex = j.value("subtitleStreamIndex", -1);
    d.audioLabel = j.value("audioLabel", "");
    d.subtitleLabel = j.value("subtitleLabel", "");
    d.status = statusFromString(j.value("status", "Queued"));
    d.bytes = j.value("bytes", int64_t{0});
    d.error = j.value("error", "");
    d.addedAt = j.value("addedAt", int64_t{0});
    return d;
}

std::string streamLabel(const MediaSource* src, int index) {
    if (!src || index < 0) return "";
    for (const auto& s : src->streams)
        if (s.index == index) return s.displayTitle.empty() ? s.language : s.displayTitle;
    return "";
}

}  // namespace

const char* statusName(DownloadStatus s) {
    switch (s) {
    case DownloadStatus::Queued: return "Queued";
    case DownloadStatus::Downloading: return "Downloading";
    case DownloadStatus::Completed: return "Completed";
    case DownloadStatus::Failed: return "Failed";
    }
    return "Queued";
}

int64_t DownloadItem::estimatedBytes() const {
    auto p = DeviceProfile::byId(profileId);
    double seconds = static_cast<double>(runTimeTicks) / TICKS_PER_SECOND;
    // MPEG-TS adds roughly 10 % container overhead on top of the elementary streams.
    return static_cast<int64_t>(seconds * (p.videoBitrate + p.audioBitrate) / 8.0 * 1.1);
}

std::string DownloadItem::label() const {
    if (type == "Episode") {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "S%02dE%02d", seasonIndex, episodeIndex);
        return std::string(buf) + " \xC2\xB7 " + name;
    }
    if (productionYear > 0) return name + " (" + std::to_string(productionYear) + ")";
    return name;
}

DownloadManager::DownloadManager(Client& client, IHttp& http, std::string root, ClockFn clock)
    : client_(client), http_(http), root_(std::move(root)), clock_(std::move(clock)) {}

std::string DownloadManager::dir(const std::string& itemId) const { return root_ + "/" + itemId; }
std::string DownloadManager::videoPath(const std::string& itemId) const { return dir(itemId) + "/video.ts"; }
std::string DownloadManager::thumbPath(const std::string& itemId) const { return dir(itemId) + "/thumb.jpg"; }

void DownloadManager::load() {
    makeDirs(root_);
    nlohmann::json j;
    std::lock_guard<std::mutex> lock(mutex_);
    items_.clear();
    if (loadJson(root_ + "/index.json", j) && j.is_array()) {
        for (const auto& e : j) {
            auto d = fromJson(e);
            if (d.itemId.empty()) continue;
            // A completed download whose file vanished (SD card swapped/cleaned) needs downloading again.
            if (d.status == DownloadStatus::Completed && !fileExists(videoPath(d.itemId))) {
                d.status = DownloadStatus::Failed;
                d.error = "File missing";
            }
            items_.push_back(std::move(d));
        }
    }
    saveLocked();
}

void DownloadManager::saveLocked() {
    nlohmann::json j = nlohmann::json::array();
    for (const auto& d : items_) j.push_back(toJson(d));
    saveJson(root_ + "/index.json", j);
}

DownloadItem* DownloadManager::findLocked(const std::string& itemId) {
    for (auto& d : items_)
        if (d.itemId == itemId) return &d;
    return nullptr;
}

bool DownloadManager::enqueue(const Item& item, const DownloadOptions& options) {
    if (!item.isPlayable() || item.id.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto* existing = findLocked(item.id)) {
        if (existing->status != DownloadStatus::Failed) return false;
        existing->status = DownloadStatus::Queued;
        existing->error.clear();
        existing->profileId = options.profile.id;
        existing->audioStreamIndex = options.audioStreamIndex;
        existing->subtitleStreamIndex = options.subtitleStreamIndex;
        saveLocked();
        return true;
    }

    const MediaSource* src = item.mediaSources.empty() ? nullptr : &item.mediaSources.front();
    DownloadItem d;
    d.itemId = item.id;
    d.mediaSourceId = src ? src->id : item.id;
    d.name = item.name;
    d.type = item.type;
    d.seriesId = item.seriesId;
    d.seriesName = item.seriesName;
    d.seasonId = item.seasonId;
    d.seasonIndex = item.parentIndexNumber;
    d.episodeIndex = item.indexNumber;
    d.productionYear = item.productionYear;
    d.runTimeTicks = item.runTimeTicks;
    d.overview = item.overview;
    d.profileId = options.profile.id;
    d.audioStreamIndex = options.audioStreamIndex;
    d.subtitleStreamIndex = options.subtitleStreamIndex;
    d.audioLabel = streamLabel(src, options.audioStreamIndex);
    d.subtitleLabel = streamLabel(src, options.subtitleStreamIndex);
    d.addedAt = clock_();
    items_.push_back(std::move(d));
    saveLocked();
    return true;
}

void DownloadManager::retry(const std::string& itemId) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto* d = findLocked(itemId); d && d->status == DownloadStatus::Failed) {
        d->status = DownloadStatus::Queued;
        d->error.clear();
        saveLocked();
    }
}

void DownloadManager::cancel(const std::string& itemId) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (itemId == activeId_) {
        cancel_ = true;
        return;
    }
    auto it = std::find_if(items_.begin(), items_.end(), [&](const DownloadItem& d) { return d.itemId == itemId; });
    if (it != items_.end() && it->status == DownloadStatus::Queued) {
        items_.erase(it);
        saveLocked();
        removeAll(dir(itemId));
    }
}

void DownloadManager::remove(const std::string& itemId) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (itemId == activeId_) {
        removeAfterCancel_ = true;
        cancel_ = true;
        return;
    }
    items_.erase(std::remove_if(items_.begin(), items_.end(), [&](const DownloadItem& d) { return d.itemId == itemId; }),
                 items_.end());
    saveLocked();
    removeAll(dir(itemId));
}

bool DownloadManager::runNext() {
    DownloadItem job;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!activeId_.empty()) return false;
        auto it = std::find_if(items_.begin(), items_.end(),
                               [](const DownloadItem& d) { return d.status == DownloadStatus::Queued; });
        if (it == items_.end()) return false;
        it->status = DownloadStatus::Downloading;
        it->bytes = 0;
        it->error.clear();
        job = *it;
        activeId_ = job.itemId;
        cancel_ = false;
        removeAfterCancel_ = false;
        saveLocked();
    }

    const std::string itemDir = dir(job.itemId);
    makeDirs(itemDir);
    auto headers = client_.authHeaders();

    // Artwork first: small, and lets the Downloads screen show something while the video transcodes.
    if (!fileExists(thumbPath(job.itemId))) {
        std::atomic_bool noCancel{false};
        auto r = http_.download(client_.imageUrl(job.itemId, "", 200, 200), headers, thumbPath(job.itemId), nullptr,
                                noCancel);
        if (!r.ok()) removeAll(thumbPath(job.itemId));
    }

    StreamRequest req;
    req.itemId = job.itemId;
    req.mediaSourceId = job.mediaSourceId;
    req.audioStreamIndex = job.audioStreamIndex;
    req.subtitleStreamIndex = job.subtitleStreamIndex;
    // Separate device id so a stream started meanwhile never makes the server kill this transcode.
    req.deviceId = client_.deviceId() + "-dl";
    req.playSessionId = randomHex(32);
    const std::string url = buildStreamUrl(client_.server(), DeviceProfile::byId(job.profileId), req);

    const std::string partPath = itemDir + "/video.ts.part";
    auto progress = [this, id = job.itemId](int64_t now, int64_t) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto* d = findLocked(id)) d->bytes = now;
    };
    auto r = http_.download(url, headers, partPath, progress, cancel_);

    std::lock_guard<std::mutex> lock(mutex_);
    activeId_.clear();
    auto* d = findLocked(job.itemId);
    if (!d) return true;

    if (cancel_) {
        removeAll(partPath);
        if (removeAfterCancel_) {
            items_.erase(std::remove_if(items_.begin(), items_.end(),
                                        [&](const DownloadItem& x) { return x.itemId == job.itemId; }),
                         items_.end());
            removeAll(itemDir);
        } else {
            d->status = DownloadStatus::Failed;
            d->error = "Cancelled";
        }
    } else if (!r.ok()) {
        removeAll(partPath);
        d->status = DownloadStatus::Failed;
        d->error = !r.error.empty() ? r.error : "HTTP " + std::to_string(r.status);
    } else if (fileSize(partPath) < MIN_VIDEO_BYTES) {
        removeAll(partPath);
        d->status = DownloadStatus::Failed;
        d->error = "Server sent an empty video (transcoding failed?)";
    } else {
        removeAll(videoPath(job.itemId));
        std::rename(partPath.c_str(), videoPath(job.itemId).c_str());
        d->status = DownloadStatus::Completed;
        d->bytes = fileSize(videoPath(job.itemId));
    }
    cancel_ = false;
    saveLocked();
    return true;
}

bool DownloadManager::hasPending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::any_of(items_.begin(), items_.end(), [](const DownloadItem& d) {
        return d.status == DownloadStatus::Queued || d.status == DownloadStatus::Downloading;
    });
}

bool DownloadManager::isDownloading() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !activeId_.empty();
}

std::vector<DownloadItem> DownloadManager::items() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_;
}

std::optional<DownloadItem> DownloadManager::find(const std::string& itemId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& d : items_)
        if (d.itemId == itemId) return d;
    return std::nullopt;
}

std::optional<DownloadStatus> DownloadManager::status(const std::string& itemId) const {
    auto d = find(itemId);
    if (!d) return std::nullopt;
    return d->status;
}

std::optional<DownloadItem> DownloadManager::nextEpisode(const std::string& itemId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const DownloadItem* cur = nullptr;
    for (const auto& d : items_)
        if (d.itemId == itemId) cur = &d;
    if (!cur || cur->type != "Episode" || cur->seriesId.empty()) return std::nullopt;

    const DownloadItem* best = nullptr;
    auto key = [](const DownloadItem& d) { return std::make_pair(d.seasonIndex, d.episodeIndex); };
    for (const auto& d : items_) {
        if (d.seriesId != cur->seriesId || d.status != DownloadStatus::Completed || d.itemId == cur->itemId) continue;
        if (key(d) <= key(*cur)) continue;
        if (!best || key(d) < key(*best)) best = &d;
    }
    if (!best) return std::nullopt;
    return *best;
}

}  // namespace jelly
