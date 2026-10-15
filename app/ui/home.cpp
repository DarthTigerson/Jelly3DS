#include "cover_page.hpp"
#include "downloads_page.hpp"
#include "screens.hpp"

#include "jelly/util.hpp"

#include <map>

namespace app {

namespace {

using jelly::DownloadStatus;

enum Tab { DOWNLOADS = 0, CONTINUE, LIBRARIES, SETTINGS, TAB_COUNT };
const char* TAB_NAMES[] = {"Downloads", "Continue", "Libraries", "Settings"};
constexpr float TAB_H = 28;
constexpr float SHOULDER_W = 22;  // L / R markers at the ends of the tab bar

class HomeScreen : public Screen {
public:
    HomeScreen() {
        auto& a = App::get();
        bool haveDownloads = !a.downloads->items().empty();
        tab_ = haveDownloads ? DOWNLOADS : CONTINUE;
        list_.thumbW = 28;
        grid_.columns = 2;  // library art is wide 16:9
        grid_.posterW = 176;
        grid_.posterH = 99;
        reload();
    }

    void onResume() override { reload(); }

    void update(const Input& in) override {
        auto& a = App::get();
        (void)a;
        int newTab = tab_;
        if (in.down & KEY_R) newTab = (tab_ + 1) % TAB_COUNT;
        if (in.down & KEY_L) newTab = (tab_ + TAB_COUNT - 1) % TAB_COUNT;
        if (in.tapped && in.tapY < TAB_H) {
            if (in.tapX < SHOULDER_W)
                newTab = (tab_ + TAB_COUNT - 1) % TAB_COUNT;
            else if (in.tapX >= BOTTOM_W - SHOULDER_W)
                newTab = (tab_ + 1) % TAB_COUNT;
            else
                newTab = std::min(TAB_COUNT - 1, static_cast<int>((in.tapX - SHOULDER_W) * TAB_COUNT / (BOTTOM_W - 2 * SHOULDER_W)));
        }
        if (newTab != tab_) {
            tab_ = static_cast<Tab>(newTab);
            list_.selected = 0;
            grid_.selected = 0;
            reload();
            return;
        }

        if (tab_ == DOWNLOADS) {
            if (++frames_ % 20 == 0) downloadsPage_.refresh();  // live progress
            downloadsPage_.update(in);
            return;
        }
        if (tab_ == CONTINUE) {
            continue_.update(in);
            if (++frames_ % 30 == 0) continue_.refreshBadges();  // download progress
            return;
        }
        if (tab_ == LIBRARIES) {
            updateLibraries(in);
            return;
        }
        list_.update(in, LIST_H);
        buildButtons();
        if (buttons_.update(in)) return;

        // Downloads progress changes under us: refresh twice a second.
        if (tab_ == DOWNLOADS && ++frames_ % 30 == 0) buildDownloadRows();
        if (tab_ == SETTINGS && frames_++ % 60 == 0) buildSettingsRows();
    }

    void drawTop(Gfx& g) override {
        auto& a = App::get();
        std::string status = a.online() ? a.config.userName : (a.wifiConnected() ? "Server unreachable" : "Offline");
        if (tab_ == DOWNLOADS) status = downloadsPage_.headerText();
        drawHeader(g, std::string("Jelly3DS \xC2\xB7 ") + TAB_NAMES[tab_], status);
        if (tab_ == DOWNLOADS)
            downloadsPage_.drawTop(g, 28, LIST_H);
        else if (tab_ == CONTINUE)
            continue_.drawTop(g, 28, LIST_H);
        else if (loading_)
            drawLoading(g, 0, 28, TOP_W, LIST_H);
        else if (tab_ == LIBRARIES)
            grid_.draw(g, 0, 28, TOP_W, LIST_H);
        else
            list_.draw(g, 0, 28, TOP_W, LIST_H);
    }

