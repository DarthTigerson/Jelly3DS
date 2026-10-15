#include "continue_page.hpp"

#include "jelly/util.hpp"

#include <cmath>

namespace app {

namespace {

using jelly::TICKS_PER_SECOND;

std::string episodeCode(const jelly::Item& item) {
    if (item.type != "Episode") return "";
    return "S" + std::to_string(item.parentIndexNumber) + "E" + std::to_string(item.indexNumber);
}

/// Big line: the show for an episode, the title otherwise.
std::string headline(const jelly::Item& item) { return item.type == "Episode" ? item.seriesName : item.name; }

/// Small line: "S01E03 · Episode title" or "2023 · 1h 52m".
std::string subline(const jelly::Item& item) {
    if (item.type == "Episode") {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "S%02dE%02d", item.parentIndexNumber, item.indexNumber);
        return std::string(buf) + " \xC2\xB7 " + item.name;
    }
    std::string s = item.productionYear > 0 ? std::to_string(item.productionYear) : "";
    std::string rt = runtimeText(item.runTimeTicks);
    if (!rt.empty()) s += (s.empty() ? "" : " \xC2\xB7 ") + rt;
    return s;
}

float resumeFraction(const jelly::Item& item) {
    int64_t r = resumeTicks(item.id, &item.userData);
    return r > 0 && item.runTimeTicks > 0 ? static_cast<float>(r) / item.runTimeTicks : -1;
}

std::string timeLeft(const jelly::Item& item) {
    int64_t r = resumeTicks(item.id, &item.userData);
    if (r <= 0 || item.runTimeTicks <= 0) return runtimeText(item.runTimeTicks);
    int64_t mins = (item.runTimeTicks - r) / TICKS_PER_SECOND / 60;
    return std::to_string(std::max<int64_t>(1, mins)) + " min left";
}

bool downloaded(const jelly::Item& item) {
    auto s = App::get().downloads->status(item.id);
    return s && *s == jelly::DownloadStatus::Completed;
}

void savedChip(Gfx& g, float right, float y) {
    float w = g.textWidth("SAVED", 0.32f) + 8;
    g.rect(right - w, y, w, 11, color::good);
    g.text("SAVED", right - w + 4, y + 1, 0.32f, color::bg);
}

constexpr u32 BG_A(u8 a) { return color::rgba(16, 18, 26, a); }

}  // namespace

ContinuePage::ContinuePage() {
    const auto& v = App::get().config.continueView;
    view_ = v == "grid" ? View::Grid : v == "hero" ? View::Hero : View::Shelf;
    grid_.emptyText = emptyText_;
}

const char* ContinuePage::viewName(View v) {
    switch (v) {
    case View::Grid: return "Grid";
    case View::Hero: return "Hero";
    default: return "Shelf";
    }
}

void ContinuePage::setItems(std::vector<jelly::Item> items) {
    std::string keep = selected() ? selected()->id : "";
    items_ = std::move(items);
    selected_ = 0;
    for (size_t i = 0; i < items_.size(); ++i)
        if (items_[i].id == keep) selected_ = static_cast<int>(i);
    shelfPos_ = stripPos_ = static_cast<float>(selected_);
    rebuildGrid();
}

const jelly::Item* ContinuePage::selected() const {
    if (selected_ < 0 || selected_ >= static_cast<int>(items_.size())) return nullptr;
    return &items_[selected_];
}

void ContinuePage::select(int index) {
    if (items_.empty()) return;
    selected_ = std::max(0, std::min(static_cast<int>(items_.size()) - 1, index));
    grid_.selected = selected_;
}

void ContinuePage::rebuildGrid() {
    std::vector<ListRow> rows;
    for (size_t i = 0; i < items_.size(); ++i) {
        const auto& it = items_[i];
        ListRow r;
        r.tag = static_cast<int>(i);
        r.title = headline(it);
        r.badge = episodeCode(it);
        r.badgeColor = color::text;
        r.progress = resumeFraction(it);
        r.image = coverFor(it, 64, 96);
        rows.push_back(std::move(r));
    }
    grid_.emptyText = emptyText_;
    grid_.setItems(std::move(rows));
    grid_.selected = selected_;
}

void ContinuePage::cycleView() {
    view_ = view_ == View::Shelf ? View::Grid : view_ == View::Grid ? View::Hero : View::Shelf;
    auto& a = App::get();
    a.config.continueView = view_ == View::Grid ? "grid" : view_ == View::Hero ? "hero" : "shelf";
    a.saveConfig();
}

void ContinuePage::buildHints() {
    hints_.hints.clear();
    if (const jelly::Item* item = selected()) {
        jelly::Item copy = *item;
        bool resume = resumeTicks(copy.id, &copy.userData) > 0;
        bool played = isPlayed(copy.id, &copy.userData);
        hints_.hints.push_back({glyph::A, resume ? "Resume" : "Play", KEY_A, [copy] { playItem(copy, false); }});
        hints_.hints.push_back({glyph::Y, "More", KEY_Y, [copy] { openItemMenu(copy); }});
        hints_.hints.push_back({glyph::X, played ? "Unwatch" : "Watched", KEY_X, [this, copy, played] {
                                    toggleWatched(copy.id, !played, copy.runTimeTicks);
                                    rebuildGrid();
                                }});
    }
    hints_.hints.push_back({"SELECT", viewName(view_), KEY_SELECT, [this] { cycleView(); }});
}

