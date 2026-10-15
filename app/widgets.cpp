#include "widgets.hpp"

#include <cmath>

namespace app {

void ListView::setRows(std::vector<ListRow> r, bool keepSelection) {
    rows = std::move(r);
    if (!keepSelection) {
        selected = 0;
        scroll_ = 0;
    }
    clamp();
    skipHeaders(1);
}

void ListView::clamp() {
    if (rows.empty()) {
        selected = 0;
        return;
    }
    if (selected < 0) selected = 0;
    if (selected >= static_cast<int>(rows.size())) selected = static_cast<int>(rows.size()) - 1;
}

void ListView::skipHeaders(int dir) {
    if (rows.empty()) return;
    int n = static_cast<int>(rows.size());
    int s = selected;
    while (s >= 0 && s < n && rows[s].header) s += dir;
    if (s < 0 || s >= n) {
        // Nothing selectable that way: search the other direction.
        s = selected;
        while (s >= 0 && s < n && rows[s].header) s -= dir;
    }
    if (s >= 0 && s < n) selected = s;
}

const ListRow* ListView::current() const {
    if (selected < 0 || selected >= static_cast<int>(rows.size()) || rows[selected].header) return nullptr;
    return &rows[selected];
}

bool ListView::update(const Input& in, float visibleHeight) {
    if (rows.empty()) return false;
    int before = selected;
    int page = std::max(1, static_cast<int>(visibleHeight / rowHeight) - 1);
    if (in.repeat & KEY_DOWN) {
        if (selected + 1 < static_cast<int>(rows.size())) {
            ++selected;
            skipHeaders(1);
        } else if (in.down & KEY_DOWN) {
            selected = 0;  // wrap only on a fresh press, not while held
            skipHeaders(1);
        }
    }
    if (in.repeat & KEY_UP) {
        if (selected > 0) {
            --selected;
            skipHeaders(-1);
        } else if (in.down & KEY_UP) {
            selected = static_cast<int>(rows.size()) - 1;
            skipHeaders(-1);
        }
    }
    if (paging && (in.repeat & KEY_RIGHT)) {
        selected += page;
        clamp();
        skipHeaders(1);
    }
    if (paging && (in.repeat & KEY_LEFT)) {
        selected -= page;
        clamp();
        skipHeaders(-1);
    }
    return selected != before;
}

void ListView::draw(Gfx& g, float x, float y, float w, float h) {
    if (rows.empty()) {
        g.text(emptyText, x + w / 2, y + h / 2 - 10, 0.55f, color::dim, Align::Center);
        return;
    }
    // Keep the selection in view with a little easing.
    float target = scroll_;
    float selTop = selected * rowHeight;
    if (selTop < target) target = selTop;
    if (selTop + rowHeight > target + h) target = selTop + rowHeight - h;
    float maxScroll = std::max(0.0f, rows.size() * rowHeight - h);
    target = std::min(std::max(target, 0.0f), maxScroll);
    scroll_ += (target - scroll_) * 0.35f;
    if (std::fabs(target - scroll_) < 0.5f) scroll_ = target;

    int first = std::max(0, static_cast<int>(scroll_ / rowHeight));
    int last = std::min(static_cast<int>(rows.size()), static_cast<int>((scroll_ + h) / rowHeight) + 1);
    for (int i = first; i < last; ++i) {
        const auto& r = rows[i];
        float ry = y + i * rowHeight - scroll_;
        if (ry + rowHeight < y || ry > y + h) continue;
        if (r.header) {
            g.text(r.title, x + 8, ry + rowHeight - 20, 0.5f, color::accent2, Align::Left, w - 16);
            g.rect(x + 8, ry + rowHeight - 3, w - 16, 1, color::panelHi);
            continue;
        }
        bool sel = i == selected;
        if (sel) {
            g.rect(x, ry + 1, w, rowHeight - 2, color::panelHi);
            g.rect(x, ry + 1, 3, rowHeight - 2, color::accent);
        }
        float tx = x + 10;
        if (thumbW > 0) {
            float boxW = thumbW, boxH = rowHeight - 6;
            g.rect(tx, ry + 3, boxW, boxH, color::panel);
            if (const Texture* t = App::get().images.get(r.image)) {
                float s = std::min(boxW / t->width, boxH / t->height);
                float dw = t->width * s, dh = t->height * s;
                g.image(*t, tx + (boxW - dw) / 2, ry + 3 + (boxH - dh) / 2, dw, dh);
            }
            tx += boxW + 8;
        }
        float badgeW = r.badge.empty() ? 0 : g.textWidth(r.badge, 0.45f) + 10;
        float textW = x + w - tx - badgeW - 6;
        if (r.subtitle.empty()) {
            g.text(r.title, tx, ry + rowHeight / 2 - 9, 0.55f, color::text, Align::Left, textW);
        } else {
            g.text(r.title, tx, ry + 3, 0.52f, color::text, Align::Left, textW);
            g.text(r.subtitle, tx, ry + rowHeight / 2 + 1, 0.42f, color::dim, Align::Left, textW);
        }
        if (!r.badge.empty())
            g.text(r.badge, x + w - 8, ry + rowHeight / 2 - 8, 0.45f, r.badgeColor, Align::Right);
        if (r.progress >= 0) g.progressBar(tx, ry + rowHeight - 5, x + w - tx - 8, 2, r.progress, color::accent2);
    }
    // Scrollbar
    if (maxScroll > 0) {
        float barH = std::max(16.0f, h * h / (rows.size() * rowHeight));
        float barY = y + (h - barH) * (scroll_ / maxScroll);
        g.rect(x + w - 2, barY, 2, barH, color::dim);
    }
}

void GridView::setItems(std::vector<ListRow> r, bool keepSelection) {
    items = std::move(r);
    if (!keepSelection) {
        selected = 0;
        scroll_ = 0;
    }
    if (selected >= static_cast<int>(items.size())) selected = std::max(0, static_cast<int>(items.size()) - 1);
}

const ListRow* GridView::current() const {
    if (selected < 0 || selected >= static_cast<int>(items.size())) return nullptr;
    return &items[selected];
}

bool GridView::update(const Input& in) {
    int n = static_cast<int>(items.size());
    if (n == 0) return false;
    int before = selected;
    if (in.repeat & KEY_RIGHT) selected = std::min(n - 1, selected + 1);
    if (in.repeat & KEY_LEFT) selected = std::max(0, selected - 1);
    if (in.repeat & KEY_DOWN) selected = std::min(n - 1, selected + columns);
    if (in.repeat & KEY_UP) selected = std::max(0, selected - columns);
    return selected != before;
}

void GridView::draw(Gfx& g, float x, float y, float w, float h) {
    if (items.empty()) {
        g.text(emptyText, x + w / 2, y + h / 2 - 10, 0.55f, color::dim, Align::Center);
        return;
    }
    const float cellW = w / columns;
    const float cellH = posterH + 26;
    // Keep the selected row in view, easing like the list.
    int row = selected / columns;
    float target = scroll_;
    if (row * cellH < target) target = row * cellH;
    if ((row + 1) * cellH > target + h) target = (row + 1) * cellH - h;
    int rows = (static_cast<int>(items.size()) + columns - 1) / columns;
    target = std::min(std::max(0.0f, target), std::max(0.0f, rows * cellH - h));
    scroll_ += (target - scroll_) * 0.35f;
    if (std::fabs(target - scroll_) < 0.5f) scroll_ = target;

    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        float cy = y + (i / columns) * cellH - scroll_ + 6;
        if (cy + cellH < y || cy > y + h) continue;
        float cx = x + (i % columns) * cellW + (cellW - posterW) / 2;
        const auto& it = items[i];
        bool sel = i == selected;
        if (sel) g.rect(cx - 3, cy - 3, posterW + 6, posterH + 6, color::accent);
        g.rect(cx, cy, posterW, posterH, color::panel);
        if (const Texture* t = App::get().images.get(it.image)) {
            // Fill the poster box (crop the overflow by drawing centred at cover scale).
            float s = std::min(posterW / t->width, posterH / t->height);
            float dw = t->width * s, dh = t->height * s;
            g.image(*t, cx + (posterW - dw) / 2, cy + (posterH - dh) / 2, dw, dh);
        } else {
            g.wrapped(it.title, cx + 4, cy + 6, 0.4f, color::dim, posterW - 8, 5);
        }
        if (it.progress >= 0) g.progressBar(cx, cy + posterH - 3, posterW, 3, it.progress, color::accent2, color::shade);
        if (!it.badge.empty()) {
            float bw = g.textWidth(it.badge, 0.34f) + 6;
            g.rect(cx + posterW - bw, cy, bw, 13, color::shade);
            g.text(it.badge, cx + posterW - bw + 3, cy + 1, 0.34f, it.badgeColor);
        }
        g.text(it.title, cx + posterW / 2, cy + posterH + 4, 0.4f, sel ? color::text : color::dim, Align::Center,
               cellW - 4);
    }
}

