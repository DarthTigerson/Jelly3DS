#include "cover_page.hpp"
#include "screens.hpp"

#include "jelly/util.hpp"

namespace app {

namespace {

using ItemsResult = jelly::Result<std::vector<jelly::Item>>;

constexpr float CRUMB_H = 22;

/// A pushed screen built around a CoverPage: header on top, breadcrumb + details + hints below.
class CoverScreen : public Screen {
public:
    CoverScreen(std::string title, std::string crumb, CoverPage::Options options)
        : title_(std::move(title)), crumb_(std::move(crumb)), page_(std::move(options)) {}

    void onResume() override { page_.refreshBadges(); }

    void update(const Input& in) override {
        if (in.down & KEY_B) {
            App::get().pop();
            return;
        }
        page_.update(in);
        if (++frames_ % 30 == 0) page_.refreshBadges();  // download progress, watched changes
    }

    void drawTop(Gfx& g) override {
        drawHeader(g, title_ + subtitle(), App::get().online() || letters_ ? "" : "Offline");
        if (letters_) page_.drawLetters(g, TOP_W - 8, 5);
        page_.drawTop(g, 28, SCREEN_H - 28);
    }

    void drawBottom(Gfx& g) override {
        g.rect(0, 0, BOTTOM_W, CRUMB_H, color::panel);
        g.text(crumb_, 10, 4, 0.42f, color::dim, Align::Left, BOTTOM_W - 20);
        page_.drawBottom(g, CRUMB_H);
    }

protected:
    virtual std::string subtitle() const { return ""; }

    /// `job` runs on the worker thread: it must capture copies, never `this`.
    void load(std::function<ItemsResult()> job) {
        page_.setLoading(true);
        async<ItemsResult>(std::move(job), [this](ItemsResult r) {
            page_.setLoading(false);
            if (!r.ok) {
                page_.setEmptyText(r.error);
                page_.setItems({});
                return;
            }
            page_.setItems(std::move(r.value));
            onLoaded();
        });
    }
    virtual void onLoaded() {}

    std::string title_, crumb_;
    CoverPage page_;
    bool letters_ = false;
    int frames_ = 0;
};

// --- Shows / Movies library -------------------------------------------------------------------

struct Sort {
    const char* label;
    const char* sortBy;
    const char* order;
    const char* filters;
};
const Sort SORTS[] = {
    {"A\xE2\x80\x93Z", "SortName", "Ascending", ""},
    {"Newest", "DateCreated", "Descending", ""},
    {"Unwatched", "SortName", "Ascending", "IsUnplayed"},
};

class LibraryScreen : public CoverScreen {
public:
    explicit LibraryScreen(jelly::Item lib)
        : CoverScreen(lib.name, "Libraries \xE2\x80\xBA " + lib.name, options(lib)), lib_(std::move(lib)) {
        letters_ = true;
        reload();
    }

private:
    bool movies() const { return lib_.collectionType == "movies"; }

    CoverPage::Options options(const jelly::Item& lib) {
        bool isMovies = lib.collectionType == "movies";
        CoverPage::Options o;
        o.viewKey = lib.collectionType;  // "tvshows" | "movies"
        o.defaultView = isMovies ? "hero" : "grid";
        o.letterJump = true;
        o.hints = [this, isMovies](const jelly::Item* item) {
            std::vector<Hint> h;
            if (item) {
                jelly::Item copy = *item;
                if (isMovies) {
                    bool resume = resumeTicks(copy.id, &copy.userData) > 0;
                    h.push_back({glyph::A, resume ? "Resume" : "Play", KEY_A, [copy] { playItem(copy, false); }});
                    h.push_back({glyph::Y, "More", KEY_Y, [copy] { openItemMenu(copy); }});
                } else {
                    h.push_back({glyph::A, "Open", KEY_A, [copy] { App::get().push(makeSeriesScreen(copy)); }});
                    h.push_back({glyph::Y, "More", KEY_Y, [copy] { seriesMenu(copy); }});
                }
            }
            h.push_back({glyph::X, std::string("Sort: ") + SORTS[sort_].label, KEY_X, [this] {
                             sort_ = (sort_ + 1) % 3;
                             reload();
                         }});
            return h;
        };
        return o;
    }

    static void seriesMenu(const jelly::Item& series) {
        std::string id = series.id;
        App::get().push(makeMenu(series.name, {
                                                  {"Open", [series] { App::get().push(makeSeriesScreen(series)); }},
                                                  {"Download series", [id] { downloadSeries(id); }, App::get().online()},
                                              }));
    }

    std::string subtitle() const override { return std::string(" \xC2\xB7 ") + SORTS[sort_].label; }

