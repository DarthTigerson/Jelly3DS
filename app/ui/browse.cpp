#include "screens.hpp"

#include "jelly/util.hpp"

namespace app {

namespace {

using ItemsResult = jelly::Result<std::vector<jelly::Item>>;

/// Shared layout for server-backed lists: list on top, details + buttons below.
class ItemListScreen : public Screen {
public:
    explicit ItemListScreen(std::string title) : title_(std::move(title)) { list_.thumbW = 28; }

    void onResume() override { rebuildRows(); }

    void update(const Input& in) override {
        if (in.down & KEY_B) {
            App::get().pop();
            return;
        }
        list_.update(in, SCREEN_H - 28);
        buildButtons();
        buttons_.update(in);
        if (++frames_ % 30 == 0 && hasActiveDownloads()) rebuildRows();
    }

    void drawTop(Gfx& g) override {
        drawHeader(g, title_, App::get().online() ? "" : "Offline");
        if (loading_)
            drawLoading(g, 0, 28, TOP_W, SCREEN_H - 28);
        else
            list_.draw(g, 0, 28, TOP_W, SCREEN_H - 28);
    }

    void drawBottom(Gfx& g) override {
        if (const jelly::Item* item = selected())
            drawDetail(g, detail_.get(list_.selected, [&] { return detailFor(*item); }), 0, SCREEN_H - ButtonBar::HEIGHT);
        buttons_.draw(g);
    }

protected:
    /// Returns the fetch job; it runs on the worker thread, so it must capture copies, never `this`.
    virtual std::function<ItemsResult()> loader() const = 0;
    virtual void activate(const jelly::Item& item) = 0;
    virtual void extraButtons() {}

    void start() {
        loading_ = true;
        async<ItemsResult>(loader(),
                           [this](ItemsResult r) {
                               loading_ = false;
                               if (!r.ok) {
                                   list_.emptyText = r.error;
                                   return;
                               }
                               items_ = std::move(r.value);
                               rebuildRows();
                               onLoaded();
                           });
    }
    virtual void onLoaded() {}

    const jelly::Item* selected() const {
        const ListRow* row = list_.current();
        if (!row || row->tag < 0 || row->tag >= static_cast<int>(items_.size())) return nullptr;
        return &items_[row->tag];
    }

    virtual ListRow rowFor(const jelly::Item& item, int tag) {
        ListRow r;
        r.tag = tag;
        r.title = item.label();
        r.subtitle = runtimeText(item.runTimeTicks);
        if (item.type == "Series" && item.userData.unplayedItemCount > 0)
            r.subtitle = std::to_string(item.userData.unplayedItemCount) + " unwatched";
        auto badge = downloadBadge(item.id);
        r.badge = badge.text;
        r.badgeColor = badge.color;
        if (item.isPlayable()) {
            if (isPlayed(item.id, &item.userData) && badge.text.empty()) {
                r.badge = "WATCHED";
                r.badgeColor = color::dim;
            }
            int64_t resume = resumeTicks(item.id, &item.userData);
            if (resume > 0 && item.runTimeTicks > 0) r.progress = static_cast<float>(resume) / item.runTimeTicks;
            if (badge.progress >= 0) r.progress = badge.progress;
        }
        r.image = posterFor(item, 28, 34);
        return r;
    }

    void rebuildRows() {
        detail_.invalidate();
        std::vector<ListRow> rows;
        for (size_t i = 0; i < items_.size(); ++i) rows.push_back(rowFor(items_[i], static_cast<int>(i)));
        list_.setRows(std::move(rows));
    }

    bool hasActiveDownloads() const {
        for (const auto& i : items_) {
            auto s = App::get().downloads->status(i.id);
            if (s && (*s == jelly::DownloadStatus::Downloading || *s == jelly::DownloadStatus::Queued)) return true;
        }
        return false;
    }

    void buildButtons() {
        buttons_.buttons.clear();
        if (const jelly::Item* item = selected()) {
            jelly::Item copy = *item;
            buttons_.buttons.push_back({std::string(glyph::A) + " Open", KEY_A, [this, copy] { activate(copy); }});
        }
        extraButtons();
        buttons_.buttons.push_back({std::string(glyph::B) + " Back", KEY_B, [] { App::get().pop(); }});
    }

    /// Queues every not-yet-downloaded playable item after the user picks tracks.
    static void downloadAll(std::vector<jelly::Item> items) {
        std::vector<jelly::Item> todo;
        for (auto& i : items)
            if (i.isPlayable() && !App::get().downloads->status(i.id)) todo.push_back(std::move(i));
        if (todo.empty()) {
            App::get().toast("Already downloaded");
            return;
        }
        App::get().push(makeDownloadOptions(std::move(todo)));
    }

    std::string title_;
    ListView list_;
    ButtonBar buttons_;
    DetailCache detail_;
    std::vector<jelly::Item> items_;
    bool loading_ = false;
    int frames_ = 0;
};

// --- Folders / libraries ----------------------------------------------------------------------

class FolderScreen : public ItemListScreen {
public:
    explicit FolderScreen(jelly::Item folder) : ItemListScreen(folder.name), folder_(std::move(folder)) { start(); }

protected:
    std::function<ItemsResult()> loader() const override {
        return [id = folder_.id] { return loadFolder(id); };
    }

