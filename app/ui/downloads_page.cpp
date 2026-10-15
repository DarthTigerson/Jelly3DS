#include "downloads_page.hpp"

#include "jelly/util.hpp"

#include <map>

namespace app {

namespace {

using jelly::DownloadItem;
using jelly::DownloadStatus;
using jelly::TICKS_PER_SECOND;

constexpr float CHIP_Y = 178, CHIP_H = 22;
constexpr float SCREENS_X = 12, SCREENS_W = 92;
constexpr float RETRY_X = 112, RETRY_W = 120;

std::string groupKey(const DownloadItem& d) { return d.type == "Episode" && !d.seriesId.empty() ? d.seriesId : d.itemId; }

std::string episodeTitle(const DownloadItem& d) {
    if (d.type != "Episode") return d.label();
    char buf[16];
    std::snprintf(buf, sizeof(buf), "S%02dE%02d", d.seasonIndex, d.episodeIndex);
    return std::string(buf) + " \xC2\xB7 " + d.name;
}

int percent(const DownloadItem& d) {
    int64_t est = d.estimatedBytes();
    return est > 0 ? static_cast<int>(std::min<int64_t>(99, d.bytes * 100 / est)) : 0;
}

std::string minutes(int64_t seconds) {
    if (seconds < 60) return "under a minute";
    return std::to_string((seconds + 59) / 60) + " min";
}

ImageCache::Source coverOf(const DownloadItem& d, int w, int h) {
    ImageCache::Source s;
    auto& dm = *App::get().downloads;
    s.path = jelly::fileExists(dm.posterPath(d.itemId)) ? dm.posterPath(d.itemId) : dm.thumbPath(d.itemId);
    s.key = "dlposter/" + d.itemId + "@" + std::to_string(w) + "x" + std::to_string(h);
    s.maxW = w;
    s.maxH = h;
    return s;
}

/// "Y Episodes": every download of one show, with the full per-episode menu.
class DownloadGroupScreen : public Screen {
public:
    DownloadGroupScreen(std::string key, std::string title) : key_(std::move(key)), title_(std::move(title)) {
        list_.thumbW = 64;
        list_.rowHeight = 46;
        rebuild();
    }

    void onResume() override { rebuild(); }

    void update(const Input& in) override {
        if (in.down & KEY_B) {
            App::get().pop();
            return;
        }
        list_.update(in, SCREEN_H - 28);
        if (++frames_ % 30 == 0) rebuild();
        hints_.hints.clear();
        if (const ListRow* r = list_.current(); r && r->tag >= 0 && r->tag < static_cast<int>(items_.size())) {
            DownloadItem d = items_[r->tag];
            if (d.status == DownloadStatus::Completed)
                hints_.hints.push_back({glyph::A, "Play", KEY_A, [d] { playDownload(d, false); }});
            hints_.hints.push_back({glyph::Y, "More", KEY_Y, [d] { openDownloadMenu(d); }});
            hints_.hints.push_back({glyph::B, "Back", 0, [] { App::get().pop(); }});
        }
        hints_.update(in);
    }

    void drawTop(Gfx& g) override {
        drawHeader(g, title_, std::to_string(items_.size()) + " downloads");
        list_.draw(g, 0, 28, TOP_W, SCREEN_H - 28);
    }

    void drawBottom(Gfx& g) override {
        if (const ListRow* r = list_.current(); r && r->tag >= 0 && r->tag < static_cast<int>(items_.size()))
            drawDetail(g, detailFor(items_[r->tag]), 0, SCREEN_H - HintStrip::HEIGHT);
        hints_.draw(g);
    }

private:
    void rebuild() {
        items_.clear();
        for (const auto& d : App::get().downloads->items())
            if (groupKey(d) == key_) items_.push_back(d);
        std::sort(items_.begin(), items_.end(), [](const DownloadItem& a, const DownloadItem& b) {
            return std::make_pair(a.seasonIndex, a.episodeIndex) < std::make_pair(b.seasonIndex, b.episodeIndex);
        });
        std::vector<ListRow> rows;
        for (size_t i = 0; i < items_.size(); ++i) {
            const auto& d = items_[i];
            ListRow r;
            r.tag = static_cast<int>(i);
            r.title = episodeTitle(d);
            auto det = detailFor(d);
            r.subtitle = det.status;
            r.progress = det.progress;
            auto badge = downloadBadge(d.itemId);
            r.badge = d.status == DownloadStatus::Completed && isPlayed(d.itemId, nullptr) ? "WATCHED" : badge.text;
            r.badgeColor = r.badge == "WATCHED" ? color::dim : badge.color;
            r.image = thumbFor(d, 64, 36);
            rows.push_back(std::move(r));
        }
        list_.emptyText = "Nothing left from this show";
        list_.setRows(std::move(rows));
    }

