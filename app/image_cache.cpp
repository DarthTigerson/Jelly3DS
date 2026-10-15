#include "image_cache.hpp"

#include "jelly/cache.hpp"
#include "jelly/util.hpp"

#include <cstdio>
#include <filesystem>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include <stb/stb_image.h>

namespace app {

void ImageCache::start(jelly::IHttp* http, std::function<jelly::Headers()> headers, std::string diskDir) {
    http_ = http;
    diskDir_ = std::move(diskDir);
    if (!diskDir_.empty()) jelly::makeDirs(diskDir_);
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
    bytes_ = 0;
}

const Texture* ImageCache::get(const Source& src) {
    if (src.key.empty() || (src.url.empty() && src.path.empty())) return nullptr;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(src.key);
    if (it != entries_.end()) {
        // A failed load (offline, server hiccup) is retried after 30 s rather than staying blank.
        if (it->second.failed && osGetTime() - it->second.failedAt > 30000) {
            lru_.erase(it->second.lru);
            entries_.erase(it);
        } else {
            lru_.splice(lru_.begin(), lru_, it->second.lru);
            return it->second.failed ? nullptr : it->second.tex.get();
        }
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
            e.failedAt = osGetTime();
        } else {
            e.tex = std::make_unique<Texture>();
            if (e.tex->create(d.w, d.h)) {
                e.tex->uploadRgb565(d.pixels.data(), d.w, d.h);
            } else {
                e.tex.reset();
                e.failed = true;
                e.failedAt = osGetTime();
            }
        }
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.erase(d.key);
        lru_.push_front(d.key);
        e.lru = lru_.begin();
        if (e.tex) bytes_ += e.tex->tex.size;
        entries_[d.key] = std::move(e);
        while (entries_.size() > MAX_ENTRIES || (bytes_ > MAX_BYTES && entries_.size() > 1)) {
            auto victim = entries_.find(lru_.back());
            if (victim != entries_.end() && victim->second.tex) bytes_ -= victim->second.tex->tex.size;
            entries_.erase(victim);
            lru_.pop_back();
        }
    }
}

bool ImageCache::loadDisk(const std::string& file, Decoded& out) {
    FILE* f = std::fopen(file.c_str(), "rb");
    if (!f) return false;
    uint16_t wh[2] = {0, 0};
    bool ok = std::fread(wh, sizeof(wh), 1, f) == 1 && wh[0] > 0 && wh[1] > 0 && wh[0] <= 1024 && wh[1] <= 1024;
    if (ok) {
        out.w = wh[0];
        out.h = wh[1];
        out.pixels.resize(static_cast<size_t>(out.w) * out.h);
        ok = std::fread(out.pixels.data(), 2, out.pixels.size(), f) == out.pixels.size();
    }
    std::fclose(f);
    if (!ok) out.pixels.clear();
    return ok;
}

void ImageCache::saveDisk(const std::string& file, const Decoded& d) {
    std::string tmp = file + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return;
    uint16_t wh[2] = {static_cast<uint16_t>(d.w), static_cast<uint16_t>(d.h)};
    bool ok = std::fwrite(wh, sizeof(wh), 1, f) == 1 && std::fwrite(d.pixels.data(), 2, d.pixels.size(), f) == d.pixels.size();
    std::fclose(f);
    if (ok)
        std::rename(tmp.c_str(), file.c_str());
    else
        std::remove(tmp.c_str());
}

/// Keeps the SD image cache under 64 MB by deleting the least recently written files.
void ImageCache::pruneDisk() {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<std::pair<fs::file_time_type, std::pair<std::string, uintmax_t>>> files;
    uintmax_t total = 0;
    for (const auto& e : fs::directory_iterator(diskDir_, ec)) {
        if (!e.is_regular_file(ec)) continue;
        uintmax_t size = e.file_size(ec);
        total += size;
        files.push_back({e.last_write_time(ec), {e.path().string(), size}});
    }
    const uintmax_t limit = 64ull * 1024 * 1024, target = 48ull * 1024 * 1024;
    if (total <= limit) return;
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
        if (total <= target) break;
        fs::remove(f.second.first, ec);
        total -= f.second.second;
    }
}

ImageCache::Decoded ImageCache::decode(const Source& src) {
    Decoded out;
    out.key = src.key;
    // Decoded pictures are kept on the SD card: next time there is no download and no JPEG decode.
    // The URL carries Jellyfin's image tag, so changed artwork gets a new file automatically.
    std::string diskFile;
    if (!diskDir_.empty()) {
        diskFile = diskDir_ + "/" +
                   jelly::hashName((src.path.empty() ? src.url : src.path) + "@" + std::to_string(src.maxW) + "x" +
                                   std::to_string(src.maxH)) +
                   ".565";
        if (loadDisk(diskFile, out)) return out;
    }
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

    // Box-fit into maxW×maxH, averaging every source pixel that falls in each destination pixel.
    float scale = std::min(1.0f, std::min(static_cast<float>(src.maxW) / w, static_cast<float>(src.maxH) / h));
    out.w = std::max(1, static_cast<int>(w * scale));
    out.h = std::max(1, static_cast<int>(h * scale));
    out.pixels.resize(static_cast<size_t>(out.w) * out.h);
    for (int y = 0; y < out.h; ++y) {
        int y0 = y * h / out.h, y1 = std::max(y0 + 1, (y + 1) * h / out.h);
        for (int x = 0; x < out.w; ++x) {
            int x0 = x * w / out.w, x1 = std::max(x0 + 1, (x + 1) * w / out.w);
            unsigned r = 0, gsum = 0, b = 0, n = 0;
            for (int sy = y0; sy < y1; ++sy) {
                const unsigned char* p = rgb + (static_cast<size_t>(sy) * w + x0) * 3;
                for (int sx = x0; sx < x1; ++sx, p += 3) {
                    r += p[0];
                    gsum += p[1];
                    b += p[2];
                    ++n;
                }
            }
            r /= n;
            gsum /= n;
            b /= n;
            out.pixels[static_cast<size_t>(y) * out.w + x] =
                static_cast<uint16_t>(((r >> 3) << 11) | ((gsum >> 2) << 5) | (b >> 3));
        }
    }
    stbi_image_free(rgb);
    if (!diskFile.empty()) saveDisk(diskFile, out);
    return out;
}

void ImageCache::loop() {
    if (!diskDir_.empty()) pruneDisk();
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
