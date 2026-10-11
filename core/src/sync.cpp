#include "jelly/sync.hpp"
#include "jelly/util.hpp"

namespace jelly {

SyncEngine::SyncEngine(Client& client, ProgressStore& progress, DownloadManager& downloads)
    : client_(client), progress_(progress), downloads_(downloads) {}

SyncEngine::PushResult SyncEngine::push(const std::string& itemId, const ProgressEntry& entry) {
    auto server = client_.item(itemId);
    if (!server.ok) {
        // Item deleted from the library: nothing to sync any more.
        if (server.error == "Not found on server") {
            progress_.markClean(itemId, entry.revision);
            return PushResult::Failed;
        }
        return PushResult::Unreachable;
    }

    // Someone watched it elsewhere after our offline session: the server's newer state wins.
    if (server.value.userData.lastPlayedDate > entry.updatedAt) {
        progress_.adoptServer(itemId, server.value.userData, server.value.runTimeTicks);
        return PushResult::Adopted;
    }

    bool ok = client_.updateUserData(itemId, entry.positionTicks, entry.played, entry.updatedAt).ok;
    if (!ok) {
        // Servers without the UserData endpoint: a "stopped" report stores the position,
        // and Jellyfin's own 90 % rule or the played endpoint handle the watched flag.
        if (entry.played) {
            ok = client_.setPlayed(itemId, true).ok;
        } else {
            std::string session = randomHex(32);
            ok = client_.reportStart(itemId, "", entry.positionTicks, session, true).ok &&
                 client_.reportStopped(itemId, "", entry.positionTicks, session).ok;
        }
    }
    if (!ok) return PushResult::Failed;
    progress_.markClean(itemId, entry.revision);
    return PushResult::Pushed;
}

bool SyncEngine::pushItem(const std::string& itemId) {
    auto entry = progress_.get(itemId);
    if (!entry || !entry->dirty) return true;
    bool ok = push(itemId, *entry) != PushResult::Unreachable;
    progress_.save();
    return ok;
}

SyncReport SyncEngine::run() {
    SyncReport report;
    auto ping = client_.ping();
    if (!ping.ok) {
        report.error = ping.error;
        return report;
    }
    report.reachedServer = true;

    for (const auto& [id, entry] : progress_.dirtyEntries()) {
        switch (push(id, entry)) {
        case PushResult::Pushed: ++report.pushed; break;
        case PushResult::Adopted: ++report.adopted; break;
        case PushResult::Failed: ++report.failed; break;
        case PushResult::Unreachable:
            ++report.failed;
            report.error = "Lost connection during sync";
            progress_.save();
            return report;
        }
    }

    // Pull positions for downloaded items so watching on the TV at home carries over to the 3DS.
    std::vector<std::string> ids;
    for (const auto& d : downloads_.items())
        if (d.status == DownloadStatus::Completed) ids.push_back(d.itemId);
    for (size_t i = 0; i < ids.size(); i += 50) {
        std::vector<std::string> batch(ids.begin() + i, ids.begin() + std::min(ids.size(), i + 50));
        auto r = client_.itemsByIds(batch);
        if (!r.ok) {
            report.error = r.error;
            break;
        }
        for (const auto& item : r.value) {
            auto before = progress_.get(item.id);
            progress_.applyServer(item.id, item.userData, item.runTimeTicks);
            auto after = progress_.get(item.id);
            if (after && !after->dirty &&
                (!before || before->positionTicks != after->positionTicks || before->played != after->played))
                ++report.refreshed;
        }
    }
    progress_.save();
    return report;
}

}  // namespace jelly
