#pragma once

#include <cstdint>
#include <mutex>
#include <string>

namespace jelly {

/// 64-bit FNV-1a, used to turn URLs / API paths into cache file names.
uint64_t fnv1a(const std::string& s);
std::string hashName(const std::string& s);

/// Server responses kept on the SD card, so screens can show the last known list instantly
/// (and while offline) and refresh it in the background.
class ResponseCache {
public:
    explicit ResponseCache(std::string dir);

    bool get(const std::string& key, std::string& body) const;
    void put(const std::string& key, const std::string& body);

private:
    std::string path(const std::string& key) const;
    std::string dir_;
    mutable std::mutex mutex_;
};

}  // namespace jelly
