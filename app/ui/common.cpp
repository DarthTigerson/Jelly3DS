#include "screens.hpp"

#include "jelly/util.hpp"

namespace app {

using jelly::DownloadStatus;
using jelly::TICKS_PER_SECOND;

int64_t resumeTicks(const std::string& itemId, const jelly::UserData* server) {
    auto local = App::get().progress->get(itemId);
    if (local && (!server || local->dirty || local->updatedAt >= server->lastPlayedDate))
        return local->played ? 0 : local->positionTicks;
    if (server) return server->played ? 0 : server->playbackPositionTicks;
    return 0;
}

bool isPlayed(const std::string& itemId, const jelly::UserData* server) {
    auto local = App::get().progress->get(itemId);
    if (local && (!server || local->dirty || local->updatedAt >= server->lastPlayedDate)) return local->played;
    return server && server->played;
}

std::string runtimeText(int64_t ticks) {
    if (ticks <= 0) return "";
    int64_t minutes = (ticks / TICKS_PER_SECOND + 30) / 60;
    if (minutes >= 60) return std::to_string(minutes / 60) + "h " + std::to_string(minutes % 60) + "m";
    return std::to_string(minutes) + " min";
}

int64_t sdFreeBytes() {
    FS_ArchiveResource res{};
    if (R_FAILED(FSUSER_GetArchiveResource(&res, SYSTEM_MEDIATYPE_SD))) return -1;
    return static_cast<int64_t>(res.freeClusters) * res.clusterSize;
}

ImageCache::Source posterFor(const jelly::Item& item, int w, int h) {
    ImageCache::Source s;
    std::string id = item.id;
    std::string tag = item.primaryImageTag;
    // Downloaded artwork works offline.
    // Downloaded artwork works offline (it is fetched first, as soon as a download starts).
    auto& dm = *App::get().downloads;
    auto st = dm.status(item.id);
    if (st && (*st == DownloadStatus::Completed || *st == DownloadStatus::Downloading)) {
        s.path = dm.thumbPath(item.id);
    } else if (!tag.empty() && App::get().online()) {
        s.url = App::get().client->imageUrl(id, tag, w * 2, h * 2);
    }
    s.key = id + "@" + std::to_string(w) + "x" + std::to_string(h);
    s.maxW = w;
    s.maxH = h;
    return s;
}

ImageCache::Source thumbFor(const jelly::DownloadItem& d, int w, int h) {
    ImageCache::Source s;
    s.path = App::get().downloads->thumbPath(d.itemId);
    s.key = d.itemId + "@" + std::to_string(w) + "x" + std::to_string(h);
    s.maxW = w;
    s.maxH = h;
    return s;
}

static ImageCache::Source remoteImage(const std::string& id, const std::string& tag, const std::string& type, int w,
                                      int h) {
    ImageCache::Source s;
    s.key = id + "/" + type + "@" + std::to_string(w) + "x" + std::to_string(h);
    if (App::get().online()) s.url = App::get().client->imageUrl(id, tag, w, h, type);
    s.maxW = w;
    s.maxH = h;
    return s;
}

ImageCache::Source stillFor(const jelly::Item& item, int w, int h) {
    if (item.type == "Episode" && !item.primaryImageTag.empty()) return posterFor(item, w, h);
    if (!item.backdropImageTag.empty()) return remoteImage(item.backdropItemId, item.backdropImageTag, "Backdrop", w, h);
    return posterFor(item, w, h);
}

ImageCache::Source coverFor(const jelly::Item& item, int w, int h) {
    if (item.type == "Episode" && !item.seriesPrimaryImageTag.empty() && !item.seriesId.empty())
        return remoteImage(item.seriesId, item.seriesPrimaryImageTag, "Primary", w, h);
    return posterFor(item, w, h);
}

ImageCache::Source backdropFor(const jelly::Item& item, int w, int h) {
    if (!item.backdropImageTag.empty()) return remoteImage(item.backdropItemId, item.backdropImageTag, "Backdrop", w, h);
    return stillFor(item, w, h);
}

DownloadBadge downloadBadge(const std::string& itemId) {
    DownloadBadge b;
    auto d = App::get().downloads->find(itemId);
    if (!d) return b;
    switch (d->status) {
    case DownloadStatus::Completed:
        b.text = "SAVED";
        b.color = color::good;
        break;
    case DownloadStatus::Queued:
        b.text = "QUEUED";
        b.color = color::dim;
        break;
    case DownloadStatus::Downloading: {
        int64_t est = d->estimatedBytes();
        int pct = est > 0 ? static_cast<int>(std::min<int64_t>(99, d->bytes * 100 / est)) : 0;
        b.text = std::to_string(pct) + "%";
        b.color = color::accent2;
        b.progress = pct / 100.0f;
        break;
    }
    case DownloadStatus::Failed:
        b.text = "FAILED";
        b.color = color::bad;
        break;
    }
    return b;
}

Detail detailFor(const jelly::Item& item) {
    Detail d;
    d.title = item.type == "Episode" ? item.name : item.label();
    if (item.type == "Episode")
        d.subtitle = item.seriesName + " \xC2\xB7 S" + std::to_string(item.parentIndexNumber) + "E" +
                     std::to_string(item.indexNumber);
    else if (item.type == "Series")
        d.subtitle = "Series";
    else if (item.type == "Season")
        d.subtitle = item.seriesName;
    else if (item.type == "Movie")
        d.subtitle = "Movie";
    d.meta = runtimeText(item.runTimeTicks);
    if (item.type == "Series" || item.type == "Season") {
        if (item.childCount > 0) d.meta = std::to_string(item.childCount) + (item.type == "Series" ? " seasons" : " episodes");
        if (item.userData.unplayedItemCount > 0)
            d.meta += (d.meta.empty() ? "" : " \xC2\xB7 ") + std::to_string(item.userData.unplayedItemCount) + " unwatched";
    }
    d.overview = item.overview;
    d.image = posterFor(item, 72, 108);
    if (item.isPlayable()) {
        int64_t resume = resumeTicks(item.id, &item.userData);
        if (isPlayed(item.id, &item.userData)) {
            d.status = "Watched";
            d.statusColor = color::good;
        } else if (resume > 0) {
            d.status = "Resume at " + jelly::formatDuration(resume / TICKS_PER_SECOND);
            d.statusColor = color::accent2;
            if (item.runTimeTicks > 0) d.progress = static_cast<float>(resume) / item.runTimeTicks;
        }
        auto badge = downloadBadge(item.id);
        if (!badge.text.empty()) {
            std::string dl = badge.text == "SAVED" ? "Downloaded" : "Download " + badge.text;
            d.status = d.status.empty() ? dl : d.status + " \xC2\xB7 " + dl;
        }
    }
    return d;
}

Detail detailFor(const jelly::DownloadItem& dl) {
    Detail d;
    d.title = dl.type == "Episode" ? dl.name : dl.label();
    if (dl.type == "Episode")
        d.subtitle = dl.seriesName + " \xC2\xB7 S" + std::to_string(dl.seasonIndex) + "E" + std::to_string(dl.episodeIndex);
    d.meta = runtimeText(dl.runTimeTicks);
    std::string tracks;
    if (!dl.audioLabel.empty()) tracks = dl.audioLabel;
    if (!dl.subtitleLabel.empty()) tracks += (tracks.empty() ? "" : " \xC2\xB7 ") + std::string("Subs: ") + dl.subtitleLabel;
    if (!tracks.empty()) d.meta += (d.meta.empty() ? "" : " \xC2\xB7 ") + tracks;
    d.overview = dl.overview;
    d.image = thumbFor(dl, 72, 108);
    switch (dl.status) {
    case DownloadStatus::Completed: {
        d.status = jelly::formatSize(dl.bytes);
        auto p = App::get().progress->get(dl.itemId);
        if (p && p->played) {
            d.status += " \xC2\xB7 Watched";
            d.statusColor = color::good;
        } else if (p && p->positionTicks > 0) {
            d.status += " \xC2\xB7 Resume at " + jelly::formatDuration(p->positionTicks / TICKS_PER_SECOND);
            d.statusColor = color::accent2;
            if (dl.runTimeTicks > 0) d.progress = static_cast<float>(p->positionTicks) / dl.runTimeTicks;
        }
        if (p && p->dirty) d.status += " \xC2\xB7 not synced";
        break;
    }
    case DownloadStatus::Downloading:
        d.status = "Downloading " + jelly::formatSize(dl.bytes) + " of ~" + jelly::formatSize(dl.estimatedBytes());
        d.statusColor = color::accent2;
        d.progress = dl.estimatedBytes() > 0 ? std::min(0.99f, static_cast<float>(dl.bytes) / dl.estimatedBytes()) : 0;
        break;
    case DownloadStatus::Queued:
        d.status = App::get().online() ? "Waiting in queue" : "Queued - waiting for your server";
        break;
    case DownloadStatus::Failed:
        d.status = "Failed: " + dl.error;
        d.statusColor = color::bad;
        break;
    }
    return d;
}

void drawDetail(Gfx& g, const Detail& d, float top, float bottom) {
    const float pad = 8;
    float x = pad, y = top + pad;
    float imgW = 72, imgH = 108;
    bool hasImage = !d.image.key.empty();
    if (hasImage) {
        g.rect(x, y, imgW, imgH, color::panel);
        if (const Texture* t = App::get().images.get(d.image)) {
            float s = std::min(imgW / t->width, imgH / t->height);
            float w = t->width * s, h = t->height * s;
            g.image(*t, x + (imgW - w) / 2, y + (imgH - h) / 2, w, h);
        }
        x += imgW + 10;
    }
    float w = BOTTOM_W - x - pad;
    float ty = y;
    g.text(d.title, x, ty, 0.58f, color::text, Align::Left, w);
    ty += 22;
    if (!d.subtitle.empty()) {
        g.text(d.subtitle, x, ty, 0.46f, color::dim, Align::Left, w);
        ty += 17;
    }
    if (!d.meta.empty()) {
        g.text(d.meta, x, ty, 0.44f, color::dim, Align::Left, w);
        ty += 17;
    }
    if (!d.status.empty()) {
        g.text(d.status, x, ty, 0.44f, d.statusColor, Align::Left, w);
        ty += 17;
    }
    if (d.progress >= 0) {
        g.progressBar(x, ty + 2, w, 4, d.progress, color::accent2);
        ty += 10;
    }
    float oy = std::max(ty + 4, hasImage ? y + imgH + 6 : ty + 4);
    int lines = static_cast<int>((bottom - oy - 4) / 13);
    if (lines > 0) g.wrapped(d.overview, pad, oy, 0.42f, color::dim, BOTTOM_W - 2 * pad, lines);
}

// --- Playback entry points --------------------------------------------------------------------

void playDownload(const jelly::DownloadItem& d, bool fromStart) {
    PlayRequest r;
    r.itemId = d.itemId;
    r.mediaSourceId = d.mediaSourceId;
    r.title = d.type == "Episode" ? d.name : d.label();
    if (d.type == "Episode")
        r.subtitle = d.seriesName + " \xC2\xB7 S" + std::to_string(d.seasonIndex) + "E" + std::to_string(d.episodeIndex);
    r.seriesId = d.seriesId;
    r.runTimeTicks = d.runTimeTicks;
    r.localPath = App::get().downloads->videoPath(d.itemId);
    r.seasonIndex = d.seasonIndex;
    r.episodeIndex = d.episodeIndex;
    if (d.introEndTicks > d.introStartTicks && d.introStartTicks >= 0) {
        r.introStart = static_cast<double>(d.introStartTicks) / TICKS_PER_SECOND;
        r.introEnd = static_cast<double>(d.introEndTicks) / TICKS_PER_SECOND;
    }
    r.startSeconds = fromStart ? 0 : static_cast<double>(resumeTicks(d.itemId, nullptr)) / TICKS_PER_SECOND;
    App::get().push(makePlayerScreen(std::move(r)));
}

void playItem(const jelly::Item& item, bool fromStart) {
    auto& a = App::get();
    auto d = a.downloads->find(item.id);
    if (d && d->status == DownloadStatus::Completed) {
        playDownload(*d, fromStart);
        return;
    }
    if (!a.online()) {
        a.toast("Offline - download this first to watch it here");
        return;
    }
    PlayRequest r;
    r.itemId = item.id;
    r.title = item.type == "Episode" ? item.name : item.label();
    if (item.type == "Episode")
        r.subtitle = item.seriesName + " \xC2\xB7 S" + std::to_string(item.parentIndexNumber) + "E" +
                     std::to_string(item.indexNumber);
    r.seriesId = item.seriesId;
    r.runTimeTicks = item.runTimeTicks;
    r.seasonIndex = item.parentIndexNumber;
    r.episodeIndex = item.indexNumber;
    r.startSeconds = fromStart ? 0 : static_cast<double>(resumeTicks(item.id, &item.userData)) / TICKS_PER_SECOND;
    if (!item.mediaSources.empty()) {
        const auto& src = item.mediaSources.front();
        r.mediaSourceId = src.id;
        r.audioStreamIndex = jelly::chooseAudioStream(src, a.config.audioLanguage);
        r.subtitleStreamIndex = jelly::chooseSubtitleStream(src, a.config.subtitleLanguage);
    }
    a.push(makePlayerScreen(std::move(r)));
}

void toggleWatched(const std::string& itemId, bool played, int64_t runTimeTicks) {
    auto& a = App::get();
    a.progress->setPlayed(itemId, played, runTimeTicks);
    a.progress->save();
    if (a.online()) a.tasks.run([itemId] { App::get().sync->pushItem(itemId); });
    a.toast(played ? "Marked as watched" : "Marked as unwatched");
}

void openItemMenu(const jelly::Item& item, std::vector<MenuOption> extra) {
    auto& a = App::get();
    std::vector<MenuOption> opts;
    int64_t resume = resumeTicks(item.id, &item.userData);
    if (resume > 0) {
        opts.push_back({"Resume from " + jelly::formatDuration(resume / TICKS_PER_SECOND), [item] { playItem(item, false); }});
        opts.push_back({"Play from the beginning", [item] { playItem(item, true); }});
    } else {
        opts.push_back({"Play", [item] { playItem(item, true); }});
    }
    auto d = a.downloads->find(item.id);
    if (!d) {
        opts.push_back({item.type == "Episode" ? "Download episode" : "Download", [item] { App::get().push(makeDownloadOptions({item})); },
                        a.online()});
    } else if (d->status == DownloadStatus::Completed) {
        std::string id = item.id;
        opts.push_back({"Delete download", [id] {
                            App::get().push(makeConfirm("Delete this download from the SD card?", "Delete", [id] {
                                App::get().downloads->remove(id);
                                App::get().toast("Download deleted");
                            }));
                        }});
    } else if (d->status == DownloadStatus::Failed) {
        std::string id = item.id;
        opts.push_back({"Retry download", [id] { App::get().downloads->retry(id); }});
    } else {
        std::string id = item.id;
        opts.push_back({"Cancel download", [id] { App::get().downloads->cancel(id); }});
    }
    bool played = isPlayed(item.id, &item.userData);
    std::string id = item.id;
    int64_t rt = item.runTimeTicks;
    opts.push_back({played ? "Mark as unwatched" : "Mark as watched", [id, played, rt] { toggleWatched(id, !played, rt); }});
    for (auto& o : extra) opts.push_back(std::move(o));
    a.push(makeMenu(item.label(), std::move(opts)));
}

void downloadItems(std::vector<jelly::Item> items) {
    std::vector<jelly::Item> todo;
    for (auto& i : items)
        if (i.isPlayable() && !App::get().downloads->status(i.id)) todo.push_back(std::move(i));
    if (todo.empty()) {
        App::get().toast("Already downloaded");
        return;
    }
    App::get().push(makeDownloadOptions(std::move(todo)));
}

void downloadSeries(const std::string& seriesId, const std::string& seasonId) {
    auto& a = App::get();
    if (!a.online()) {
        a.toast("Downloading needs your server");
        return;
    }
    a.toast("Fetching episodes\xE2\x80\xA6");
    a.tasks.run([seriesId, seasonId] {
        auto r = App::get().client->episodes(seriesId, seasonId);
        App::get().tasks.post([r]() mutable {
            if (r.ok)
                downloadItems(std::move(r.value));
            else
                App::get().toast(r.error);
        });
    });
}

void openDownloadMenu(const jelly::DownloadItem& d) {
    std::vector<MenuOption> opts;
    std::string id = d.itemId;
    if (d.status == DownloadStatus::Completed) {
        int64_t resume = resumeTicks(d.itemId, nullptr);
        if (resume > 0) {
            opts.push_back({"Resume from " + jelly::formatDuration(resume / TICKS_PER_SECOND), [d] { playDownload(d, false); }});
            opts.push_back({"Play from the beginning", [d] { playDownload(d, true); }});
        } else {
            opts.push_back({"Play", [d] { playDownload(d, true); }});
        }
        bool played = isPlayed(d.itemId, nullptr);
        int64_t rt = d.runTimeTicks;
        opts.push_back({played ? "Mark as unwatched" : "Mark as watched", [id, played, rt] { toggleWatched(id, !played, rt); }});
    } else if (d.status == DownloadStatus::Failed) {
        opts.push_back({"Retry download", [id] { App::get().downloads->retry(id); }});
    } else {
        opts.push_back({"Cancel download", [id] { App::get().downloads->cancel(id); }});
    }
    opts.push_back({"Delete", [id] {
                        App::get().push(makeConfirm("Delete this download from the SD card?", "Delete", [id] {
                            App::get().downloads->remove(id);
                            App::get().toast("Download deleted");
                        }));
                    }});
    App::get().push(makeMenu(d.label(), std::move(opts)));
}

}  // namespace app
