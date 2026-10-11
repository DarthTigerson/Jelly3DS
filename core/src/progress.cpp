#include "jelly/progress.hpp"
#include "jelly/util.hpp"

namespace jelly {

ProgressStore::ProgressStore(std::string path, ClockFn clock) : path_(std::move(path)), clock_(std::move(clock)) {}

void ProgressStore::load() {
    nlohmann::json j;
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    if (!loadJson(path_, j) || !j.is_object()) return;
    for (auto& [id, e] : j.items()) {
        ProgressEntry p;
        p.positionTicks = e.value("positionTicks", int64_t{0});
        p.runTimeTicks = e.value("runTimeTicks", int64_t{0});
        p.played = e.value("played", false);
        p.updatedAt = e.value("updatedAt", int64_t{0});
        p.dirty = e.value("dirty", false);
        entries_[id] = p;
    }
}

bool ProgressStore::save() {
    nlohmann::json j = nlohmann::json::object();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [id, p] : entries_) {
            j[id] = {{"positionTicks", p.positionTicks},
                     {"runTimeTicks", p.runTimeTicks},
                     {"played", p.played},
                     {"updatedAt", p.updatedAt},
                     {"dirty", p.dirty}};
        }
    }
    return saveJson(path_, j);
}

std::optional<ProgressEntry> ProgressStore::get(const std::string& itemId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(itemId);
    if (it == entries_.end()) return std::nullopt;
    return it->second;
}

void ProgressStore::record(const std::string& itemId, int64_t positionTicks, int64_t runTimeTicks) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& e = entries_[itemId];
    if (runTimeTicks > 0) e.runTimeTicks = runTimeTicks;
    if (e.runTimeTicks > 0 && positionTicks >= static_cast<int64_t>(e.runTimeTicks * PLAYED_THRESHOLD)) {
        e.played = true;
        e.positionTicks = 0;
    } else {
        e.positionTicks = positionTicks < 0 ? 0 : positionTicks;
    }
    e.updatedAt = clock_();
    e.dirty = true;
    ++e.revision;
}

void ProgressStore::setPlayed(const std::string& itemId, bool played, int64_t runTimeTicks) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& e = entries_[itemId];
    if (runTimeTicks > 0) e.runTimeTicks = runTimeTicks;
    e.played = played;
    e.positionTicks = 0;
    e.updatedAt = clock_();
    e.dirty = true;
    ++e.revision;
}

void ProgressStore::applyServer(const std::string& itemId, const UserData& data, int64_t runTimeTicks) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(itemId);
        if (it != entries_.end() && it->second.dirty) return;
    }
    adoptServer(itemId, data, runTimeTicks);
}

void ProgressStore::adoptServer(const std::string& itemId, const UserData& data, int64_t runTimeTicks) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& e = entries_[itemId];
    e.positionTicks = data.playbackPositionTicks;
    e.played = data.played;
    if (runTimeTicks > 0) e.runTimeTicks = runTimeTicks;
    e.updatedAt = data.lastPlayedDate;
    e.dirty = false;
    ++e.revision;
}

std::vector<std::pair<std::string, ProgressEntry>> ProgressStore::dirtyEntries() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::pair<std::string, ProgressEntry>> out;
    for (const auto& [id, e] : entries_)
        if (e.dirty) out.emplace_back(id, e);
    return out;
}

void ProgressStore::markClean(const std::string& itemId, uint64_t revision) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(itemId);
    if (it != entries_.end() && it->second.revision == revision) it->second.dirty = false;
}

void ProgressStore::erase(const std::string& itemId) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(itemId);
}

}  // namespace jelly