    void drawBottom(Gfx& g) override {
        // Tabs, with the shoulder buttons that switch them at either end
        g.rect(0, 0, BOTTOM_W, TAB_H, color::panel);
        g.text(glyph::L, SHOULDER_W / 2, 6, 0.5f, color::dim, Align::Center);
        g.text(glyph::R, BOTTOM_W - SHOULDER_W / 2, 6, 0.5f, color::dim, Align::Center);
        float w = (BOTTOM_W - 2 * SHOULDER_W) / TAB_COUNT;
        for (int i = 0; i < TAB_COUNT; ++i) {
            bool sel = i == tab_;
            float tx = SHOULDER_W + i * w;
            if (sel) g.rect(tx, TAB_H - 3, w, 3, color::accent);
            g.text(TAB_NAMES[i], tx + w / 2, 7, 0.44f, sel ? color::text : color::dim, Align::Center, w - 2);
        }
        if (tab_ == CONTINUE) {
            continue_.drawBottom(g, TAB_H);
            return;
        }
        if (tab_ == DOWNLOADS) {
            downloadsPage_.drawBottom(g, TAB_H);
            return;
        }
        if (tab_ == LIBRARIES) {
            drawLibraryDetail(g, TAB_H);
            return;
        }

        const ListRow* row = currentRow();
        float top = TAB_H + 2, bottom = SCREEN_H - ButtonBar::HEIGHT;
        if (tab_ == DOWNLOADS && row && row->tag >= 0 && row->tag < static_cast<int>(downloads_.size())) {
            const auto& d = downloads_[row->tag];
            drawDetail(g, detail_.get(tab_ * 100000 + row->tag, [&] { return detailFor(d); }), top, bottom);
        } else if ((tab_ == CONTINUE || tab_ == LIBRARIES) && row && row->tag >= 0 && row->tag < static_cast<int>(items_.size())) {
            const auto& item = items_[row->tag];
            drawDetail(g, detail_.get(tab_ * 100000 + row->tag, [&] { return detailFor(item); }), top, bottom);
        } else if (tab_ == SETTINGS) {
            drawSettingsHelp(g, top);
        } else if (!message_.empty()) {
            g.wrapped(message_, 12, top + 30, 0.5f, color::dim, BOTTOM_W - 24, 6);
        }
        buttons_.draw(g);
    }

private:
    static constexpr float LIST_H = SCREEN_H - 28;

    static CoverPage::Options continueOptions() {
        CoverPage::Options o;
        o.viewKey = "continue";
        o.defaultView = "shelf";
        o.heroLabel = "CONTINUE WATCHING";
        o.hints = [](const jelly::Item* item) {
            std::vector<Hint> h;
            if (!item) return h;
            jelly::Item copy = *item;
            bool resume = resumeTicks(copy.id, &copy.userData) > 0;
            bool played = isPlayed(copy.id, &copy.userData);
            h.push_back({glyph::A, resume ? "Resume" : "Play", KEY_A, [copy] { playItem(copy, false); }});
            h.push_back({glyph::Y, "More", KEY_Y, [copy] { openItemMenu(copy); }});
            h.push_back({glyph::X, played ? "Unwatch" : "Watched", KEY_X,
                         [copy, played] { toggleWatched(copy.id, !played, copy.runTimeTicks); }});
            return h;
        };
        return o;
    }

    // --- Libraries tab: wide 16:9 cards (library art is wide) + recently added ------------------

    void openLibrary(const jelly::Item& lib) { App::get().push(makeFolderScreen(lib)); }

    void updateLibraries(const Input& in) {
        grid_.update(in);
        libraryHints_.hints.clear();
        const ListRow* row = grid_.current();
        if (row && row->tag >= 0 && row->tag < static_cast<int>(items_.size())) {
            jelly::Item lib = items_[row->tag];
            libraryHints_.hints.push_back({glyph::A, "Open", KEY_A, [this, lib] { openLibrary(lib); }});
            requestLatest(lib.id);
        }
        libraryHints_.update(in);
    }

