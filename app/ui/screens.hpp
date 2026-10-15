#pragma once

#include "../app.hpp"
#include "../widgets.hpp"

#include "jelly/downloads.hpp"
#include "jelly/models.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace app {

// --- Screens ---------------------------------------------------------------------------------
std::unique_ptr<Screen> makeLoginScreen();
std::unique_ptr<Screen> makeHomeScreen();
std::unique_ptr<Screen> makeFolderScreen(const jelly::Item& folder);
std::unique_ptr<Screen> makeSeriesScreen(const jelly::Item& series);
std::unique_ptr<Screen> makeEpisodesScreen(const jelly::Item& series, const jelly::Item& season);
std::unique_ptr<Screen> makeDownloadOptions(std::vector<jelly::Item> items);

struct MenuOption {
    std::string label;
    std::function<void()> action;
    bool enabled = true;
};
/// Modal list drawn on the bottom screen. Options close the menu before running.
std::unique_ptr<Screen> makeMenu(std::string title, std::vector<MenuOption> options);
std::unique_ptr<Screen> makeConfirm(std::string question, std::string yesLabel, std::function<void()> onYes);

struct PlayRequest {
    std::string itemId;
    std::string mediaSourceId;
    std::string title;
    std::string subtitle;
    std::string seriesId;
    int64_t runTimeTicks = 0;
    std::string localPath;  // empty = stream from the server
    double startSeconds = 0;
    int audioStreamIndex = -1;
    int subtitleStreamIndex = -1;
};
std::unique_ptr<Screen> makePlayerScreen(PlayRequest request);

// --- Shared helpers ---------------------------------------------------------------------------

/// Resume point, preferring unsynced local progress over (possibly older) server data.
int64_t resumeTicks(const std::string& itemId, const jelly::UserData* server);
bool isPlayed(const std::string& itemId, const jelly::UserData* server);

/// Plays an item: from the SD card when downloaded, otherwise streamed (needs the server).
void playItem(const jelly::Item& item, bool fromStart);
void playDownload(const jelly::DownloadItem& d, bool fromStart);
/// Action menu for a playable item (resume / play / download / watched).
void openItemMenu(const jelly::Item& item);
/// Action menu for a downloaded item (works offline).
void openDownloadMenu(const jelly::DownloadItem& d);
void toggleWatched(const std::string& itemId, bool played, int64_t runTimeTicks);

ImageCache::Source posterFor(const jelly::Item& item, int w, int h);
ImageCache::Source thumbFor(const jelly::DownloadItem& d, int w, int h);
/// Wide 16:9 picture: the episode's own image, else the backdrop.
ImageCache::Source stillFor(const jelly::Item& item, int w, int h);
/// Tall 2:3 cover: the show's poster for an episode, else the item's own image.
ImageCache::Source coverFor(const jelly::Item& item, int w, int h);
/// Full-screen art: the show's or movie's backdrop, else the still.
ImageCache::Source backdropFor(const jelly::Item& item, int w, int h);

struct DownloadBadge {
    std::string text;
    u32 color = color::dim;
    float progress = -1;
};
DownloadBadge downloadBadge(const std::string& itemId);

/// Bottom-screen detail block for the selected item.
struct Detail {
    std::string title;
    std::string subtitle;
    std::string meta;
    std::string overview;
    std::string status;
    u32 statusColor = color::dim;
    float progress = -1;
    ImageCache::Source image;
};
Detail detailFor(const jelly::Item& item);
Detail detailFor(const jelly::DownloadItem& d);
void drawDetail(Gfx& g, const Detail& d, float top, float bottom);

/// Rebuilds the detail block only when the selection changes, or twice a second (it touches locks and SD state).
class DetailCache {
public:
    template <typename F>
    const Detail& get(int key, F make) {
        u64 now = osGetTime();
        if (key != key_ || now >= refreshAt_) {
            detail_ = make();
            key_ = key;
            refreshAt_ = now + 500;
        }
        return detail_;
    }
    void invalidate() { key_ = -1000000; }

private:
    int key_ = -1000000;
    u64 refreshAt_ = 0;
    Detail detail_;
};

std::string runtimeText(int64_t ticks);
int64_t sdFreeBytes();

}  // namespace app
