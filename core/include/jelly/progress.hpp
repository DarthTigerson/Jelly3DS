#pragma once

#include "jelly/models.hpp"
#include "jelly/platform.hpp"

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace jelly {

struct ProgressEntry {
    int64_t positionTicks = 0;
    int64_t runTimeTicks = 0;
    bool played = false;
    int64_t updatedAt = 0;  // unix seconds of the last local or server change
    bool dirty = false;     // changed locally, not yet sent to the server
    uint64_t revision = 0;  // bumped on every local change (in-memory only)
};

/// Resume positions and watched flags, persisted on SD so they survive offline sessions.
class ProgressStore {
public:
    /// Jellyfin marks an item played once it is stopped past 90 % (MaxResumePct default).
    static constexpr double PLAYED_THRESHOLD = 0.90;

    ProgressStore(std::string path, ClockFn clock = systemClock);

    void load();
    bool save();

    std::optional<ProgressEntry> get(const std::string& itemId) const;
    /// Records a local playback position; marks the item played (and rewinds it) past the threshold.
    void record(const std::string& itemId, int64_t positionTicks, int64_t runTimeTicks);
    void setPlayed(const std::string& itemId, bool played, int64_t runTimeTicks = 0);
    /// Adopts server state unless there are unsynced local changes.
    void applyServer(const std::string& itemId, const UserData& data, int64_t runTimeTicks);
    /// Overwrites local state with the server state (server wins a conflict).
    void adoptServer(const std::string& itemId, const UserData& data, int64_t runTimeTicks);

    std::vector<std::pair<std::string, ProgressEntry>> dirtyEntries() const;
    /// Clears dirty only if nothing changed since the entry (with this revision) was read.
    void markClean(const std::string& itemId, uint64_t revision);
    void erase(const std::string& itemId);

private:
    std::string path_;
    ClockFn clock_;
    mutable std::mutex mutex_;
    std::map<std::string, ProgressEntry> entries_;
};

}  // namespace jelly
