#pragma once

#include "jelly/client.hpp"
#include "jelly/downloads.hpp"
#include "jelly/progress.hpp"

#include <string>

namespace jelly {

struct SyncReport {
    bool reachedServer = false;
    int pushed = 0;     // local changes sent to the server
    int adopted = 0;    // conflicts where the server had newer state
    int refreshed = 0;  // downloaded items whose position was pulled from the server
    int failed = 0;
    std::string error;
};

/// Reconciles offline watch progress with Jellyfin. Safe to call from a worker thread.
class SyncEngine {
public:
    SyncEngine(Client& client, ProgressStore& progress, DownloadManager& downloads);

    SyncReport run();
    /// Pushes one item right away (end of an online playback). Returns true when the server accepted it.
    bool pushItem(const std::string& itemId);

private:
    enum class PushResult { Pushed, Adopted, Failed, Unreachable };
    PushResult push(const std::string& itemId, const ProgressEntry& entry);

    Client& client_;
    ProgressStore& progress_;
    DownloadManager& downloads_;
};

}  // namespace jelly
