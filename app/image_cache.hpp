#pragma once

#include "gfx.hpp"
#include "tasks.hpp"

#include "jelly/platform.hpp"

#include <condition_variable>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace app {

/// Loads posters / thumbnails (HTTP or SD file) on a background thread, decodes JPEG/PNG with stb_image,
/// downsizes to the requested box and uploads RGB565 textures on the main thread. LRU-bounded.
class ImageCache {
public:
    struct Source {
        std::string key;   // cache key (item id + size)
        std::string url;   // remote URL, or
        std::string path;  // local file
        int maxW = 64;
        int maxH = 64;
    };

    void start(jelly::IHttp* http, std::function<jelly::Headers()> headers);
    void stop();

    /// Returns the texture when ready; otherwise queues the load and returns nullptr.
    const Texture* get(const Source& src);
    /// Uploads finished decodes. Call once per frame on the main thread.
    void pump();
    void clear();

private:
    struct Decoded {
        std::string key;
        int w = 0, h = 0;
        std::vector<uint16_t> pixels;  // empty = failed
    };
    struct Entry {
        std::unique_ptr<Texture> tex;
        bool failed = false;
        std::list<std::string>::iterator lru;
    };

    void loop();
    Decoded decode(const Source& src);

    static constexpr size_t MAX_ENTRIES = 60;

    jelly::IHttp* http_ = nullptr;
    std::function<jelly::Headers()> headers_;
    Thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Source> queue_;
    std::vector<Decoded> done_;
    std::unordered_map<std::string, bool> pending_;
    std::unordered_map<std::string, Entry> entries_;
    std::list<std::string> lru_;
    bool stopping_ = false;
};

}  // namespace app