bool ButtonBar::update(const Input& in) {
    if (buttons.empty()) return false;
    float w = static_cast<float>(BOTTOM_W) / buttons.size();
    for (size_t i = 0; i < buttons.size(); ++i) {
        auto& b = buttons[i];
        if (!b.enabled || !b.action) continue;
        bool touched = in.tapped && in.tapY >= SCREEN_H - HEIGHT && in.tapX >= i * w && in.tapX < (i + 1) * w;
        if (touched || (b.key && (in.down & b.key))) {
            auto action = b.action;  // the action may rebuild `buttons`
            action();
            return true;
        }
    }
    return false;
}

void ButtonBar::draw(Gfx& g) {
    if (buttons.empty()) return;
    float y = SCREEN_H - HEIGHT;
    g.rect(0, y, BOTTOM_W, HEIGHT, color::panel);
    float w = static_cast<float>(BOTTOM_W) / buttons.size();
    for (size_t i = 0; i < buttons.size(); ++i) {
        const auto& b = buttons[i];
        if (i > 0) g.rect(i * w, y + 6, 1, HEIGHT - 12, color::panelHi);
        g.text(b.label, i * w + w / 2, y + 9, 0.47f, b.enabled ? color::text : color::dim, Align::Center, w - 6);
    }
}

bool HintStrip::update(const Input& in) {
    for (size_t i = 0; i < hints.size(); ++i) {
        const auto& h = hints[i];
        if (!h.action) continue;
        bool tapped = in.tapped && in.tapY >= SCREEN_H - HEIGHT - 4 && i < spans_.size() && in.tapX >= spans_[i].first &&
                      in.tapX < spans_[i].second;
        if (tapped || (h.keyMask && (in.down & h.keyMask))) {
            auto action = h.action;  // may rebuild `hints`
            action();
            return true;
        }
    }
    return false;
}

