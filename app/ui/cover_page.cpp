#include "cover_page.hpp"

#include "jelly/util.hpp"

#include <cctype>
#include <cmath>

namespace app {

namespace {

using jelly::TICKS_PER_SECOND;

bool isEpisode(const jelly::Item& i) { return i.type == "Episode"; }
bool isGroup(const jelly::Item& i) { return i.type == "Series" || i.type == "Season" || i.isFolder; }

std::string episodeCode(const jelly::Item& item) {
    return "S" + std::to_string(item.parentIndexNumber) + "E" + std::to_string(item.indexNumber);
}

std::string join(const std::vector<std::string>& parts) {
    std::string out;
    for (const auto& p : parts)
        if (!p.empty()) out += (out.empty() ? "" : " \xC2\xB7 ") + p;
    return out;
}

/// Big line: the show for an episode, the title otherwise.
std::string headline(const jelly::Item& item) {
    if (isEpisode(item)) return item.seriesName;
    return item.name;
}

/// Small line under the title.
std::string subline(const jelly::Item& item) {
    if (isEpisode(item)) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "S%02dE%02d", item.parentIndexNumber, item.indexNumber);
        return std::string(buf) + " \xC2\xB7 " + item.name;
    }
    std::string year = item.productionYear > 0 ? std::to_string(item.productionYear) : "";
    if (item.type == "Series")
        return join({year, item.childCount > 0 ? std::to_string(item.childCount) + (item.childCount == 1 ? " season" : " seasons") : ""});
    if (item.type == "Season")
        return join({item.childCount > 0 ? std::to_string(item.childCount) + " episodes" : "",
                     item.userData.unplayedItemCount > 0 ? std::to_string(item.userData.unplayedItemCount) + " unwatched" : ""});
    return join({year, runtimeText(item.runTimeTicks), item.officialRating});
}

float resumeFraction(const jelly::Item& item) {
    int64_t r = resumeTicks(item.id, &item.userData);
    return r > 0 && item.runTimeTicks > 0 ? static_cast<float>(r) / item.runTimeTicks : -1;
}

/// "12 min left", "Watched", "3 unwatched"...
std::string statusLine(const jelly::Item& item) {
    if (isGroup(item)) {
        int n = item.userData.unplayedItemCount;
        return n > 0 ? std::to_string(n) + " unwatched" : (item.userData.played ? "All watched" : "");
    }
    if (isPlayed(item.id, &item.userData)) return "Watched";
    int64_t r = resumeTicks(item.id, &item.userData);
    if (r <= 0 || item.runTimeTicks <= 0) return runtimeText(item.runTimeTicks);
    int64_t mins = (item.runTimeTicks - r) / TICKS_PER_SECOND / 60;
    return std::to_string(std::max<int64_t>(1, mins)) + " min left";
}

bool downloaded(const jelly::Item& item) {
    auto s = App::get().downloads->status(item.id);
    return s && *s == jelly::DownloadStatus::Completed;
}

/// Wide art: an episode's own still, else the show's / movie's backdrop.
ImageCache::Source wideFor(const jelly::Item& item, int w, int h) {
    return isEpisode(item) ? stillFor(item, w, h) : backdropFor(item, w, h);
}

void chip(Gfx& g, const std::string& text, float right, float y, u32 fill, u32 ink) {
    float w = g.textWidth(text, 0.32f) + 8;
    g.rect(right - w, y, w, 11, fill);
    g.text(text, right - w + 4, y + 1, 0.32f, ink);
}

constexpr u32 BG_A(u8 a) { return color::rgba(16, 18, 26, a); }

}  // namespace

CoverPage::CoverPage(Options options) : options_(std::move(options)) {
    std::string v = App::get().config.view(options_.viewKey, options_.defaultView);
    view_ = v == "shelf" ? View::Shelf : v == "hero" ? View::Hero : View::Grid;
}

const char* CoverPage::viewName(View v) {
    switch (v) {
    case View::Shelf: return "Shelf";
    case View::Hero: return "Hero";
    default: return "Grid";
    }
}

