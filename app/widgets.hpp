#pragma once

#include "app.hpp"
#include "gfx.hpp"
#include "image_cache.hpp"

#include <functional>
#include <string>
#include <vector>

namespace app {

struct ListRow {
    std::string title;
    std::string subtitle;
    std::string badge;
    u32 badgeColor = color::dim;
    float progress = -1;  // 0..1 draws a thin bar under the row
    bool header = false;  // non-selectable section header
    ImageCache::Source image;
    int tag = -1;  // caller data (index into its own model)
};

/// Scrollable list for the top screen: d-pad to move, L/R to page, optional thumbnails.
class ListView {
public:
    std::vector<ListRow> rows;
    int selected = 0;
    float rowHeight = 40;
    float thumbW = 0;  // 0 = no thumbnail column
    bool paging = true;  // d-pad left/right jumps a page
    std::string emptyText = "Nothing here";

    void setRows(std::vector<ListRow> r, bool keepSelection = true);
    /// Returns true when the selection moved.
    bool update(const Input& in, float visibleHeight);
    void draw(Gfx& g, float x, float y, float w, float h);
    const ListRow* current() const;

private:
    void clamp();
    void skipHeaders(int dir);
    float scroll_ = 0;
};

/// Poster shelf for the top screen: covers in a grid with titles, d-pad moves in 2D.
class GridView {
public:
    std::vector<ListRow> items;  // uses title, badge, progress, image
    int selected = 0;
    int columns = 5;
    float posterW = 64, posterH = 96;
    std::string emptyText = "Nothing here";

    void setItems(std::vector<ListRow> r, bool keepSelection = true);
    bool update(const Input& in);
    void draw(Gfx& g, float x, float y, float w, float h);
    const ListRow* current() const;

private:
    float scroll_ = 0;
};

struct Button {
    std::string label;
    u32 key = 0;  // keys that trigger it besides touch
    std::function<void()> action;
    bool enabled = true;
};

/// Row of touch buttons along the bottom of the bottom screen.
class ButtonBar {
public:
    std::vector<Button> buttons;
    static constexpr float HEIGHT = 34;

    /// Handles touch + key presses. Returns true if a button fired.
    bool update(const Input& in);
    void draw(Gfx& g);
};

/// Title strip for the top screen.
void drawHeader(Gfx& g, const std::string& title, const std::string& right = "");
/// Spinner-ish "Loading…" text centered in an area.
void drawLoading(Gfx& g, float x, float y, float w, float h, const std::string& msg = "Loading");

}  // namespace app
