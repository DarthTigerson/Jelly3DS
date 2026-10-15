#pragma once

#include "gfx.hpp"
#include "image_cache.hpp"
#include "tasks.hpp"

#include "curl_http.hpp"
#include "jelly/client.hpp"
#include "jelly/config.hpp"
#include "jelly/downloads.hpp"
#include "jelly/progress.hpp"
#include "jelly/sync.hpp"
#include "jelly/transcode.hpp"

#include <3ds.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace app {

constexpr const char* DATA_DIR = "sdmc:/3ds/Jelly3DS";

struct Input {
    u32 down = 0;    // pressed this frame
    u32 held = 0;
    u32 repeat = 0;  // down + auto-repeat for held d-pad / shoulder keys
    bool touching = false;
    bool tapped = false;  // touch released this frame without dragging far
    int touchX = 0, touchY = 0;
    int tapX = 0, tapY = 0;
};

class Screen;

class App {
public:
    static App& get();

    bool init();
    void run();
    void shutdown();
    void quit();
    /// Sleep / HOME menu: lets the current screen pause playback and saves progress.
    void suspendTop();

    // Navigation (applied after the current frame's update)
    void push(std::unique_ptr<Screen> s);
    void pop();
    void replaceAll(std::unique_ptr<Screen> s);
    /// Pops everything above the root (Home) screen.
    void goHome();

    void toast(const std::string& message);
    void saveConfig();
    jelly::DeviceProfile profile() const;
    std::string profileLabel() const;

    /// Checks the server in the background and, when reachable, syncs offline progress.
    void checkOnline(bool thenSync = true);
    void logout();
    bool online() const { return online_; }
    bool wifiConnected() const { return wifi_; }

    void setBottomScreenOn(bool on);
    /// Both backlights off until any button is pressed (saves battery while downloading).
    void screensOff();
    bool bottomScreenOn() const { return bottomOn_; }
    /// Called by the player: true while video is playing (keeps the downloader polite on Old 3DS).
    void setPlaying(bool playing) { playing_ = playing; }

    bool isNew3ds = false;
    /// False on the original 2DS: one panel and one backlight, so "bottom screen off" would blank everything.
    bool canTurnOffBottomScreen = true;
    jelly::Config config;
    std::unique_ptr<jelly::CurlHttp> http;
    std::unique_ptr<jelly::Client> client;
    std::unique_ptr<jelly::ResponseCache> responseCache;
    std::unique_ptr<jelly::ProgressStore> progress;
    std::unique_ptr<jelly::DownloadManager> downloads;
    std::unique_ptr<jelly::SyncEngine> sync;
    Gfx gfx;
    ImageCache images;
    Tasks tasks;

    std::string configPath() const { return std::string(DATA_DIR) + "/config.json"; }

private:
    void readInput(Input& in);
    void applyNavigation();
    void downloadLoop();
    void pollWifi();
    void drawToast();

    std::vector<std::unique_ptr<Screen>> stack_;
    std::vector<std::unique_ptr<Screen>> pendingPush_;
    int pendingPops_ = 0;
    bool pendingReplace_ = false;

    std::string toast_;
    u64 toastUntil_ = 0;

    std::atomic_bool online_{false};
    std::atomic_bool wifi_{false};
    std::atomic_bool checking_{false};
    std::atomic_bool playing_{false};
    std::atomic_bool quitting_{false};
    bool bottomOn_ = true;
    u64 nextWifiPoll_ = 0;
    u32 heldFrames_ = 0;
    bool touchMoved_ = false;
    int touchStartX_ = 0, touchStartY_ = 0;
    int lastTouchX_ = 0, lastTouchY_ = 0;
    bool sleepBlocked_ = false;
    bool lidDark_ = false;      // backlights off because the lid closed during a download
    bool screensOff_ = false;   // backlights off on request
    bool ptmSysm_ = false;
    bool mcuHwc_ = false;
    bool doneLed_ = false;      // green notification light: queue finished while the lid was shut
    int completedAtLidClose_ = 0;
    void setDoneLed(bool on);
    int completedCount() const;
    u64 nextLidPoll_ = 0;
    void manageSleep();
    Thread downloadThread_;
    aptHookCookie aptCookie_{};
};

/// Base class for every screen. `alive()` tokens let async callbacks detect that their screen is gone.
class Screen {
public:
    virtual ~Screen() { *alive_ = false; }
    virtual void update(const Input& in) = 0;
    virtual void drawTop(Gfx& g) = 0;
    virtual void drawBottom(Gfx& g) = 0;
    virtual void onResume() {}
    /// Overlays (menus, dialogs) let the screen below draw first.
    virtual bool overlay() const { return false; }
    /// Player sleep/suspend hook.
    virtual void onSuspend() {}
    /// START returns to the Home screen from here (the player opts out: B stops it properly).
    virtual bool startGoesHome() const { return true; }
    /// True while the screen wants the bottom backlight to stay off (headphone mode in the player).
    virtual bool keepsBottomOff() const { return false; }

protected:
    template <typename T>
    void async(std::function<T()> work, std::function<void(T)> done) {
        std::weak_ptr<bool> alive = alive_;
        App& a = App::get();
        a.tasks.run([work = std::move(work), done = std::move(done), alive, &a]() mutable {
            auto result = std::make_shared<T>(work());
            a.tasks.post([done = std::move(done), alive, result]() {
                auto token = alive.lock();
                if (token && *token) done(std::move(*result));
            });
        });
    }

    /// Shows the copy saved on the SD card straight away, then the server's answer when it arrives.
    /// `done(result, fresh)` may run twice. `job` runs on the worker thread: capture copies, never `this`.
    template <typename T>
    void loadCachedThenFresh(std::function<jelly::Result<T>(jelly::Source)> job,
                             std::function<void(jelly::Result<T>, bool)> done) {
        async<jelly::Result<T>>([job] { return job(jelly::Source::Cache); },
                                [this, job, done](jelly::Result<T> cached) {
                                    bool had = cached.ok;
                                    if (had) done(std::move(cached), false);
                                    if (!App::get().online()) {
                                        if (!had)
                                            done(jelly::Result<T>::failure("Offline - not saved on this console yet"), true);
                                        return;
                                    }
                                    async<jelly::Result<T>>([job] { return job(jelly::Source::Network); },
                                                            [done, had](jelly::Result<T> fresh) {
                                                                // Keep showing the saved list if the server fails.
                                                                if (fresh.ok || !had) done(std::move(fresh), true);
                                                            });
                                });
    }

    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

/// Opens the system keyboard. Returns false when cancelled.
bool keyboard(const std::string& hint, std::string& value, bool password = false, int maxLength = 256);

/// Button glyphs from the system font.
namespace glyph {
constexpr const char* A = "\xEE\x80\x80";
constexpr const char* B = "\xEE\x80\x81";
constexpr const char* X = "\xEE\x80\x82";
constexpr const char* Y = "\xEE\x80\x83";
constexpr const char* L = "\xEE\x80\x84";
constexpr const char* R = "\xEE\x80\x85";
constexpr const char* DPad = "\xEE\x80\x86";
constexpr const char* Start = "\xEE\x81\xB3";
constexpr const char* Select = "\xEE\x81\xB4";
}  // namespace glyph

}  // namespace app