char CoverPage::letterOf(const jelly::Item& item) {
    for (char c : item.name)
        if (std::isalnum(static_cast<unsigned char>(c)))
            return std::isdigit(static_cast<unsigned char>(c)) ? '#' : static_cast<char>(std::toupper(c));
    return '#';
}

void CoverPage::setItems(std::vector<jelly::Item> items) {
    std::string keep = selected() ? selected()->id : "";
    items_ = std::move(items);
    selected_ = 0;
    for (size_t i = 0; i < items_.size(); ++i)
        if (items_[i].id == keep) selected_ = static_cast<int>(i);
    shelfPos_ = static_cast<float>(selected_);
    stripPos_ = static_cast<float>(std::max(0, selected_ - 5));
    rebuildGrid();
}

const jelly::Item* CoverPage::selected() const {
    if (selected_ < 0 || selected_ >= static_cast<int>(items_.size())) return nullptr;
    return &items_[selected_];
}

void CoverPage::select(int index) {
    if (items_.empty()) return;
    selected_ = std::max(0, std::min(static_cast<int>(items_.size()) - 1, index));
    grid_.selected = selected_;
}

void CoverPage::rebuildGrid() {
    std::vector<ListRow> rows;
    for (size_t i = 0; i < items_.size(); ++i) {
        const auto& it = items_[i];
        ListRow r;
        r.tag = static_cast<int>(i);
        r.title = headline(it);
        if (it.type == "Season" || (isEpisode(it) && options_.viewKey == "episodes")) r.title = it.name;
        bool played = isPlayed(it.id, &it.userData);
        if (isGroup(it) && it.userData.unplayedItemCount > 0) {
            r.badge = std::to_string(it.userData.unplayedItemCount);
            r.badgeColor = color::accent;
        } else if (played) {
            r.badge = "WATCHED";
            r.badgeColor = color::dim;
        } else if (isEpisode(it)) {
            r.badge = episodeCode(it);
            r.badgeColor = color::text;
        } else if (downloaded(it)) {
            r.badge = "SAVED";
            r.badgeColor = color::good;
        }
        r.progress = resumeFraction(it);
        r.image = coverFor(it, 64, 96);
        rows.push_back(std::move(r));
    }
    grid_.emptyText = emptyText_;
    grid_.setItems(std::move(rows));
    grid_.selected = selected_;
}

void CoverPage::cycleView() {
    view_ = view_ == View::Shelf ? View::Grid : view_ == View::Grid ? View::Hero : View::Shelf;
    auto& a = App::get();
    a.config.views[options_.viewKey] = view_ == View::Grid ? "grid" : view_ == View::Hero ? "hero" : "shelf";
    a.saveConfig();
}

void CoverPage::jumpLetter(int dir) {
    if (items_.empty()) return;
    char cur = letterOf(items_[selected_]);
    int i = selected_;
    if (dir > 0) {
        while (i < static_cast<int>(items_.size()) && letterOf(items_[i]) == cur) ++i;
        if (i >= static_cast<int>(items_.size())) return;
    } else {
        // Start of the current group; if already there, start of the previous one.
        while (i > 0 && letterOf(items_[i - 1]) == cur) --i;
        if (i == selected_ && i > 0) {
            char prev = letterOf(items_[i - 1]);
            --i;
            while (i > 0 && letterOf(items_[i - 1]) == prev) --i;
        }
    }
    select(i);
}

void CoverPage::update(const Input& in) {
    if (!loading_ && !items_.empty()) {
        if (view_ == View::Grid) {
            grid_.update(in);
            selected_ = grid_.selected;
        } else {
            if (in.repeat & KEY_RIGHT) select(selected_ + 1);
            if (in.repeat & KEY_LEFT) select(selected_ - 1);
        }
        if (options_.letterJump) {
            if (in.repeat & KEY_R) jumpLetter(1);
            if (in.repeat & KEY_L) jumpLetter(-1);
        }
    }
    hints_.hints = options_.hints ? options_.hints(selected()) : std::vector<Hint>{};
    hints_.hints.push_back({"SELECT", viewName(view_), KEY_SELECT, [this] { cycleView(); }});
    hints_.update(in);
}

