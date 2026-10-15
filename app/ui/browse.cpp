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
        if (grid_)
            gridView_.update(in);
        else
            list_.update(in, SCREEN_H - 28);
        buildButtons();
        buttons_.update(in);
        if (++frames_ % 30 == 0 && hasActiveDownloads()) rebuildRows();
    }

    void drawTop(Gfx& g) override {
        drawHeader(g, title_, App::get().online() ? "" : "Offline");
        if (loading_)
            drawLoading(g, 0, 28, TOP_W, SCREEN_H - 28);
        else if (grid_)
            gridView_.draw(g, 0, 28, TOP_W, SCREEN_H - 28);
        else
            list_.draw(g, 0, 28, TOP_W, SCREEN_H - 28);
    }

    void drawBottom(Gfx& g) override {
        if (const jelly::Item* item = selected())
            drawDetail(g, detail_.get(grid_ ? gridView_.selected : list_.selected, [&] { return detailFor(*item); }), 0,
                       SCREEN_H - ButtonBar::HEIGHT);
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
                                   list_.emptyText = gridView_.emptyText = r.error;
                                   return;
                               }
                               items_ = std::move(r.value);
                               // Posters for shows, movies, seasons and folders; a list for episodes.
                               grid_ = !items_.empty() && std::none_of(items_.begin(), items_.end(), [](const jelly::Item& i) {
                                   return i.type == "Episode";
                               });
                               rebuildRows();
                               onLoaded();
                           });
    }
    virtual void onLoaded() {}

    const jelly::Item* selected() const {
        const ListRow* row = grid_ ? gridView_.current() : list_.current();
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
        for (size_t i = 0; i < items_.size(); ++i) {
            rows.push_back(rowFor(items_[i], static_cast<int>(i)));
            if (grid_) {
                rows.back().image = posterFor(items_[i], 64, 96);
                rows.back().title = items_[i].name;
            }
        }
        if (grid_)
            gridView_.setItems(std::move(rows));
        else
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
    GridView gridView_;
    bool grid_ = false;
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

}  // namespace

std::unique_ptr<Screen> makePlainFolderScreen(const jelly::Item& folder) { return std::make_unique<FolderScreen>(folder); }

}  // namespace app
