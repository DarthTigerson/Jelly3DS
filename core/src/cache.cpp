#include "jelly/cache.hpp"
#include "jelly/util.hpp"

#include <cstdio>

namespace jelly {

uint64_t fnv1a(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

std::string hashName(const std::string& s) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(fnv1a(s)));
    return buf;
}

ResponseCache::ResponseCache(std::string dir) : dir_(std::move(dir)) { makeDirs(dir_); }

std::string ResponseCache::path(const std::string& key) const { return dir_ + "/" + hashName(key) + ".json"; }

bool ResponseCache::get(const std::string& key, std::string& body) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return readFile(path(key), body) && !body.empty();
}

void ResponseCache::put(const std::string& key, const std::string& body) {
    std::lock_guard<std::mutex> lock(mutex_);
    writeFileAtomic(path(key), body);
}

}  // namespace jelly
