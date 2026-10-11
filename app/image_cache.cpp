#include "image_cache.hpp"

#include "jelly/util.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include <stb/stb_image.h>

namespace app {

void ImageCache::start(jelly::IHttp* http, std::function<jelly::Headers()> headers) {
    http_ = http;
    headers_ = std::move(headers);
    stopping_ = false;
    thread_.start([this] { loop(); }, 0x32, -2, 96 * 1024);
}

void ImageCache::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    thread_.join();
    clear();
}

void ImageCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    lru_.clear();
}

const Texture* ImageCache::get(const Source& src) {
    if (src.key.empty() || (src.url.empty() && src.path.empty())) return nullptr;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(src.key);
    if (it != entries_.end()) {
        lru_.splice(lru_.begin(), lru_, it->second.lru);
        return it->second.failed ? nullptr : it->second.tex.get();
    }
    if (!pending_.count(src.key)) {
        pending_[src.key] = true;
        // Newest requests first: what's on screen now matters more than what scrolled past.
        queue_.push_front(src);
        if (queue_.size() > 24) {
            pending_.erase(queue_.back().key);
            queue_.pop_back();
        }
        cv_.notify_one();
    }
    return nullptr;
}

void ImageCache::pump() {
    std::vector<Decoded> ready;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ready.swap(done_);
    }
    for (auto& d : ready) {
        Entry e;
        if (d.pixels.empty()) {
            e.failed = true;
        } else {
            e.tex = std::make_unique<Texture>();
            if (e.tex->create(d.w, d.h))
                e.tex->uploadRgb565(d.pixels.data(), d.w, d.h);
            else
                e.failed = true;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.erase(d.key);
        lru_.push_front(d.key);
        e.lru = lru_.begin();
        entries_[d.key] = std::move(e);
        while (entries_.size() > MAX_ENTRIES) {
            entries_.erase(lru_.back());
            lru_.pop_back();
        }
    }
}

ImageCache::Decoded ImageCache::decode(const Source& src) {
    Decoded out;
    out.key = src.key;
    std::string bytes;
    if (!src.path.empty()) {
        if (!jelly::readFile(src.path, bytes)) return out;
    } else if (http_) {
        auto r = http_->get(src.url, headers_ ? headers_() : jelly::Headers{});
        if (!r.ok()) return out;
        bytes = std::move(r.body);
    }
    int w = 0, h = 0, n = 0;
    unsigned char* rgb = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()),
                                               static_cast<int>(bytes.size()), &w, &h, &n, 3);
    if (!rgb) return out;

    // Box-fit into maxW×maxH with nearest-neighbour sampling (sources are already server-resized).
    float scale = std::min(1.0f, std::min(static_cast<float>(src.maxW) / w, static_cast<float>(src.maxH) / h));
    out.w = std::max(1, static_cast<int>(w * scale));
    out.h = std::max(1, static_cast<int>(h * scale));
    out.pixels.resize(static_cast<size_t>(out.w) * out.h);
    for (int y = 0; y < out.h; ++y) {
        int sy = std::min(h - 1, static_cast<int>(y / scale));
        for (int x = 0; x < out.w; ++x) {
            int sx = std::min(w - 1, static_cast<int>(x / scale));
            const unsigned char* p = rgb + (static_cast<size_t>(sy) * w + sx) * 3;
            out.pixels[static_cast<size_t>(y) * out.w + x] =
                static_cast<uint16_t>(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
        }
    }
    stbi_image_free(rgb);
    return out;
}

void ImageCache::loop() {
    for (;;) {
        Source src;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;
            src = queue_.front();
            queue_.pop_front();
        }
        Decoded d = decode(src);
        std::lock_guard<std::mutex> lock(mutex_);
        done_.push_back(std::move(d));
    }
}

}  // namespace app
