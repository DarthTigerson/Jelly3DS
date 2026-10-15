#pragma once

#include "screens.hpp"

#include <string>
#include <vector>

namespace app {

/// The Home screen's Downloads tab. SELECT switches between:
///  - Saved: one cover per downloaded show / movie; the bottom screen lists that show's episodes.
///  - Queue: the running download (speed, time left) and what waits, plus SD card usage.
class DownloadsPage {
public:
    DownloadsPage();

    /// Re-reads the download index (call a few times a second while visible).
    void refresh();
    void update(const Input& in);
    void drawTop(Gfx& g, float y, float h);
    void drawBottom(Gfx& g, float top);
    /// "4.1 GB free" for the header.
    std::string headerText() const;

private:
    struct Group {
        std::string key;    // series id, or the item id for a movie
        std::string title;
        std::vector<jelly::DownloadItem> items;  // sorted by season / episode
        int saved = 0;
        int64_t bytes = 0;
        int downloadingPct = -1;
    };

    void rebuildGroups();
    void buildHints();
    const Group* currentGroup() const;
    const jelly::DownloadItem* nextToWatch(const Group& g) const;
    std::vector<jelly::DownloadItem> queueItems() const;
    void deleteWatched(const Group& g);

    void drawQueueTop(Gfx& g, float y, float h);
    void drawQueueBottom(Gfx& g, float top);
    void drawSavedBottom(Gfx& g, float top);

    bool queueView_ = false;
    std::vector<jelly::DownloadItem> all_;
    std::vector<Group> groups_;
    GridView grid_;
    int queueSel_ = 0;
    HintStrip hints_;
    int64_t freeBytes_ = -1;
    u64 nextFreeCheck_ = 0;
};

}  // namespace app