    std::string key_, title_;
    std::vector<DownloadItem> items_;
    ListView list_;
    HintStrip hints_;
    int frames_ = 0;
};

}  // namespace

DownloadsPage::DownloadsPage() { refresh(); }

void DownloadsPage::refresh() {
    all_ = App::get().downloads->items();
    rebuildGroups();
    u64 now = osGetTime();
    if (now >= nextFreeCheck_) {
        freeBytes_ = sdFreeBytes();
        nextFreeCheck_ = now + 5000;
    }
    int queued = static_cast<int>(queueItems().size());
    if (queueSel_ >= queued) queueSel_ = std::max(0, queued - 1);
}

void DownloadsPage::rebuildGroups() {
    std::map<std::string, size_t> index;
    groups_.clear();
    for (const auto& d : all_) {
        if (d.status != DownloadStatus::Completed && d.status != DownloadStatus::Downloading) continue;
        std::string key = groupKey(d);
        auto it = index.find(key);
        if (it == index.end()) {
            index[key] = groups_.size();
            Group g;
            g.key = key;
            g.title = d.type == "Episode" && !d.seriesName.empty() ? d.seriesName : d.label();
            groups_.push_back(g);
            it = index.find(key);
        }
        auto& g = groups_[it->second];
        g.items.push_back(d);
        if (d.status == DownloadStatus::Completed) {
            ++g.saved;
            g.bytes += d.bytes;
        } else {
            g.downloadingPct = percent(d);
        }
    }
    for (auto& g : groups_)
        std::sort(g.items.begin(), g.items.end(), [](const DownloadItem& a, const DownloadItem& b) {
            return std::make_pair(a.seasonIndex, a.episodeIndex) < std::make_pair(b.seasonIndex, b.episodeIndex);
        });
    std::sort(groups_.begin(), groups_.end(), [](const Group& a, const Group& b) { return a.title < b.title; });

    std::vector<ListRow> rows;
    for (size_t i = 0; i < groups_.size(); ++i) {
        const auto& g = groups_[i];
        ListRow r;
        r.tag = static_cast<int>(i);
        r.title = g.title;
        if (g.downloadingPct >= 0) {
            r.badge = std::to_string(g.downloadingPct) + "%";
            r.badgeColor = color::accent2;
        } else if (g.items.size() > 1 || g.items.front().type == "Episode") {
            r.badge = std::to_string(g.saved);
            r.badgeColor = color::text;
        }
        if (const DownloadItem* next = nextToWatch(g)) {
            auto p = App::get().progress->get(next->itemId);
            if (p && p->positionTicks > 0 && next->runTimeTicks > 0)
                r.progress = static_cast<float>(p->positionTicks) / next->runTimeTicks;
        }
        r.image = coverOf(g.items.front(), 64, 96);
        rows.push_back(std::move(r));
    }
    int queued = static_cast<int>(queueItems().size());
    grid_.emptyText = queued > 0 ? std::to_string(queued) + " in the queue - press SELECT to see it"
                                 : "No downloads yet - open an episode and choose Download";
    grid_.setItems(std::move(rows));
}

const DownloadsPage::Group* DownloadsPage::currentGroup() const {
    const ListRow* r = grid_.current();
    if (!r || r->tag < 0 || r->tag >= static_cast<int>(groups_.size())) return nullptr;
    return &groups_[r->tag];
}

const DownloadItem* DownloadsPage::nextToWatch(const Group& g) const {
    for (const auto& d : g.items)
        if (d.status == DownloadStatus::Completed && !isPlayed(d.itemId, nullptr)) return &d;
    return nullptr;
}

std::vector<DownloadItem> DownloadsPage::queueItems() const {
    std::vector<DownloadItem> out;
    for (auto s : {DownloadStatus::Downloading, DownloadStatus::Queued, DownloadStatus::Failed})
        for (const auto& d : all_)
            if (d.status == s) out.push_back(d);
    return out;
}

void DownloadsPage::deleteWatched(const Group& g) {
    std::vector<std::string> ids;
    for (const auto& d : g.items)
        if (d.status == DownloadStatus::Completed && isPlayed(d.itemId, nullptr)) ids.push_back(d.itemId);
    if (ids.empty()) {
        App::get().toast("Nothing watched to delete");
        return;
    }
    std::string q = "Delete " + std::to_string(ids.size()) + " watched episode" + (ids.size() == 1 ? "" : "s") + "?";
    App::get().push(makeConfirm(q, "Delete", [ids] {
        for (const auto& id : ids) App::get().downloads->remove(id);
        App::get().toast("Freed up space");
    }));
}

std::string DownloadsPage::headerText() const {
    if (queueView_) {
        auto q = queueItems();
        int waiting = 0;
        int64_t bytesLeft = 0, speed = 0;
        for (const auto& d : q) {
            if (d.status == DownloadStatus::Failed) continue;
            ++waiting;
            bytesLeft += std::max<int64_t>(0, d.estimatedBytes() - d.bytes);
            if (d.status == DownloadStatus::Downloading) speed = d.speedBps;
        }
        if (App::get().downloads->paused()) return std::to_string(waiting) + " waiting \xC2\xB7 paused";
        if (waiting == 0) return "Queue empty";
        return std::to_string(waiting) + " waiting" + (speed > 0 ? " \xC2\xB7 about " + minutes(bytesLeft / speed) : "");
    }
    return freeBytes_ >= 0 ? jelly::formatSize(freeBytes_) + " free" : "";
}

void DownloadsPage::buildHints() {
    hints_.hints.clear();
    auto& dm = *App::get().downloads;
    if (!queueView_) {
        if (const Group* g = currentGroup()) {
            if (const DownloadItem* next = nextToWatch(*g)) {
                DownloadItem d = *next;
                hints_.hints.push_back({glyph::A, "Play next", KEY_A, [d] { playDownload(d, false); }});
            }
            std::string key = g->key, title = g->title;
            hints_.hints.push_back({glyph::Y, "Episodes", KEY_Y, [key, title] {
                                        App::get().push(std::make_unique<DownloadGroupScreen>(key, title));
                                    }});
            Group copy = *g;
            hints_.hints.push_back({glyph::X, "Delete watched", KEY_X, [this, copy] { deleteWatched(copy); }});
        }
        hints_.hints.push_back({"SELECT", "Saved", KEY_SELECT, [this] { queueView_ = true; }});
        return;
    }
    bool paused = dm.paused();
    hints_.hints.push_back({glyph::A, paused ? "Resume queue" : "Pause queue", KEY_A, [paused] {
                                App::get().downloads->setPaused(!paused);
                                App::get().toast(paused ? "Downloads resumed" : "Queue paused after this download");
                            }});
    auto q = queueItems();
    if (queueSel_ < static_cast<int>(q.size())) {
        DownloadItem d = q[queueSel_];
        if (d.status == DownloadStatus::Queued)
            hints_.hints.push_back({glyph::Y, "Move up", KEY_Y, [this, d] {
                                        App::get().downloads->moveUp(d.itemId);
                                        if (queueSel_ > 0) --queueSel_;
                                    }});
        bool failed = d.status == DownloadStatus::Failed;
        // Both cancel (a running/queued download) and remove (a failed one) delete its files.
        hints_.hints.push_back({glyph::X, failed ? "Remove" : "Cancel", KEY_X, [d] { App::get().downloads->remove(d.itemId); }});
    }
    hints_.hints.push_back({"SELECT", "Queue", KEY_SELECT, [this] { queueView_ = false; }});
}

void DownloadsPage::update(const Input& in) {
    if (queueView_) {
        int n = static_cast<int>(queueItems().size());
        if (n > 0) {
            if (in.repeat & KEY_DOWN) queueSel_ = std::min(n - 1, queueSel_ + 1);
            if (in.repeat & KEY_UP) queueSel_ = std::max(0, queueSel_ - 1);
        }
        if (in.tapped && in.tapY >= CHIP_Y && in.tapY < CHIP_Y + CHIP_H) {
            if (in.tapX >= SCREENS_X && in.tapX < SCREENS_X + SCREENS_W) App::get().screensOff();
            if (in.tapX >= RETRY_X && in.tapX < RETRY_X + RETRY_W) App::get().downloads->retryFailed();
        }
    } else {
        grid_.update(in);
    }
    buildHints();
    hints_.update(in);
}

// --- Top screen -------------------------------------------------------------------------------

void DownloadsPage::drawTop(Gfx& g, float y, float h) {
    if (queueView_)
        drawQueueTop(g, y, h);
    else
        grid_.draw(g, 0, y, TOP_W, h);
}

void DownloadsPage::drawQueueTop(Gfx& g, float y, float h) {
    auto q = queueItems();
    if (q.empty()) {
        g.text("Nothing is downloading", TOP_W / 2.0f, y + h / 2 - 10, 0.5f, color::dim, Align::Center);
        return;
    }
    float ry = y + 8;
    // Keep the selection on screen: skip rows above it when the list is long.
    int first = std::max(0, queueSel_ - 3);
    for (int i = first; i < static_cast<int>(q.size()) && ry < y + h; ++i) {
        const auto& d = q[i];
        bool sel = i == queueSel_;
        bool big = d.status == DownloadStatus::Downloading;
        float rh = big ? 76 : 40;
        if (sel) g.rect(8, ry - 2, TOP_W - 16, rh + 4, color::accent);
        g.rect(10, ry, TOP_W - 20, rh, big ? color::panelHi : color::panel);
        float tw = big ? 112 : 56, th = big ? 63 : 32;
        float tx = 16, ty = ry + (rh - th) / 2;
        g.rect(tx, ty, tw, th, color::bg);
        if (const Texture* t = App::get().images.get(thumbFor(d, 112, 63))) g.imageCover(*t, tx, ty, tw, th);
        float x = tx + tw + 10, w = TOP_W - x - 18;
        std::string show = d.type == "Episode" ? d.seriesName : d.name;
        if (big) {
            g.text(show, x, ry + 7, 0.5f, color::text, Align::Left, w);
            g.text(d.type == "Episode" ? episodeTitle(d) : "", x, ry + 24, 0.42f, color::dim, Align::Left, w);
            int64_t est = d.estimatedBytes();
            g.progressBar(x, ry + 44, w, 6, est > 0 ? std::min(0.99f, static_cast<float>(d.bytes) / est) : 0, color::accent2,
                          color::bg);
            std::string stats = jelly::formatSize(d.bytes) + " of ~" + jelly::formatSize(est);
            if (d.speedBps > 0) stats += " \xC2\xB7 " + std::to_string(d.speedBps / 1024) + " KB/s";
            g.text(stats, x, ry + 54, 0.38f, color::accent2, Align::Left, w - 70);
            if (d.speedBps > 0 && est > d.bytes)
                g.text(minutes((est - d.bytes) / d.speedBps) + " left", x + w, ry + 54, 0.38f, color::accent2, Align::Right);
        } else {
            std::string title = d.type == "Episode" ? show + " \xC2\xB7 " + episodeTitle(d) : show;
            g.text(title, x, ry + 5, 0.44f, color::text, Align::Left, w - 50);
            bool failed = d.status == DownloadStatus::Failed;
            std::string meta = failed ? d.error : "~" + jelly::formatSize(d.estimatedBytes());
            g.text(meta, x, ry + 21, 0.38f, failed ? color::bad : color::dim, Align::Left, w - 50);
            g.text(failed ? "FAILED" : "QUEUED", x + w, ry + 13, 0.36f, failed ? color::bad : color::dim, Align::Right);
        }
        ry += rh + 8;
    }
}

// --- Bottom screen ----------------------------------------------------------------------------

void DownloadsPage::drawBottom(Gfx& g, float top) {
    if (queueView_)
        drawQueueBottom(g, top);
    else
        drawSavedBottom(g, top);
    hints_.draw(g);
}

void DownloadsPage::drawSavedBottom(Gfx& g, float top) {
    const Group* grp = currentGroup();
    if (!grp) {
        g.wrapped(grid_.emptyText, 14, top + 40, 0.48f, color::dim, BOTTOM_W - 28, 5);
        return;
    }
    float y = top + 8;
    std::string summary = grp->items.front().type == "Episode"
                              ? std::to_string(grp->saved) + " episode" + (grp->saved == 1 ? "" : "s")
                              : "Movie";
    summary += " \xC2\xB7 " + jelly::formatSize(grp->bytes);
    float sw = g.textWidth(summary, 0.38f);
    g.text(grp->title, 12, y, 0.55f, color::text, Align::Left, BOTTOM_W - 34 - sw);
    g.text(summary, BOTTOM_W - 12, y + 4, 0.38f, color::dim, Align::Right);
    y += 24;

    const DownloadItem* next = nextToWatch(*grp);
    int start = 0;
    if (next)
        for (size_t i = 0; i < grp->items.size(); ++i)
            if (grp->items[i].itemId == next->itemId) start = std::max(0, static_cast<int>(i) - 1);
    const float rowH = 38;
    for (int i = start; i < static_cast<int>(grp->items.size()) && y + rowH <= SCREEN_H - HintStrip::HEIGHT; ++i) {
        const auto& d = grp->items[i];
        bool isNext = next && d.itemId == next->itemId;
        if (isNext) g.rect(6, y, BOTTOM_W - 12, rowH - 2, color::panelHi);
        g.rect(10, y + 3, 56, 32, color::panel);
        if (const Texture* t = App::get().images.get(thumbFor(d, 64, 36))) g.imageCover(*t, 10, y + 3, 56, 32);
        auto p = App::get().progress->get(d.itemId);
        if (p && p->positionTicks > 0 && d.runTimeTicks > 0)
            g.progressBar(10, y + 33, 56, 2, static_cast<float>(p->positionTicks) / d.runTimeTicks, color::accent2, color::bg);
        g.text(episodeTitle(d), 74, y + 4, 0.44f, color::text, Align::Left, BOTTOM_W - 86);
        std::string meta;
        u32 metaColor = color::dim;
        if (d.status == DownloadStatus::Downloading) {
            meta = "Downloading " + std::to_string(percent(d)) + "%";
            metaColor = color::accent2;
        } else if (isNext) {
            meta = p && p->positionTicks > 0 ? "Up next \xC2\xB7 Resume at " + jelly::formatDuration(p->positionTicks / TICKS_PER_SECOND)
                                             : "Up next";
            metaColor = color::accent2;
        } else if (isPlayed(d.itemId, nullptr)) {
            meta = "Watched \xC2\xB7 " + jelly::formatSize(d.bytes);
        } else {
            meta = runtimeText(d.runTimeTicks) + " \xC2\xB7 " + jelly::formatSize(d.bytes);
        }
        g.text(meta, 74, y + 20, 0.38f, metaColor, Align::Left, BOTTOM_W - 86);
        y += rowH;
    }
}

void DownloadsPage::drawQueueBottom(Gfx& g, float top) {
    float x = 12, y = top + 10, w = BOTTOM_W - 24;
    g.text("SD CARD", x, y, 0.38f, color::accent);
    y += 16;
    int64_t used = 0, queued = 0;
    int failed = 0;
    for (const auto& d : all_) {
        if (d.status == DownloadStatus::Completed) used += d.bytes;
        if (d.status == DownloadStatus::Queued || d.status == DownloadStatus::Downloading)
            queued += std::max<int64_t>(0, d.estimatedBytes() - d.bytes);
        if (d.status == DownloadStatus::Failed) ++failed;
    }
    int64_t total = std::max<int64_t>(1, used + queued + std::max<int64_t>(0, freeBytes_));
    g.rect(x, y, w, 10, color::panelHi);
    float uw = w * used / total, qw = w * queued / total;
    g.rect(x, y, uw, 10, color::accent2);
    g.rect(x + uw, y, qw, 10, color::rgba(127, 211, 240));
    y += 14;
    g.text("Jelly3DS " + jelly::formatSize(used), x, y, 0.36f, color::accent2);
    g.text("Queue ~" + jelly::formatSize(queued), BOTTOM_W / 2.0f, y, 0.36f, color::rgba(127, 211, 240), Align::Center);
    g.text(freeBytes_ >= 0 ? jelly::formatSize(freeBytes_) + " free" : "", x + w, y, 0.36f, color::dim, Align::Right);
    y += 22;
    g.text("WHILE DOWNLOADING", x, y, 0.38f, color::accent);
    y += 15;
    g.wrapped("The console stays awake until the queue is done. Close the lid: the screens turn off and the download "
              "keeps going, then the 3DS sleeps once it finishes.",
              x, y, 0.4f, color::rgba(201, 207, 223), w, 4);

    g.roundRect(SCREENS_X, CHIP_Y, SCREENS_W, CHIP_H, color::panelHi);
    g.text("Screens off", SCREENS_X + SCREENS_W / 2, CHIP_Y + 4, 0.4f, color::text, Align::Center);
    g.roundRect(RETRY_X, CHIP_Y, RETRY_W, CHIP_H, failed ? color::panelHi : color::panel);
    g.text("Retry failed (" + std::to_string(failed) + ")", RETRY_X + RETRY_W / 2, CHIP_Y + 4, 0.4f,
           failed ? color::text : color::dim, Align::Center);
}

}  // namespace app