    void requestLatest(const std::string& id) {
        if (latest_.count(id) || latestLoading_[id]) return;
        latestLoading_[id] = true;
        loadCachedThenFresh<std::vector<jelly::Item>>(
            [id](jelly::Source src) { return App::get().client->latest(id, 5, src); },
            [this, id](jelly::Result<std::vector<jelly::Item>> r, bool fresh) {
                if (fresh) latestLoading_[id] = false;
                if (r.ok || !latest_.count(id)) latest_[id] = r.ok ? std::move(r.value) : std::vector<jelly::Item>{};
            });
    }

    void drawLibraryDetail(Gfx& g, float top) {
        const ListRow* row = grid_.current();
        if (row && row->tag >= 0 && row->tag < static_cast<int>(items_.size())) {
            const auto& lib = items_[row->tag];
            float x = 12, y = top + 10;
            g.text(lib.name, x, y, 0.7f, color::text, Align::Left, BOTTOM_W - 24);
            std::string kind = lib.collectionType == "tvshows" ? "TV shows" : lib.collectionType == "movies" ? "Movies" : "Library";
            g.text(kind, x, y + 24, 0.42f, color::dim);
            g.text("RECENTLY ADDED", x, y + 46, 0.38f, color::accent);
            auto it = latest_.find(lib.id);
            if (it == latest_.end()) {
                g.text(App::get().online() ? "Loading\xE2\x80\xA6" : "Offline", x, y + 64, 0.4f, color::dim);
            } else if (it->second.empty()) {
                g.text("Nothing new", x, y + 64, 0.4f, color::dim);
            } else {
                float px = x;
                for (const auto& item : it->second) {
                    g.rect(px, y + 62, 50, 75, color::panelHi);
                    if (const Texture* t = App::get().images.get(coverFor(item, 50, 75))) g.imageCover(*t, px, y + 62, 50, 75);
                    g.text(item.type == "Episode" ? item.seriesName : item.name, px, y + 140, 0.32f, color::dim,
                           Align::Left, 50);
                    px += 58;
                }
            }
        }
        libraryHints_.draw(g);
    }

    const ListRow* currentRow() const { return tab_ == LIBRARIES ? grid_.current() : list_.current(); }

    void reload() {
        message_.clear();
        detail_.invalidate();
        list_.thumbW = tab_ == SETTINGS ? 0 : 64;
        list_.rowHeight = tab_ == SETTINGS ? 40 : 46;
        switch (tab_) {
        case DOWNLOADS: downloadsPage_.refresh(); break;
        case CONTINUE: loadContinue(); break;
        case LIBRARIES: loadLibraries(); break;
        case SETTINGS: buildSettingsRows(); break;
        default: break;
        }
        buildButtons();
    }

    // --- Downloads ----------------------------------------------------------------------------

    void buildDownloadRows() {
        auto& a = App::get();
        downloads_ = a.downloads->items();
        // Group by series (movies last), episodes in order.
        std::vector<int> order(downloads_.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
        std::sort(order.begin(), order.end(), [&](int x, int y) {
            const auto& p = downloads_[x];
            const auto& q = downloads_[y];
            bool pm = p.seriesName.empty(), qm = q.seriesName.empty();
            if (pm != qm) return qm;
            if (p.seriesName != q.seriesName) return p.seriesName < q.seriesName;
            if (p.seasonIndex != q.seasonIndex) return p.seasonIndex < q.seasonIndex;
            if (p.episodeIndex != q.episodeIndex) return p.episodeIndex < q.episodeIndex;
            return p.name < q.name;
        });
        std::vector<ListRow> rows;
        std::string group = "\x01";
        for (int i : order) {
            const auto& d = downloads_[i];
            std::string g = d.seriesName.empty() ? "Movies & videos" : d.seriesName;
            if (g != group) {
                ListRow h;
                h.title = g;
                h.header = true;
                rows.push_back(h);
                group = g;
            }
            ListRow r;
            r.tag = i;
            r.title = d.type == "Episode" ? d.label() : d.label();
            if (d.type == "Episode") {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "S%02dE%02d", d.seasonIndex, d.episodeIndex);
                r.title = std::string(buf) + " \xC2\xB7 " + d.name;
            }
            auto badge = downloadBadge(d.itemId);
            r.badge = badge.text;
            r.badgeColor = badge.color;
            auto det = detailFor(d);
            r.subtitle = det.status;
            r.progress = d.status == DownloadStatus::Downloading ? badge.progress : det.progress;
            if (d.status == DownloadStatus::Completed && isPlayed(d.itemId, nullptr)) {
                r.badge = "WATCHED";
                r.badgeColor = color::dim;
            }
            r.image = thumbFor(d, 64, 36);
            rows.push_back(std::move(r));
        }
        list_.emptyText = "No downloads yet - find an episode and choose Download";
        list_.setRows(std::move(rows));
        loading_ = false;
    }