// --- Top screen -------------------------------------------------------------------------------

void CoverPage::drawLetters(Gfx& g, float right, float y) {
    if (items_.empty()) return;
    char cur = letterOf(items_[selected_]);
    char prev = 0, next = 0;
    for (int i = selected_; i >= 0; --i)
        if (letterOf(items_[i]) != cur) {
            prev = letterOf(items_[i]);
            break;
        }
    for (int i = selected_; i < static_cast<int>(items_.size()); ++i)
        if (letterOf(items_[i]) != cur) {
            next = letterOf(items_[i]);
            break;
        }
    float x = right;
    x -= g.text(glyph::R, x, y, 0.45f, color::dim, Align::Right) + 4;
    if (next) x -= g.text(std::string(1, next), x, y + 1, 0.42f, color::dim, Align::Right) + 4;
    x -= g.text(std::string(1, cur), x, y - 2, 0.6f, color::text, Align::Right) + 4;
    if (prev) x -= g.text(std::string(1, prev), x, y + 1, 0.42f, color::dim, Align::Right) + 4;
    g.text(glyph::L, x, y, 0.45f, color::dim, Align::Right);
}

void CoverPage::drawTop(Gfx& g, float y, float h) {
    if (loading_) {
        drawLoading(g, 0, y, TOP_W, h);
        return;
    }
    if (items_.empty()) {
        g.text(emptyText_, TOP_W / 2.0f, y + h / 2 - 10, 0.5f, color::dim, Align::Center, TOP_W - 20);
        return;
    }
    switch (view_) {
    case View::Shelf: drawShelf(g, y, h); break;
    case View::Grid: grid_.draw(g, 0, y, TOP_W, h); break;
    case View::Hero: drawHero(g, y, h); break;
    }
}

void CoverPage::drawCard(Gfx& g, const jelly::Item& item, float cx, float top, float w, float h, float focus) {
    float x = cx - w / 2;
    if (focus > 0.5f) g.rect(x - 2, top - 2, w + 4, h + 4, color::accent);
    g.rect(x, top, w, h, color::panelHi);
    if (const Texture* t = App::get().images.get(wideFor(item, 192, 108))) g.imageCover(*t, x, top, w, h);
    if (focus < 1) g.rect(x, top, w, h, BG_A(static_cast<u8>((1 - focus) * 150)));
    float frac = resumeFraction(item);
    if (frac >= 0) g.progressBar(x, top + h - 3, w, 3, frac, color::accent2, color::shade);
    if (focus > 0.5f && downloaded(item)) chip(g, "SAVED", x + w - 4, top + 4, color::good, color::bg);
}

void CoverPage::drawShelf(Gfx& g, float y, float h) {
    shelfPos_ += (selected_ - shelfPos_) * 0.3f;
    if (std::fabs(selected_ - shelfPos_) < 0.01f) shelfPos_ = static_cast<float>(selected_);

    const float bigW = 176, smallW = 128;
    const float midY = y + 14 + 99 / 2.0f;
    std::vector<int> order;  // far cards first, so the focused one is drawn on top
    for (int i = std::max(0, selected_ - 3); i <= std::min(static_cast<int>(items_.size()) - 1, selected_ + 3); ++i)
        order.push_back(i);
    std::sort(order.begin(), order.end(),
              [&](int a, int b) { return std::fabs(a - shelfPos_) > std::fabs(b - shelfPos_); });
    for (int i : order) {
        float d = i - shelfPos_, ad = std::fabs(d);
        float focus = std::max(0.0f, 1 - ad);
        float w = smallW + (bigW - smallW) * focus, ch = w * 9 / 16;
        float off = ad <= 1 ? ad * 150 : 150 + (ad - 1) * 140;
        drawCard(g, items_[i], TOP_W / 2.0f + (d < 0 ? -off : off), midY - ch / 2, w, ch, focus);
    }

    const auto& item = items_[selected_];
    float ty = y + 14 + 99 + 10;
    g.text(headline(item), TOP_W / 2.0f, ty, 0.62f, color::text, Align::Center, TOP_W - 30);
    g.text(subline(item), TOP_W / 2.0f, ty + 21, 0.45f, color::dim, Align::Center, TOP_W - 30);
    g.text(statusLine(item), TOP_W / 2.0f, ty + 38, 0.42f, color::accent2, Align::Center);

    int n = static_cast<int>(items_.size());
    if (n > 1) {
        int shown = std::min(n, 9);
        int first = std::max(0, std::min(n - shown, selected_ - shown / 2));
        float total = (shown - 1) * 9 + 14;
        float x = (TOP_W - total) / 2;
        for (int i = first; i < first + shown; ++i) {
            bool sel = i == selected_;
            g.rect(x, y + h - 9, sel ? 14 : 5, 5, sel ? color::accent : color::panelHi);
            x += sel ? 18 : 9;
        }
    }
}