void HintStrip::draw(Gfx& g) {
    const float y = SCREEN_H - HEIGHT;
    g.rect(0, y, BOTTOM_W, HEIGHT, color::panel);
    // Measure, then spread the hints evenly across the strip.
    std::vector<float> keyW, labelW;
    float total = 0;
    for (const auto& h : hints) {
        bool pill = h.key.size() > 3;  // words like SELECT, not a single glyph
        float kw = pill ? g.textWidth(h.key, 0.3f) + 6 : g.textWidth(h.key, 0.45f);
        float lw = g.textWidth(h.label, 0.4f);
        keyW.push_back(kw);
        labelW.push_back(lw);
        total += kw + 3 + lw;
    }
    float gap = hints.size() > 1 ? (BOTTOM_W - 16 - total) / (hints.size() - 1) : 0;
    float x = 8;
    spans_.clear();
    for (size_t i = 0; i < hints.size(); ++i) {
        const auto& h = hints[i];
        float start = x;
        if (h.key.size() > 3) {
            g.outline(x, y + 4, keyW[i], 10, color::dim);
            g.text(h.key, x + keyW[i] / 2, y + 5, 0.3f, color::dim, Align::Center);
        } else {
            g.text(h.key, x, y + 2, 0.45f, color::text);
        }
        x += keyW[i] + 3;
        g.text(h.label, x, y + 3, 0.4f, i == 0 ? color::text : color::dim);
        x += labelW[i];
        spans_.push_back({start - gap / 2, x + gap / 2});
        x += gap;
    }
}

void drawHeader(Gfx& g, const std::string& title, const std::string& right) {
    g.rect(0, 0, TOP_W, 26, color::panel);
    g.rect(0, 26, TOP_W, 1, color::accent);
    float rw = right.empty() ? 0 : g.text(right, TOP_W - 8, 5, 0.47f, color::dim, Align::Right);
    g.text(title, 8, 4, 0.55f, color::text, Align::Left, TOP_W - 24 - rw);
}

void drawLoading(Gfx& g, float x, float y, float w, float h, const std::string& msg) {
    static int frame = 0;
    ++frame;
    std::string dots(static_cast<size_t>((frame / 15) % 4), '.');
    float tw = g.textWidth(msg + "...", 0.55f);
    g.text(msg + dots, x + (w - tw) / 2, y + h / 2 - 10, 0.55f, color::dim);
}

}  // namespace app
