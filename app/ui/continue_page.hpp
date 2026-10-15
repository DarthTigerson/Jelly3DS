#pragma once

#include "screens.hpp"

#include <string>
#include <vector>

namespace app {

/// The Home screen's Continue tab: resume + next up as covers.
/// Top screen: SELECT cycles Shelf (wide stills) → Grid (posters) → Hero (backdrop).
/// Bottom screen: the selected episode's still, progress, overview and a one-line hint strip.
class ContinuePage {
public:
    ContinuePage();

    void setItems(std::vector<jelly::Item> items);
    void setLoading(bool loading) { loading_ = loading; }
    void setEmptyText(std::string text) { emptyText_ = std::move(text); }
    void refreshBadges() { rebuildGrid(); }

    /// Returns true when it consumed the input.
    void update(const Input& in);
    void drawTop(Gfx& g, float y, float h);
    void drawBottom(Gfx& g, float top);

private:
    enum class View { Shelf, Grid, Hero };

    const jelly::Item* selected() const;
    void select(int index);
    void cycleView();
    void buildHints();
    void rebuildGrid();
    static const char* viewName(View v);

    void drawShelf(Gfx& g, float y, float h);
    void drawHero(Gfx& g, float y, float h);
    void drawCard(Gfx& g, const jelly::Item& item, float cx, float top, float w, float h, float focus);

    std::vector<jelly::Item> items_;
    int selected_ = 0;
    float shelfPos_ = 0;  // eased towards selected_
    float stripPos_ = 0;
    View view_ = View::Shelf;
    GridView grid_;
    HintStrip hints_;
    bool loading_ = false;
    std::string emptyText_ = "Nothing in progress";
};

}  // namespace app
