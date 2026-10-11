#include "screens.hpp"

namespace app {

namespace {

class MenuScreen : public Screen {
public:
    MenuScreen(std::string title, std::vector<MenuOption> options) : title_(std::move(title)), options_(std::move(options)) {
        while (selected_ < static_cast<int>(options_.size()) && !options_[selected_].enabled) ++selected_;
    }

    bool overlay() const override { return true; }

    void update(const Input& in) override {
        int n = static_cast<int>(options_.size());
        if (in.down & KEY_B) {
            App::get().pop();
            return;
        }
        auto move = [&](int dir) {
            for (int i = 0; i < n; ++i) {
                selected_ = (selected_ + dir + n) % n;
                if (options_[selected_].enabled) break;
            }
        };
        if (in.repeat & KEY_DOWN) move(1);
        if (in.repeat & KEY_UP) move(-1);
        if (in.tapped) {
            // Tap outside the panel closes; tap on a row runs it.
            for (int i = 0; i < n; ++i) {
                float y = rowY(i);
                if (in.tapY >= y && in.tapY < y + ROW_H && in.tapX >= PANEL_X && in.tapX < BOTTOM_W - PANEL_X) {
                    if (options_[i].enabled) activate(i);
                    return;
                }
            }
            if (in.tapY < panelTop() || in.tapY > panelTop() + panelHeight()) App::get().pop();
            return;
        }
        if ((in.down & KEY_A) && n > 0 && options_[selected_].enabled) activate(selected_);
    }

    void drawTop(Gfx&) override {}

    void drawBottom(Gfx& g) override {
        g.rect(0, 0, BOTTOM_W, SCREEN_H, color::shade);
        float top = panelTop();
        g.rect(PANEL_X, top, BOTTOM_W - 2 * PANEL_X, panelHeight(), color::panel);
        g.rect(PANEL_X, top, BOTTOM_W - 2 * PANEL_X, 2, color::accent);
        g.text(title_, BOTTOM_W / 2.0f, top + 8, 0.5f, color::dim, Align::Center, BOTTOM_W - 2 * PANEL_X - 16);
        for (int i = 0; i < static_cast<int>(options_.size()); ++i) {
            float y = rowY(i);
            if (i == selected_) g.rect(PANEL_X + 4, y, BOTTOM_W - 2 * PANEL_X - 8, ROW_H - 2, color::panelHi);
            g.text(options_[i].label, PANEL_X + 14, y + 7, 0.52f, options_[i].enabled ? color::text : color::dim,
                   Align::Left, BOTTOM_W - 2 * PANEL_X - 28);
        }
    }

private:
    static constexpr float PANEL_X = 16;
    static constexpr float ROW_H = 30;

    float panelHeight() const { return 34 + options_.size() * ROW_H + 6; }
    float panelTop() const { return std::max(4.0f, (SCREEN_H - panelHeight()) / 2); }
    float rowY(int i) const { return panelTop() + 32 + i * ROW_H; }

    void activate(int i) {
        auto action = options_[i].action;
        App::get().pop();
        if (action) action();
    }

    std::string title_;
    std::vector<MenuOption> options_;
    int selected_ = 0;
};

}  // namespace

std::unique_ptr<Screen> makeMenu(std::string title, std::vector<MenuOption> options) {
    return std::make_unique<MenuScreen>(std::move(title), std::move(options));
}

std::unique_ptr<Screen> makeConfirm(std::string question, std::string yesLabel, std::function<void()> onYes) {
    return makeMenu(std::move(question), {{std::move(yesLabel), std::move(onYes)}, {"Cancel", nullptr}});
}

}  // namespace app