void CoverPage::drawHero(Gfx& g, float y, float h) {
    const auto& item = items_[selected_];
    g.rect(0, y, TOP_W, h, color::panelHi);
    if (const Texture* t = App::get().images.get(backdropFor(item, 384, 216))) g.imageCover(*t, 0, y, TOP_W, h);
    g.gradient(0, y, TOP_W, h, BG_A(240), BG_A(30), BG_A(240), BG_A(30));
    g.gradient(0, y + h - 84, TOP_W, 84, BG_A(0), BG_A(0), BG_A(255), BG_A(255));

    float x = 14, ty = y + 12;
    if (!options_.heroLabel.empty()) g.text(options_.heroLabel, x, ty, 0.38f, color::accent);
    g.text(headline(item), x, ty + 14, 0.8f, color::text, Align::Left, 240);
    g.text(subline(item), x, ty + 42, 0.45f, color::text, Align::Left, 240);
    if (!item.genres.empty()) {
        std::vector<std::string> g2(item.genres.begin(), item.genres.begin() + std::min<size_t>(3, item.genres.size()));
        g.text(join(g2), x, ty + 58, 0.38f, color::rgba(201, 207, 223), Align::Left, 240);
    }
    float sy = ty + (item.genres.empty() ? 62 : 76);
    float frac = resumeFraction(item);
    if (frac >= 0) g.progressBar(x, sy + 6, 120, 4, frac, color::accent2, color::rgba(255, 255, 255, 50));
    g.text(statusLine(item), x + (frac >= 0 ? 128 : 0), sy, 0.4f, color::accent2);
    if (downloaded(item)) chip(g, "SAVED", x + g.textWidth("SAVED", 0.32f) + 8, sy + 16, color::good, color::bg);

    float target = static_cast<float>(std::max(0, selected_ - 5));
    stripPos_ += (target - stripPos_) * 0.3f;
    const float step = 40, base = y + h - 8;
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        float px = 10 + (i - stripPos_) * step + (i > selected_ ? 8 : 0);
        if (px > TOP_W || px < -48) continue;
        bool sel = i == selected_;
        float pw = sel ? 40 : 32, ph = sel ? 60 : 48;
        if (sel) g.rect(px - 2, base - ph - 2, pw + 4, ph + 4, color::accent);
        g.rect(px, base - ph, pw, ph, color::panel);
        if (const Texture* t = App::get().images.get(coverFor(items_[i], 40, 60))) g.imageCover(*t, px, base - ph, pw, ph);
        float f2 = resumeFraction(items_[i]);
        if (f2 >= 0) g.progressBar(px, base - 2, pw, 2, f2, color::accent2, color::shade);
    }
}

// --- Bottom screen ----------------------------------------------------------------------------

void CoverPage::drawBottom(Gfx& g, float top) {
    const jelly::Item* item = loading_ ? nullptr : selected();
    if (!item) {
        if (!loading_) g.wrapped(emptyText_, 14, top + 40, 0.48f, color::dim, BOTTOM_W - 28, 5);
    } else if (item->type == "Movie") {
        drawPosterDetail(g, *item, top);
    } else {
        drawWideDetail(g, *item, top);
    }
    hints_.draw(g);
}