    void reload() {
        letters_ = SORTS[sort_].sortBy == std::string("SortName");  // L/R letter jumps only make sense A–Z
        page_.setHeroLabel(std::string(movies() ? "MOVIES" : "SHOWS") + " \xC2\xB7 " + SORTS[sort_].label);
        page_.setEmptyText(SORTS[sort_].filters[0] ? "Everything here is watched" : "This library is empty");
        jelly::ItemQuery q;
        q.parentId = lib_.id;
        q.includeItemTypes = movies() ? "Movie" : "Series";
        q.recursive = true;
        q.sortBy = SORTS[sort_].sortBy;
        q.sortOrder = SORTS[sort_].order;
        q.filters = SORTS[sort_].filters;
        load([q]() mutable {
            // Large libraries: pages of 200, up to 2000 items.
            std::vector<jelly::Item> all;
            for (int page = 0; page < 10; ++page) {
                q.startIndex = static_cast<int>(all.size());
                auto r = App::get().client->items(q);
                if (!r.ok) return page == 0 ? ItemsResult::failure(r.error) : ItemsResult::success(std::move(all));
                all.insert(all.end(), r.value.items.begin(), r.value.items.end());
                if (static_cast<int>(all.size()) >= r.value.totalRecordCount || r.value.items.empty()) break;
            }
            return ItemsResult::success(std::move(all));
        });
    }

    jelly::Item lib_;
    int sort_ = 0;
};

// --- Series → seasons -------------------------------------------------------------------------

class SeasonsScreen : public CoverScreen {
public:
    explicit SeasonsScreen(jelly::Item series)
        : CoverScreen(series.name, "Shows \xE2\x80\xBA " + series.name, options(series)), series_(std::move(series)) {
        load([id = series_.id] { return App::get().client->seasons(id); });
    }

private:
    static CoverPage::Options options(const jelly::Item& series) {
        CoverPage::Options o;
        o.viewKey = "seasons";
        o.defaultView = "grid";
        o.heroLabel = series.name;
        o.hints = [series](const jelly::Item* season) {
            std::vector<Hint> h;
            if (!season) return h;
            jelly::Item s = *season;
            h.push_back({glyph::A, "Open", KEY_A, [series, s] { App::get().push(makeEpisodesScreen(series, s)); }});
            h.push_back({glyph::Y, "Download", KEY_Y, [series, s] {
                             std::string sid = series.id, season = s.id;
                             App::get().push(makeMenu(s.name, {
                                                                  {"Download season", [sid, season] { downloadSeries(sid, season); }},
                                                                  {"Download series", [sid] { downloadSeries(sid); }},
                                                              }));
                         }});
            return h;
        };
        return o;
    }

    void onLoaded() override {
        // One season: skip straight to its episodes.
        if (page_.items().size() == 1) {
            auto season = page_.items().front();
            App::get().pop();
            App::get().push(makeEpisodesScreen(series_, season));
        }
    }

    jelly::Item series_;
};

// --- Season → episodes ------------------------------------------------------------------------

class EpisodesScreen : public CoverScreen {
public:
    EpisodesScreen(jelly::Item series, jelly::Item season)
        : CoverScreen(series.name + " \xC2\xB7 " + season.name, "Shows \xE2\x80\xBA " + series.name + " \xE2\x80\xBA " + season.name,
                      options(series, season)) {
        load([sid = series.id, season = season.id] { return App::get().client->episodes(sid, season); });
    }

private:
    static CoverPage::Options options(const jelly::Item& series, const jelly::Item& season) {
        CoverPage::Options o;
        o.viewKey = "episodes";
        o.defaultView = "shelf";
        o.heroLabel = series.name;
        std::string sid = series.id, seasonId = season.id;
        o.hints = [sid, seasonId](const jelly::Item* ep) {
            std::vector<Hint> h;
            if (!ep) return h;
            jelly::Item copy = *ep;
            bool resume = resumeTicks(copy.id, &copy.userData) > 0;
            bool played = isPlayed(copy.id, &copy.userData);
            h.push_back({glyph::A, resume ? "Resume" : "Play", KEY_A, [copy] { playItem(copy, false); }});
            h.push_back({glyph::Y, "More", KEY_Y, [copy, sid, seasonId] {
                             openItemMenu(copy, {{"Download season", [sid, seasonId] { downloadSeries(sid, seasonId); },
                                                  App::get().online()}});
                         }});
            h.push_back({glyph::X, played ? "Unwatch" : "Watched", KEY_X,
                         [copy, played] { toggleWatched(copy.id, !played, copy.runTimeTicks); }});
            return h;
        };
        return o;
    }
};

}  // namespace

std::unique_ptr<Screen> makeFolderScreen(const jelly::Item& folder) {
    if (folder.collectionType == "tvshows" || folder.collectionType == "movies")
        return std::make_unique<LibraryScreen>(folder);
    return makePlainFolderScreen(folder);
}

std::unique_ptr<Screen> makeSeriesScreen(const jelly::Item& series) { return std::make_unique<SeasonsScreen>(series); }

std::unique_ptr<Screen> makeEpisodesScreen(const jelly::Item& series, const jelly::Item& season) {
    return std::make_unique<EpisodesScreen>(series, season);
}

}  // namespace app
