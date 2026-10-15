#pragma once

#include "jelly/client.hpp"
#include "jelly/models.hpp"
#include "jelly/platform.hpp"
#include "jelly/transcode.hpp"

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace jelly {

enum class DownloadStatus { Queued, Downloading, Completed, Failed };

const char* statusName(DownloadStatus s);

struct DownloadItem {
    std::string itemId;
    std::string mediaSourceId;
    std::string name;
    std::string type;
    std::string seriesId;
    std::string seriesName;
    std::string seasonId;
    int seasonIndex = 0;
    int episodeIndex = 0;
    int productionYear = 0;
    int64_t runTimeTicks = 0;
    std::string overview;
    std::string profileId;
    int audioStreamIndex = -1;
    int subtitleStreamIndex = -1;
    std::string audioLabel;
    std::string subtitleLabel;
    DownloadStatus status = DownloadStatus::Queued;
    int64_t bytes = 0;
    std::string error;
    int64_t addedAt = 0;
    std::string seriesPrimaryImageTag;
    int64_t introStartTicks = -1;  // from the server's media segments, saved for offline Skip intro
    int64_t introEndTicks = -1;
    int64_t speedBps = 0;          // live transfer speed while downloading (not persisted)

    /// Rough final size from the profile bitrates (transcoded streams have no Content-Length).
    int64_t estimatedBytes() const;
    std::string label() const;
};

struct DownloadOptions {
    DeviceProfile profile;
    int audioStreamIndex = -1;
    int subtitleStreamIndex = -1;
};

/// Download queue persisted in `<root>/index.json`; each item lives in `<root>/<itemId>/`.
/// Not threaded itself: the platform layer calls runNext() from a worker thread.
class DownloadManager {
public:
    DownloadManager(Client& client, IHttp& http, std::string root, ClockFn clock = systemClock,
                    ClockFn clockMs = systemClockMs);

    /// Loads the index; interrupted downloads go back to Queued.
    void load();

    /// Queues a playable item. Returns false if it is already queued/downloaded (failed items are re-queued).
    bool enqueue(const Item& item, const DownloadOptions& options);
    void retry(const std::string& itemId);
    void retryFailed();
    /// Moves a queued item one place earlier in the queue.
    void moveUp(const std::string& itemId);
    /// A paused queue finishes nothing new (the running download is left to complete).
    void setPaused(bool paused);
    bool paused() const;
    /// True while there is queued/running work and the queue is not paused.
    bool active() const;
    /// Cancels a running download (it becomes Failed "Cancelled") or drops a queued one.
    void cancel(const std::string& itemId);
    /// Deletes files and forgets the item (cancelling it first when running).
    void remove(const std::string& itemId);

    /// Downloads the first queued item, blocking. Returns false when nothing was queued.
    bool runNext();
    bool hasPending() const;
    bool isDownloading() const;

    std::vector<DownloadItem> items() const;
    std::optional<DownloadItem> find(const std::string& itemId) const;
    std::optional<DownloadStatus> status(const std::string& itemId) const;
    /// Next downloaded episode of the same series (by season, then episode), if any.
    std::optional<DownloadItem> nextEpisode(const std::string& itemId) const;
    /// Previous downloaded episode of the same series, if any.
    std::optional<DownloadItem> previousEpisode(const std::string& itemId) const;

    std::string videoPath(const std::string& itemId) const;
    std::string thumbPath(const std::string& itemId) const;
    /// 2:3 poster (the show's for an episode), so the Saved view has covers offline.
    std::string posterPath(const std::string& itemId) const;
    const std::string& root() const { return root_; }

private:
    void saveLocked();
    std::string dir(const std::string& itemId) const;
    DownloadItem* findLocked(const std::string& itemId);
    std::optional<DownloadItem> neighbourEpisode(const std::string& itemId, int dir) const;

    Client& client_;
    IHttp& http_;
    std::string root_;
    ClockFn clock_;
    ClockFn clockMs_;
    mutable std::mutex mutex_;
    std::vector<DownloadItem> items_;
    std::string activeId_;
    std::atomic_bool cancel_{false};
    bool removeAfterCancel_ = false;
    bool paused_ = false;
};

}  // namespace jelly