void ContinuePage::update(const Input& in) {
    if (!loading_ && !items_.empty()) {
        if (view_ == View::Grid) {
            grid_.update(in);
            selected_ = grid_.selected;
        } else {
            if (in.repeat & KEY_RIGHT) select(selected_ + 1);
            if (in.repeat & KEY_LEFT) select(selected_ - 1);
        }
    }
    buildHints();
    hints_.update(in);
}

// --- Top screen -------------------------------------------------------------------------------

void ContinuePage::drawTop(Gfx& g, float y, float h) {
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

void ContinuePage::drawCard(Gfx& g, const jelly::Item& item, float cx, float top, float w, float h, float focus) {
    float x = cx - w / 2;
    if (focus > 0.5f) g.rect(x - 2, top - 2, w + 4, h + 4, color::accent);
    g.rect(x, top, w, h, color::panelHi);
    if (const Texture* t = App::get().images.get(stillFor(item, 192, 108))) g.imageCover(*t, x, top, w, h);
    if (focus < 1) g.rect(x, top, w, h, BG_A(static_cast<u8>((1 - focus) * 150)));
    float frac = resumeFraction(item);
    if (frac >= 0) g.progressBar(x, top + h - 3, w, 3, frac, color::accent2, color::shade);
    if (focus > 0.5f && downloaded(item)) savedChip(g, x + w - 4, top + 4);
}

void ContinuePage::drawShelf(Gfx& g, float y, float h) {
    shelfPos_ += (selected_ - shelfPos_) * 0.3f;
    if (std::fabs(selected_ - shelfPos_) < 0.01f) shelfPos_ = static_cast<float>(selected_);

    const float bigW = 176, smallW = 128;
    const float midY = y + 14 + 99 / 2.0f;
    // Far cards first so the focused one is drawn on top.
    std::vector<int> order;
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
    g.text(timeLeft(item), TOP_W / 2.0f, ty + 38, 0.42f, color::accent2, Align::Center);

    // Position dots (a sliding window of up to 9)
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

void ContinuePage::drawHero(Gfx& g, float y, float h) {
    const auto& item = items_[selected_];
    g.rect(0, y, TOP_W, h, color::panelHi);
    if (const Texture* t = App::get().images.get(backdropFor(item, 384, 216))) g.imageCover(*t, 0, y, TOP_W, h);
    // Scrims keep the text readable on any artwork.
    g.gradient(0, y, TOP_W, h, BG_A(240), BG_A(30), BG_A(240), BG_A(30));
    g.gradient(0, y + h - 84, TOP_W, 84, BG_A(0), BG_A(0), BG_A(255), BG_A(255));

    float x = 14, ty = y + 12;
    g.text("CONTINUE WATCHING", x, ty, 0.38f, color::accent);
    g.text(headline(item), x, ty + 14, 0.8f, color::text, Align::Left, 240);
    g.text(subline(item), x, ty + 42, 0.45f, color::text, Align::Left, 240);
    float frac = resumeFraction(item);
    if (frac >= 0) g.progressBar(x, ty + 66, 120, 4, frac, color::accent2, color::rgba(255, 255, 255, 50));
    g.text(timeLeft(item), x + (frac >= 0 ? 128 : 0), ty + 60, 0.4f, color::accent2);
    if (downloaded(item)) savedChip(g, x + g.textWidth("SAVED", 0.32f) + 8, ty + 78);

    // Strip of covers along the bottom; the selected one is bigger.
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
        float frac2 = resumeFraction(items_[i]);
        if (frac2 >= 0) g.progressBar(px, base - 2, pw, 2, frac2, color::accent2, color::shade);
    }
}

// --- Bottom screen ----------------------------------------------------------------------------

void ContinuePage::drawBottom(Gfx& g, float top) {
    const jelly::Item* item = loading_ ? nullptr : selected();
    if (!item) {
        if (!loading_) g.wrapped(emptyText_, 14, top + 40, 0.48f, color::dim, BOTTOM_W - 28, 5);
        hints_.draw(g);
        return;
    }
    const float x = 10, y = top + 6, w = BOTTOM_W - 20, h = 110;
    g.rect(x, y, w, h, color::panelHi);
    if (const Texture* t = App::get().images.get(stillFor(*item, 320, 180))) g.imageCover(*t, x, y, w, h);
    g.gradient(x, y + h - 52, w, 52, color::rgba(0, 0, 0, 0), color::rgba(0, 0, 0, 0), color::rgba(0, 0, 0, 210),
               color::rgba(0, 0, 0, 210));
    if (downloaded(*item)) savedChip(g, x + w - 5, y + 5);

    std::string title = item->type == "Episode" ? subline(*item) : item->name;
    std::string meta = item->type == "Episode" ? item->seriesName : subline(*item);
    std::string rt = runtimeText(item->runTimeTicks);
    if (item->type == "Episode" && !rt.empty()) meta += " \xC2\xB7 " + rt;
    if (downloaded(*item)) meta += " \xC2\xB7 Downloaded";
    g.text(title, x + 8, y + h - 32, 0.5f, color::text, Align::Left, w - 16);
    g.text(meta, x + 8, y + h - 15, 0.38f, color::rgba(201, 207, 223), Align::Left, w - 16);

    float py = y + h + 9;
    float frac = resumeFraction(*item);
    std::string left = timeLeft(*item);
    float lw = g.textWidth(left, 0.38f);
    g.progressBar(x, py, w - lw - 10, 4, frac >= 0 ? frac : 0, color::accent2);
    g.text(left, x + w, py - 5, 0.38f, color::accent2, Align::Right);

    g.wrapped(item->overview, x, py + 11, 0.4f, color::dim, w, 3);
    hints_.draw(g);
}

}  // namespace app
