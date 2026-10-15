#pragma once

#include "screens.hpp"

#include <functional>
#include <string>
#include <vector>

namespace app {

/// A page of covers shared by Continue, Shows, Movies, seasons and episodes.
/// Top screen: SELECT cycles Shelf (wide art) → Grid (posters) → Hero (backdrop); the choice is remembered per page.
/// Bottom screen: the selected item's art and facts, plus a one-line hint strip.
class CoverPage {
public:
    struct Options {
        std::string viewKey;            // remembered in Config::views ("continue", "tvshows", ...)
        std::string defaultView = "grid";
        std::string heroLabel;          // small caps line above the title in Hero view
        bool letterJump = false;        // L / R jump between first letters (A–Z lists)
        /// Hints for the selected item (nullptr when the page is empty); SELECT is appended by the page.
        std::function<std::vector<Hint>(const jelly::Item*)> hints;
    };

    explicit CoverPage(Options options);

    void setItems(std::vector<jelly::Item> items);
    void setLoading(bool loading) { loading_ = loading; }
    void setEmptyText(std::string text) { emptyText_ = std::move(text); }
    void setHeroLabel(std::string label) { options_.heroLabel = std::move(label); }
    void refreshBadges() { rebuildGrid(); }

    const jelly::Item* selected() const;
    const std::vector<jelly::Item>& items() const { return items_; }

    void update(const Input& in);
    void drawTop(Gfx& g, float y, float h);
    void drawBottom(Gfx& g, float top);
    /// "F G H" around the selected item's first letter, for a header (letterJump pages).
    void drawLetters(Gfx& g, float right, float y);

private:
    enum class View { Shelf, Grid, Hero };

    void select(int index);
    void cycleView();
    void jumpLetter(int dir);
    void rebuildGrid();
    static const char* viewName(View v);
    static char letterOf(const jelly::Item& item);

    void drawShelf(Gfx& g, float y, float h);
    void drawHero(Gfx& g, float y, float h);
    void drawCard(Gfx& g, const jelly::Item& item, float cx, float top, float w, float h, float focus);
    void drawWideDetail(Gfx& g, const jelly::Item& item, float top);
    void drawPosterDetail(Gfx& g, const jelly::Item& item, float top);

    Options options_;
    std::vector<jelly::Item> items_;
    int selected_ = 0;
    float shelfPos_ = 0;
    float stripPos_ = 0;
    View view_ = View::Grid;
    GridView grid_;
    HintStrip hints_;
    bool loading_ = false;
    std::string emptyText_ = "Nothing here";
};

}  // namespace app