    static ItemsResult loadFolder(const std::string& folderId) {
        // Libraries can be large: fetch in pages of 200 (up to 2000 items).
        std::vector<jelly::Item> all;
        jelly::ItemQuery q;
        q.parentId = folderId;
        for (int page = 0; page < 10; ++page) {
            q.startIndex = static_cast<int>(all.size());
            auto r = App::get().client->items(q);
            if (!r.ok) return page == 0 ? ItemsResult::failure(r.error) : ItemsResult::success(std::move(all));
            all.insert(all.end(), r.value.items.begin(), r.value.items.end());
            if (static_cast<int>(all.size()) >= r.value.totalRecordCount || r.value.items.empty()) break;
        }
        return ItemsResult::success(std::move(all));
    }

    void activate(const jelly::Item& item) override {
        if (item.isPlayable())
            openItemMenu(item);
        else if (item.type == "Series")
            App::get().push(makeSeriesScreen(item));
        else
            App::get().push(makeFolderScreen(item));
    }

private:
    jelly::Item folder_;
};

// --- Series → seasons -------------------------------------------------------------------------

class SeriesScreen : public ItemListScreen {
public:
    explicit SeriesScreen(jelly::Item series) : ItemListScreen(series.name), series_(std::move(series)) { start(); }

protected:
    std::function<ItemsResult()> loader() const override {
        return [id = series_.id] { return App::get().client->seasons(id); };
    }

    void onLoaded() override {
        // One season: skip straight to the episodes.
        if (items_.size() == 1) {
            App::get().pop();
            App::get().push(makeEpisodesScreen(series_, items_.front()));
        }
    }

    ListRow rowFor(const jelly::Item& item, int tag) override {
        ListRow r = ItemListScreen::rowFor(item, tag);
        r.title = item.name;
        r.subtitle = item.userData.unplayedItemCount > 0 ? std::to_string(item.userData.unplayedItemCount) + " unwatched"
                                                         : (item.childCount ? std::to_string(item.childCount) + " episodes" : "");
        return r;
    }

    void activate(const jelly::Item& item) override { App::get().push(makeEpisodesScreen(series_, item)); }

    void extraButtons() override {
        if (!App::get().online()) return;
        std::string id = series_.id;
        buttons_.buttons.push_back({std::string(glyph::Y) + " Get all", KEY_Y, [this, id] {
                                        App::get().toast("Fetching episodes\xE2\x80\xA6");
                                        async<ItemsResult>([id] { return App::get().client->episodes(id); },
                                                           [](ItemsResult r) {
                                                               if (r.ok)
                                                                   downloadAll(std::move(r.value));
                                                               else
                                                                   App::get().toast(r.error);
                                                           });
                                    }});
    }

private:
    jelly::Item series_;
};

// --- Season → episodes ------------------------------------------------------------------------

class EpisodesScreen : public ItemListScreen {
public:
    EpisodesScreen(jelly::Item series, jelly::Item season)
        : ItemListScreen(series.name + " \xC2\xB7 " + season.name), series_(std::move(series)), season_(std::move(season)) {
        list_.thumbW = 48;
        start();
    }

protected:
    std::function<ItemsResult()> loader() const override {
        return [sid = series_.id, season = season_.id] { return App::get().client->episodes(sid, season); };
    }

    ListRow rowFor(const jelly::Item& item, int tag) override {
        ListRow r = ItemListScreen::rowFor(item, tag);
        r.title = (item.indexNumber ? std::to_string(item.indexNumber) + ". " : "") + item.name;
        r.image = posterFor(item, 48, 27);
        return r;
    }

    void activate(const jelly::Item& item) override { openItemMenu(item); }

    void extraButtons() override {
        if (const jelly::Item* item = selected()) {
            jelly::Item copy = *item;
            bool played = isPlayed(copy.id, &copy.userData);
            buttons_.buttons.push_back({std::string(glyph::X) + (played ? " Unwatch" : " Watched"), KEY_X, [this, copy, played] {
                                            toggleWatched(copy.id, !played, copy.runTimeTicks);
                                            rebuildRows();
                                        }});
        }
        if (App::get().online())
            buttons_.buttons.push_back({std::string(glyph::Y) + " Get season", KEY_Y, [this] { downloadAll(items_); }});
    }

private:
    jelly::Item series_;
    jelly::Item season_;
};

}  // namespace

std::unique_ptr<Screen> makeFolderScreen(const jelly::Item& folder) { return std::make_unique<FolderScreen>(folder); }
std::unique_ptr<Screen> makeSeriesScreen(const jelly::Item& series) { return std::make_unique<SeriesScreen>(series); }
std::unique_ptr<Screen> makeEpisodesScreen(const jelly::Item& series, const jelly::Item& season) {
    return std::make_unique<EpisodesScreen>(series, season);
}

}  // namespace app