void CoverPage::drawWideDetail(Gfx& g, const jelly::Item& item, float top) {
    const float x = 10, y = top + 6, w = BOTTOM_W - 20, h = 110;
    g.rect(x, y, w, h, color::panelHi);
    if (const Texture* t = App::get().images.get(wideFor(item, 320, 180))) g.imageCover(*t, x, y, w, h);
    g.gradient(x, y + h - 52, w, 52, color::rgba(0, 0, 0, 0), color::rgba(0, 0, 0, 0), color::rgba(0, 0, 0, 210),
               color::rgba(0, 0, 0, 210));
    if (downloaded(item))
        chip(g, "SAVED", x + w - 5, y + 5, color::good, color::bg);
    else if (isGroup(item) && item.userData.unplayedItemCount > 0)
        chip(g, std::to_string(item.userData.unplayedItemCount) + " NEW", x + w - 5, y + 5, color::accent, color::text);

    std::string title, meta;
    if (isEpisode(item)) {
        title = subline(item);
        meta = join({item.seriesName, runtimeText(item.runTimeTicks), downloaded(item) ? "Downloaded" : ""});
    } else if (item.type == "Season") {
        title = join({item.seriesName, item.name});
        meta = subline(item);
    } else {
        title = item.name;
        meta = subline(item);
    }
    g.text(title, x + 8, y + h - 32, 0.5f, color::text, Align::Left, w - 16);
    g.text(meta, x + 8, y + h - 15, 0.38f, color::rgba(201, 207, 223), Align::Left, w - 16);

    float py = y + h + 9;
    std::string status = statusLine(item);
    float lw = g.textWidth(status, 0.38f);
    float frac = resumeFraction(item);
    if (!isGroup(item)) g.progressBar(x, py, w - lw - 10, 4, frac >= 0 ? frac : (isPlayed(item.id, &item.userData) ? 1 : 0), color::accent2);
    g.text(status, x + w, py - 5, 0.38f, color::accent2, Align::Right);
    g.wrapped(item.overview, x, py + 11, 0.4f, color::dim, w, 3);
}

void CoverPage::drawPosterDetail(Gfx& g, const jelly::Item& item, float top) {
    const float x = 10, y = top + 8, pw = 80, ph = 120;
    g.rect(x, y, pw, ph, color::panelHi);
    if (const Texture* t = App::get().images.get(coverFor(item, 80, 120))) g.imageCover(*t, x, y, pw, ph);
    float frac = resumeFraction(item);
    if (frac >= 0) g.progressBar(x, y + ph - 3, pw, 3, frac, color::accent2, color::shade);

    float tx = x + pw + 10, tw = BOTTOM_W - tx - 10, ty = y;
    g.text(item.name, tx, ty, 0.55f, color::text, Align::Left, tw);
    ty += 20;
    g.text(subline(item), tx, ty, 0.4f, color::dim, Align::Left, tw);
    ty += 14;
    if (!item.genres.empty()) {
        std::vector<std::string> g2(item.genres.begin(), item.genres.begin() + std::min<size_t>(3, item.genres.size()));
        g.text(join(g2), tx, ty, 0.4f, color::dim, Align::Left, tw);
        ty += 14;
    }
    if (auto d = App::get().downloads->find(item.id); d && d->status == jelly::DownloadStatus::Completed) {
        chip(g, "SAVED", tx + g.textWidth("SAVED", 0.32f) + 8, ty + 2, color::good, color::bg);
        g.text(jelly::formatSize(d->bytes) + " on SD", tx + g.textWidth("SAVED", 0.32f) + 14, ty + 1, 0.36f, color::dim);
        ty += 16;
    }
    g.text(statusLine(item), tx, ty, 0.4f, color::accent2, Align::Left, tw);
    ty += 16;
    g.wrapped(item.overview, tx, ty, 0.38f, color::dim, tw, std::max(1, static_cast<int>((y + ph + 40 - ty) / 12)));
}

}  // namespace app