    // --- Continue / Libraries -----------------------------------------------------------------

    void loadContinue() {
        continue_.setLoading(true);
        loadCachedThenFresh<std::vector<jelly::Item>>(
            [](jelly::Source src) {
                auto& c = *App::get().client;
                auto resume = c.resumeItems(src);
                auto next = c.nextUp(src);
                if (!resume.ok && !next.ok) return jelly::Result<std::vector<jelly::Item>>::failure(resume.error);
                std::vector<jelly::Item> out = resume.ok ? resume.value : std::vector<jelly::Item>{};
                if (next.ok)
                    for (auto& i : next.value)
                        if (std::none_of(out.begin(), out.end(), [&](const jelly::Item& x) { return x.id == i.id; }))
                            out.push_back(i);
                return jelly::Result<std::vector<jelly::Item>>::success(std::move(out));
            },
            [this](jelly::Result<std::vector<jelly::Item>> r, bool) {
                continue_.setLoading(false);
                continue_.setEmptyText(r.ok ? "Nothing in progress - start an episode and it shows up here"
                                            : "Offline - your downloads are in the Downloads tab");
                continue_.setItems(r.ok ? std::move(r.value) : std::vector<jelly::Item>{});
            });
    }

    void loadLibraries() {
        loading_ = items_.empty();
        loadCachedThenFresh<std::vector<jelly::Item>>(
            [](jelly::Source src) { return App::get().client->views(src); },
            [this](jelly::Result<std::vector<jelly::Item>> r, bool) {
                loading_ = false;
                if (tab_ != LIBRARIES) return;
                if (!r.ok) {
                    grid_.emptyText = r.error;
                    return;
                }
                items_ = std::move(r.value);
                std::vector<ListRow> rows;
                for (size_t i = 0; i < items_.size(); ++i) rows.push_back(itemRow(items_[i], static_cast<int>(i)));
                grid_.emptyText = "No libraries";
                grid_.setItems(std::move(rows));
            });
    }

    ListRow itemRow(const jelly::Item& item, int tag) {
        ListRow r;
        r.tag = tag;
        if (item.type == "Episode") {
            r.title = item.seriesName;
            char buf[16];
            std::snprintf(buf, sizeof(buf), "S%02dE%02d", item.parentIndexNumber, item.indexNumber);
            r.subtitle = std::string(buf) + " \xC2\xB7 " + item.name;
        } else {
            r.title = item.label();
            r.subtitle = item.type == "CollectionFolder" ? item.collectionType : runtimeText(item.runTimeTicks);
        }
        auto badge = downloadBadge(item.id);
        r.badge = badge.text;
        r.badgeColor = badge.color;
        int64_t resume = resumeTicks(item.id, &item.userData);
        if (resume > 0 && item.runTimeTicks > 0) r.progress = static_cast<float>(resume) / item.runTimeTicks;
        r.image = tab_ == LIBRARIES ? posterFor(item, 176, 99) : posterFor(item, 64, 36);
        if (tab_ == LIBRARIES) r.title = item.name;
        return r;
    }

    // --- Settings -----------------------------------------------------------------------------

    enum SettingRow { S_PROFILE, S_VOLUME, S_AUDIO, S_SUBS, S_TLS, S_SYNC, S_SERVER, S_STORAGE, S_LOGOUT, S_EXIT };

    void buildSettingsRows() {
        auto& a = App::get();
        auto& c = a.config;
        std::string profile = c.profile == jelly::ProfileChoice::Auto ? "Automatic: " + a.profileLabel() : a.profileLabel();
        int dirty = static_cast<int>(a.progress->dirtyEntries().size());
        int64_t free = sdFreeBytes();
        std::vector<ListRow> rows = {
            {"Video quality for downloads & streams", profile, "", color::dim, -1, false, {}, S_PROFILE},
            {"Volume boost", c.volumeBoost == 1 ? "Off" : std::to_string(c.volumeBoost) + "\xC3\x97 louder (Up/Down while watching)", "", color::dim, -1, false, {}, S_VOLUME},
            {"Preferred audio language", c.audioLanguage.empty() ? "Server default" : c.audioLanguage, "", color::dim, -1, false, {}, S_AUDIO},
            {"Burn-in subtitles", c.subtitleLanguage.empty() ? "None" : c.subtitleLanguage, "", color::dim, -1, false, {}, S_SUBS},
            {"Verify HTTPS certificates", c.verifyTls ? "On" : "Off (allows self-signed)", "", color::dim, -1, false, {}, S_TLS},
            {"Sync watch progress now", dirty ? std::to_string(dirty) + " change(s) waiting" : "Everything is synced", dirty ? "PENDING" : "",
             color::warn, -1, false, {}, S_SYNC},
            {"Server", c.serverUrl + "  (" + c.userName + ")", a.online() ? "ONLINE" : "OFFLINE",
             a.online() ? color::good : color::bad, -1, false, {}, S_SERVER},
            {"SD card free space", free >= 0 ? jelly::formatSize(free) : "unknown", "", color::dim, -1, false, {}, S_STORAGE},
            {"Log out", "Downloads stay on the SD card", "", color::dim, -1, false, {}, S_LOGOUT},
            {"Exit Jelly3DS", "Back to the HOME Menu / Homebrew Launcher", "", color::dim, -1, false, {}, S_EXIT},
        };
        list_.setRows(std::move(rows));
        loading_ = false;
    }

    void activateSetting(int which) {
        auto& a = App::get();
        auto& c = a.config;
        switch (which) {
        case S_PROFILE:
            c.profile = c.profile == jelly::ProfileChoice::Auto     ? jelly::ProfileChoice::New3DS
                        : c.profile == jelly::ProfileChoice::New3DS ? jelly::ProfileChoice::Old3DS
                                                                    : jelly::ProfileChoice::Auto;
            a.saveConfig();
            break;
        case S_VOLUME:
            c.volumeBoost = c.volumeBoost % jelly::MAX_VOLUME_BOOST + 1;
            a.saveConfig();
            break;
        case S_EXIT:
            a.quit();
            return;
        case S_AUDIO: {
            std::string v = c.audioLanguage;
            if (keyboard("3-letter language code (eng, jpn, fre...) - empty for default", v, false, 3)) {
                c.audioLanguage = v;
                a.saveConfig();
            }
            break;
        }
        case S_SUBS: {
            std::string v = c.subtitleLanguage;
            if (keyboard("3-letter subtitle language (eng...) - empty for none", v, false, 3)) {
                c.subtitleLanguage = v;
                a.saveConfig();
            }
            break;
        }
        case S_TLS:
            c.verifyTls = !c.verifyTls;
            a.saveConfig();
            break;
        case S_SYNC:
        case S_SERVER:
            a.toast("Contacting server\xE2\x80\xA6");
            a.checkOnline(true);
            break;
        case S_LOGOUT:
            a.push(makeConfirm("Log out of " + c.serverUrl + "?", "Log out", [] { App::get().logout(); }));
            break;
        default: break;
        }
        buildSettingsRows();
    }

    void drawSettingsHelp(Gfx& g, float top) {
        auto& a = App::get();
        float y = top + 10;
        g.text("This console", 12, y, 0.5f, color::accent2);
        y += 20;
        g.text(std::string(a.isNew3ds ? "New Nintendo 3DS family" : "Nintendo 3DS family (original)"), 12, y, 0.46f, color::text);
        y += 18;
        g.text("Converted by the server to " + a.profileLabel(), 12, y, 0.42f, color::dim, Align::Left, BOTTOM_W - 24);
        y += 26;
        g.wrapped("Downloads keep playing at the office with no connection. Watch progress is saved on the SD card and sent to "
                  "Jellyfin automatically next time your server is reachable.",
                  12, y, 0.42f, color::dim, BOTTOM_W - 24, 6);
    }

    // --- Buttons ------------------------------------------------------------------------------

    void buildButtons() {
        auto& a = App::get();
        buttons_.buttons.clear();
        const ListRow* row = currentRow();
        switch (tab_) {
        case DOWNLOADS:
            if (row && row->tag >= 0 && row->tag < static_cast<int>(downloads_.size())) {
                auto d = downloads_[row->tag];
                bool done = d.status == DownloadStatus::Completed;
                buttons_.buttons.push_back({std::string(glyph::A) + (done ? " Play" : " Options"), KEY_A,
                                            [d, done] { done ? playDownload(d, false) : openDownloadMenu(d); }});
                buttons_.buttons.push_back({std::string(glyph::Y) + " More", KEY_Y, [d] { openDownloadMenu(d); }});
                buttons_.buttons.push_back({std::string(glyph::X) + " Delete", KEY_X, [d] {
                                                std::string id = d.itemId;
                                                App::get().push(makeConfirm("Delete " + d.label() + "?", "Delete", [id] {
                                                    App::get().downloads->remove(id);
                                                }));
                                            }});
            }
            break;
        case CONTINUE:
        case LIBRARIES:
            if (row && row->tag >= 0 && row->tag < static_cast<int>(items_.size())) {
                auto item = items_[row->tag];
                buttons_.buttons.push_back({std::string(glyph::A) + " Open", KEY_A, [item] {
                                                if (item.isPlayable())
                                                    openItemMenu(item);
                                                else if (item.type == "Series")
                                                    App::get().push(makeSeriesScreen(item));
                                                else
                                                    App::get().push(makeFolderScreen(item));
                                            }});
            }
            buttons_.buttons.push_back({std::string(glyph::Y) + " Refresh", KEY_Y, [this] { App::get().checkOnline(false); reload(); }});
            break;
        case SETTINGS:
            if (row) {
                int which = row->tag;
                buttons_.buttons.push_back({std::string(glyph::A) + " Change", KEY_A, [this, which] { activateSetting(which); }});
            }
            break;
        default: break;
        }
        (void)a;
    }

    Tab tab_ = DOWNLOADS;
    ListView list_;
    GridView grid_;
    CoverPage continue_{continueOptions()};
    DownloadsPage downloadsPage_;
    HintStrip libraryHints_;
    std::map<std::string, std::vector<jelly::Item>> latest_;  // library id → recently added
    std::map<std::string, bool> latestLoading_;
    ButtonBar buttons_;
    DetailCache detail_;
    std::vector<jelly::DownloadItem> downloads_;
    std::vector<jelly::Item> items_;
    std::string message_;
    bool loading_ = false;
    int frames_ = 0;
};

}  // namespace

std::unique_ptr<Screen> makeHomeScreen() { return std::make_unique<HomeScreen>(); }

}  // namespace app
