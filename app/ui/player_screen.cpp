#include "screens.hpp"

#include "../player/player.hpp"

#include "jelly/transcode.hpp"
#include "jelly/util.hpp"

#include <optional>

namespace app {

namespace {

using jelly::TICKS_PER_SECOND;

// Bottom-screen layout (the top screen shows only the video). Everything is centred.
constexpr float BAR_X = 12, BAR_Y = 50, BAR_W = BOTTOM_W - 24, BAR_H = 6;
constexpr float SKIP_W = 112, SKIP_H = 24, SKIP_X = (BOTTOM_W - SKIP_W) / 2, SKIP_Y = 76;
constexpr float BTN_W = 46, BTN_H = 40, PLAY_R = 28, GAP = 10;
constexpr float ROW_Y = 136;  // vertical centre of the transport row
constexpr float CHIP_Y = 172, CHIP_H = 24, CHIP_GAP = 8;
constexpr float BOOST_W = 84, SCREEN_W = 112;
constexpr float HINT_H = 18;

struct Rect {
    float x, y, w, h;
    bool hit(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

// Transport row: previous, −10s, play/pause (circle), +10s, next.
constexpr float ROW_W = BTN_W * 4 + PLAY_R * 2 + GAP * 4;
constexpr float ROW_X = (BOTTOM_W - ROW_W) / 2;
constexpr Rect PREV{ROW_X, ROW_Y - BTN_H / 2, BTN_W, BTN_H};
constexpr Rect BACK10{ROW_X + (BTN_W + GAP), ROW_Y - BTN_H / 2, BTN_W, BTN_H};
constexpr Rect PLAY{ROW_X + (BTN_W + GAP) * 2, ROW_Y - PLAY_R, PLAY_R * 2, PLAY_R * 2};
constexpr Rect FWD10{ROW_X + (BTN_W + GAP) * 2 + PLAY_R * 2 + GAP, ROW_Y - BTN_H / 2, BTN_W, BTN_H};
constexpr Rect NEXT{ROW_X + (BTN_W + GAP) * 3 + PLAY_R * 2 + GAP, ROW_Y - BTN_H / 2, BTN_W, BTN_H};
constexpr Rect SKIP{SKIP_X, SKIP_Y, SKIP_W, SKIP_H};
constexpr Rect PLAY_NEXT_BIG{30, 70, BOTTOM_W - 60, 48};
constexpr Rect BACK_BIG{30, 128, BOTTOM_W - 60, 40};

/// Chips row, centred; the screen-off chip is hidden on the original 2DS (one shared backlight).
Rect boostRect() {
    bool both = App::get().canTurnOffBottomScreen;
    float total = BOOST_W + (both ? CHIP_GAP + SCREEN_W : 0);
    return {(BOTTOM_W - total) / 2, CHIP_Y, BOOST_W, CHIP_H};
}
Rect screenRect() {
    Rect b = boostRect();
    return {b.x + BOOST_W + CHIP_GAP, CHIP_Y, SCREEN_W, CHIP_H};
}

class PlayerScreen : public Screen {
public:
    explicit PlayerScreen(PlayRequest req) : req_(std::move(req)) {
        playSessionId_ = jelly::randomHex(32);
        App::get().setPlaying(true);
        open(req_.startSeconds);
        findNeighbours();
        if (streaming() && req_.introEnd < 0 && App::get().online()) fetchIntro();
    }

    ~PlayerScreen() override {
        player_.reset();
        App::get().setPlaying(false);
        if (!App::get().bottomScreenOn()) App::get().setBottomScreenOn(true);
    }

    bool keepsBottomOff() const override { return bottomOff_; }
    bool startGoesHome() const override { return false; }  // B stops playback (and saves the position)

    void onSuspend() override {
        if (player_ && state_ == State::Playing) {
            player_->setPaused(true);
            savePosition();
        }
    }

    void update(const Input& in) override {
        if (player_) player_->updateTexture();
        u64 now = osGetTime();

        switch (state_) {
        case State::Opening:
            if (in.down & KEY_B) finish();
            return;
        case State::Error:
            if (in.down & KEY_A) open(resumeAt_);
            if (in.down & KEY_B) finish();
            return;
        case State::NextUp:
            if ((in.down & KEY_A) || (in.tapped && PLAY_NEXT_BIG.hit(in.tapX, in.tapY)) || now >= countdownUntil_) {
                startNext();
            } else if ((in.down & KEY_B) || (in.tapped && BACK_BIG.hit(in.tapX, in.tapY))) {
                App::get().pop();
            }
            return;
        case State::Playing: break;
        }

        // Bottom screen off: a tap anywhere turns it back on (and does nothing else).
        if (bottomOff_ && (in.down & KEY_TOUCH)) {
            toggleBottomScreen();
            touchConsumed_ = true;
            return;
        }
        if (touchConsumed_) {
            if (!in.touching) touchConsumed_ = false;
            return;
        }
        if (in.down & KEY_A) togglePause();
        if (in.repeat & KEY_LEFT) seekBy(-10);
        if (in.repeat & KEY_RIGHT) seekBy(10);
        if (in.repeat & KEY_L) seekBy(-60);
        if (in.repeat & KEY_R) seekBy(60);
        if ((in.down & KEY_Y) && inIntro()) seekTo(req_.introEnd);
        if (in.down & (KEY_UP | KEY_DOWN)) changeBoost((in.down & KEY_UP) ? 1 : -1);
        if ((in.down & KEY_X) && App::get().canTurnOffBottomScreen) toggleBottomScreen();
        if (in.down & KEY_B) {
            finish();
            return;
        }
        if (in.tapped) handleTap(in.tapX, in.tapY);
        if (in.touching && in.touchY >= BAR_Y - 12 && in.touchY <= BAR_Y + BAR_H + 12 && duration() > 0) {
            // Dragging along the bar previews the target; it is applied on release.
            dragTarget_ = std::clamp((in.touchX - BAR_X) / BAR_W, 0.0f, 1.0f) * duration();
        } else if (dragTarget_ >= 0 && !in.touching) {
            seekTo(dragTarget_);
            dragTarget_ = -1;
        }

        // Streams seek by re-opening at the new offset once the user stops pressing.
        if (pendingSeek_ >= 0 && now >= pendingSeekAt_) {
            double target = pendingSeek_;
            pendingSeek_ = -1;
            open(target);
            return;
        }

        if (now >= nextSave_) {
            nextSave_ = now + 10000;
            savePosition();
            if (streaming() && App::get().online()) reportProgress();
        }

        if (player_->failed()) {
            resumeAt_ = player_->position();
            error_ = player_->error() + "\nA: retry from " + jelly::formatDuration(static_cast<int64_t>(resumeAt_));
            savePosition();
            player_.reset();
            state_ = State::Error;
            return;
        }
        if (player_->ended()) onEnded();
    }

    // --- Top screen: only the video (or the next episode's picture between episodes) -----------

    void drawTop(Gfx& g) override {
        g.rect(0, 0, TOP_W, SCREEN_H, color::black);
        if (state_ == State::NextUp) {
            drawUpNextTop(g);
            return;
        }
        if (player_ && player_->texture().valid) {
            float w = player_->videoWidth(), h = player_->videoHeight();
            float s = std::min(TOP_W / w, SCREEN_H / h);
            g.image(player_->texture(), (TOP_W - w * s) / 2, (SCREEN_H - h * s) / 2, w * s, h * s);
        }
    }

    // --- Bottom screen: everything else -----------------------------------------------------------

    void drawBottom(Gfx& g) override {
        if (state_ == State::NextUp) {
            drawUpNextBottom(g);
            return;
        }
        // Title, then "Series · S01E03" with where it plays from, all centred.
        g.text(req_.title, BOTTOM_W / 2.0f, 6, 0.55f, color::text, Align::Center, BOTTOM_W - 24);
        std::string tag = streaming() ? "STREAMING" : "OFFLINE";
        float tagW = g.textWidth(tag, 0.32f) + 10;
        float subW = req_.subtitle.empty() ? 0 : std::min(g.textWidth(req_.subtitle, 0.4f), BOTTOM_W - 40 - tagW);
        float sx = (BOTTOM_W - subW - (subW > 0 ? 6 : 0) - tagW) / 2;
        if (subW > 0) g.text(req_.subtitle, sx, 26, 0.4f, color::dim, Align::Left, subW);
        float tx = sx + subW + (subW > 0 ? 6 : 0);
        g.rect(tx, 27, tagW, 12, streaming() ? color::accent2 : color::good);
        g.text(tag, tx + 5, 28, 0.32f, color::bg);

        if (state_ == State::Opening || state_ == State::Error) {
            drawStatus(g);
            drawHints(g);
            return;
        }

        drawSeekBar(g);
        if (inIntro()) {
            g.outline(SKIP.x, SKIP.y, SKIP.w, SKIP.h, color::text, 1.5f);
            g.text("Skip intro  \xE2\x80\xBA", SKIP.x + SKIP.w / 2, SKIP.y + 5, 0.45f, color::text, Align::Center);
        } else if (player_ && player_->buffering()) {
            drawLoading(g, 0, SKIP_Y - 4, BOTTOM_W, 30, "Buffering");
        } else if (pendingSeek_ >= 0 || dragTarget_ >= 0) {
            g.text("Seek to " + jelly::formatDuration(static_cast<int64_t>(dragTarget_ >= 0 ? dragTarget_ : pendingSeek_)),
                   BOTTOM_W / 2.0f, SKIP_Y + 4, 0.45f, color::accent2, Align::Center);
        }
        drawTransport(g);
        drawChips(g);
        drawHints(g);
    }

private:
    enum class State { Opening, Playing, Error, NextUp };

    bool streaming() const { return req_.localPath.empty(); }
    bool hasNext() const { return nextDownload_.has_value() || nextItem_.has_value(); }
    bool hasPrev() const { return prevDownload_.has_value() || prevItem_.has_value(); }

    double duration() const {
        if (player_ && player_->duration() > 0) return player_->duration();
        return static_cast<double>(req_.runTimeTicks) / TICKS_PER_SECOND;
    }

    double position() const {
        if (pendingSeek_ >= 0) return pendingSeek_;
        if (player_) return player_->position();
        return resumeAt_;
    }

    bool inIntro() const {
        if (req_.introEnd <= req_.introStart || state_ != State::Playing) return false;
        double p = position();
        return p >= req_.introStart && p < req_.introEnd - 1;
    }

    // --- drawing ----------------------------------------------------------------------------------

    void drawSeekBar(Gfx& g) {
        double d = duration();
        float frac = d > 0 ? static_cast<float>((dragTarget_ >= 0 ? dragTarget_ : position()) / d) : 0;
        frac = std::clamp(frac, 0.0f, 1.0f);
        g.rect(BAR_X, BAR_Y, BAR_W, BAR_H, color::panelHi);
        if (d > 0 && req_.introEnd > req_.introStart) {
            float a = static_cast<float>(req_.introStart / d), b = static_cast<float>(req_.introEnd / d);
            g.rect(BAR_X + BAR_W * a, BAR_Y, BAR_W * (b - a), BAR_H, color::rgba(74, 80, 112));
        }
        g.rect(BAR_X, BAR_Y, BAR_W * frac, BAR_H, color::accent);
        g.circle(BAR_X + BAR_W * frac, BAR_Y + BAR_H / 2, 7, color::text);

        int64_t pos = static_cast<int64_t>(position());
        g.text(jelly::formatDuration(pos), BAR_X, BAR_Y + 10, 0.4f, color::dim);
        if (d > 0) g.text("-" + jelly::formatDuration(static_cast<int64_t>(d) - pos), BAR_X + BAR_W, BAR_Y + 10, 0.4f,
                          color::dim, Align::Right);
    }

    void button(Gfx& g, const Rect& r, bool enabled) {
        g.roundRect(r.x, r.y, r.w, r.h, enabled ? color::panelHi : color::panel);
    }

    void drawTransport(Gfx& g) {
        float cy = ROW_Y;
        // Previous episode: |◀ icon
        bool prev = hasPrev();
        button(g, PREV, prev);
        u32 pink = prev ? color::text : color::dim;
        float px = PREV.x + BTN_W / 2;
        g.rect(px - 9, cy - 9, 3, 18, pink);
        g.triangle(px + 9, cy - 9, px + 9, cy + 9, px - 5, cy, pink);

        button(g, BACK10, true);
        g.text("-10s", BACK10.x + BTN_W / 2, ROW_Y - 8, 0.45f, color::text, Align::Center);
        button(g, FWD10, true);
        g.text("+10s", FWD10.x + BTN_W / 2, ROW_Y - 8, 0.45f, color::text, Align::Center);

        float cx = PLAY.x + PLAY_R;
        g.circle(cx, cy, PLAY_R, color::accent);
        if (player_ && player_->paused()) {
            g.triangle(cx - 7, cy - 11, cx - 7, cy + 11, cx + 11, cy, color::text);
        } else {
            g.rect(cx - 9, cy - 10, 6, 20, color::text);
            g.rect(cx + 3, cy - 10, 6, 20, color::text);
        }

        // Next episode: ▶| icon
        bool next = hasNext();
        button(g, NEXT, next);
        u32 ink = next ? color::text : color::dim;
        float nx = NEXT.x + BTN_W / 2;
        g.triangle(nx - 9, cy - 9, nx - 9, cy + 9, nx + 5, cy, ink);
        g.rect(nx + 6, cy - 9, 3, 18, ink);
    }

    void drawChips(Gfx& g) {
        int boost = App::get().config.volumeBoost;
        Rect b = boostRect();
        g.roundRect(b.x, b.y, b.w, b.h, color::panel);
        g.speaker(b.x + 8, b.y + 5, 14, color::dim);
        g.text(boost == 1 ? "Boost off" : "Boost " + std::to_string(boost) + "\xC3\x97", b.x + 27, b.y + 5, 0.4f, color::dim);
        if (App::get().canTurnOffBottomScreen) {
            Rect r = screenRect();
            g.roundRect(r.x, r.y, r.w, r.h, color::panel);
            g.text("Bottom screen off", r.x + r.w / 2, r.y + 5, 0.4f, color::dim, Align::Center);
        }
    }

    /// Hint strip with drawn d-pad arrows and a speaker (nothing depends on font glyphs), centred.
    void drawHints(Gfx& g) {
        const float y = SCREEN_H - HINT_H;
        g.rect(0, y, BOTTOM_W, HINT_H, color::panel);
        float width = layoutHints(g, false, 0);  // measure first,
        layoutHints(g, true, (BOTTOM_W - width) / 2);  // then draw centred
    }

    /// Lays the hints out from `x`; draws only when `draw`. Returns the width used.
    float layoutHints(Gfx& g, bool draw, float x0) {
        const float y = SCREEN_H - HINT_H, cy = y + HINT_H / 2;
        const u32 ink = color::dim;
        float x = x0;
        bool first = true;
        auto gap = [&] {
            if (!first) x += 12;
            first = false;
        };
        auto label = [&](const std::string& s, u32 c) {
            x += draw ? g.text(s, x, y + 3, 0.4f, c) : g.textWidth(s, 0.4f);
        };
        auto key = [&](const char* gl) {
            gap();
            x += (draw ? g.text(gl, x, y + 2, 0.45f, color::text) : g.textWidth(gl, 0.45f)) + 3;
        };
        if (state_ == State::Opening) {
            key(glyph::B);
            label("Cancel", ink);
            return x - x0;
        }
        if (state_ == State::Error) {
            key(glyph::A);
            label("Retry", color::text);
            key(glyph::B);
            label("Back", ink);
            return x - x0;
        }
        bool paused = player_ && player_->paused();
        key(glyph::A);
        label(paused ? "Play" : "Pause", color::text);
        if (inIntro()) {
            key(glyph::Y);
            label("Skip intro", ink);
        }
        // ◀▶ 10s
        gap();
        if (draw) {
            g.triangle(x, cy, x + 5, cy - 4, x + 5, cy + 4, ink);
            g.triangle(x + 12, cy, x + 7, cy - 4, x + 7, cy + 4, ink);
        }
        x += 15;
        label("10s", ink);
        // ▲▼ + speaker
        gap();
        if (draw) {
            g.triangle(x + 3, cy - 6, x, cy - 1, x + 6, cy - 1, ink);
            g.triangle(x + 3, cy + 6, x, cy + 1, x + 6, cy + 1, ink);
            g.speaker(x + 9, y + 3, 12, ink);
        }
        x += 21;
        key(glyph::B);
        label("Stop", ink);
        return x - x0;
    }

    void drawStatus(Gfx& g) {
        if (state_ == State::Opening) {
            drawLoading(g, 0, 70, BOTTOM_W, 40, streaming() ? "Your server is converting the video" : "Opening");
        } else {
            g.text("Playback stopped", BOTTOM_W / 2.0f, 60, 0.6f, color::bad, Align::Center);
            g.wrapped(error_, 16, 90, 0.45f, color::text, BOTTOM_W - 32, 6);
        }
    }

    ImageCache::Source nextImage() const {
        if (nextDownload_) return thumbFor(*nextDownload_, 384, 216);
        if (nextItem_) return stillFor(*nextItem_, 384, 216);
        return {};
    }

    std::string nextLabel() const {
        if (nextDownload_) return nextDownload_->label();
        if (nextItem_) return nextItem_->label();
        return "";
    }

    void drawUpNextTop(Gfx& g) {
        g.rect(0, 0, TOP_W, SCREEN_H, color::panelHi);
        if (const Texture* t = App::get().images.get(nextImage())) g.imageCover(*t, 0, 0, TOP_W, SCREEN_H);
        g.gradient(0, 90, TOP_W, 150, color::rgba(16, 18, 26, 0), color::rgba(16, 18, 26, 0), color::bg, color::bg);
        g.text("UP NEXT", 16, 160, 0.42f, color::accent);
        g.text(nextLabel(), 16, 176, 0.7f, color::text, Align::Left, 300);
        g.text(nextDownload_ ? "Saved on SD" : "Streams from your server", 16, 202, 0.42f, color::dim);
        // Countdown ring
        float cx = TOP_W - 44, cy = 196;
        u64 now = osGetTime();
        float left = countdownUntil_ > now ? (countdownUntil_ - now) / 5000.0f : 0;
        g.circle(cx, cy, 26, color::accent);
        g.circle(cx, cy, 21, color::bg);
        g.rect(cx - 26, cy - 26, 52, 52 * (1 - left), color::rgba(16, 18, 26, 160));
        g.text(std::to_string(static_cast<int>(left * 5) + 1), cx, cy - 12, 0.8f, color::text, Align::Center);
    }

    void drawUpNextBottom(Gfx& g) {
        g.text("Finished: " + req_.title, BOTTOM_W / 2.0f, 24, 0.42f, color::dim, Align::Center, BOTTOM_W - 24);
        g.roundRect(PLAY_NEXT_BIG.x, PLAY_NEXT_BIG.y, PLAY_NEXT_BIG.w, PLAY_NEXT_BIG.h, color::accent);
        g.text("Play next episode", BOTTOM_W / 2.0f, PLAY_NEXT_BIG.y + 14, 0.6f, color::text, Align::Center);
        g.roundRect(BACK_BIG.x, BACK_BIG.y, BACK_BIG.w, BACK_BIG.h, color::panelHi);
        g.text("Back to the show", BOTTOM_W / 2.0f, BACK_BIG.y + 11, 0.5f, color::text, Align::Center);
        const float y = SCREEN_H - HINT_H;
        g.rect(0, y, BOTTOM_W, HINT_H, color::panel);
        float x = 8;
        x += g.text(glyph::A, x, y + 2, 0.45f, color::text) + 3;
        x += g.text("Play now", x, y + 3, 0.4f, color::text) + 14;
        x += g.text(glyph::B, x, y + 2, 0.45f, color::text) + 3;
        g.text("Back to show", x, y + 3, 0.4f, color::dim);
    }

    // --- actions ----------------------------------------------------------------------------------

    void open(double at) {
        auto& a = App::get();
        state_ = State::Opening;
        resumeAt_ = at;
        player_.reset();
        Player::Source src;
        if (streaming()) {
            // Every (re)open needs its own play session: Jellyfin names the transcode after the session,
            // so reusing it after a seek hands back the previous transcode (playing from the beginning).
            if (streamStarted_) {
                std::string old = playSessionId_;
                a.tasks.run([old] { App::get().client->stopEncoding(old); });
            }
            playSessionId_ = jelly::randomHex(32);
            streamStarted_ = true;
            jelly::StreamRequest sr;
            sr.itemId = req_.itemId;
            sr.mediaSourceId = req_.mediaSourceId;
            sr.audioStreamIndex = req_.audioStreamIndex;
            sr.subtitleStreamIndex = req_.subtitleStreamIndex;
            sr.startTimeTicks = static_cast<int64_t>(at * TICKS_PER_SECOND);
            sr.deviceId = a.client->deviceId();
            sr.playSessionId = playSessionId_;
            src.url = jelly::buildStreamUrl(a.client->server(), a.profile(), sr);
            src.headers = a.client->authHeaders();
            src.timeOffset = at;
            src.verifyTls = a.config.verifyTls;
            src.caFile = "romfs:/cacert.pem";
        } else {
            src.path = req_.localPath;
        }
        bool hw = a.isNew3ds;
        bool fast = !a.isNew3ds;
        auto p = std::make_shared<Player>();
        async<std::string>(
            [p, src, at] {
                std::string err;
                if (!p->open(src, at, err)) return err.empty() ? std::string("Could not start playback") : err;
                return std::string();
            },
            [this, p, hw, fast](std::string err) {
                if (err.empty() && !p->start(hw, fast, err) && err.empty()) err = "Could not start playback";
                if (!err.empty()) {
                    error_ = err;
                    state_ = State::Error;
                    return;
                }
                player_ = p;
                player_->setGain(static_cast<float>(App::get().config.volumeBoost));
                state_ = State::Playing;
                nextSave_ = osGetTime() + 10000;
                if (streaming() && App::get().online() && !reportedStart_) {
                    reportedStart_ = true;
                    auto id = req_.itemId, ms = req_.mediaSourceId, ps = playSessionId_;
                    auto ticks = static_cast<int64_t>(resumeAt_ * TICKS_PER_SECOND);
                    App::get().tasks.run([id, ms, ps, ticks] { App::get().client->reportStart(id, ms, ticks, ps, false); });
                }
            });
    }

    /// Previous / next episode: downloaded ones first (work offline), else the neighbours on the server.
    void findNeighbours() {
        auto& a = App::get();
        nextDownload_ = a.downloads->nextEpisode(req_.itemId);
        prevDownload_ = a.downloads->previousEpisode(req_.itemId);
        if ((nextDownload_ && prevDownload_) || req_.seriesId.empty() || !a.online()) return;
        std::string seriesId = req_.seriesId;
        auto cur = std::make_pair(req_.seasonIndex, req_.episodeIndex);
        using Pair = std::pair<std::optional<jelly::Item>, std::optional<jelly::Item>>;
        async<Pair>(
            [seriesId, cur]() -> Pair {
                Pair out;
                auto r = App::get().client->episodes(seriesId);
                if (!r.ok) return out;
                for (const auto& e : r.value) {
                    auto key = std::make_pair(e.parentIndexNumber, e.indexNumber);
                    if (key < cur) out.first = e;                       // last one before
                    if (key > cur && !out.second) out.second = e;      // first one after
                }
                return out;
            },
            [this](Pair found) {
                auto adopt = [](std::optional<jelly::Item>& item, std::optional<jelly::DownloadItem>& dl,
                                std::optional<jelly::Item>& remote) {
                    if (!item || dl) return;
                    auto d = App::get().downloads->find(item->id);
                    if (d && d->status == jelly::DownloadStatus::Completed)
                        dl = *d;
                    else
                        remote = std::move(item);
                };
                adopt(found.first, prevDownload_, prevItem_);
                adopt(found.second, nextDownload_, nextItem_);
            });
    }

    void fetchIntro() {
        std::string id = req_.itemId;
        async<jelly::Result<std::pair<int64_t, int64_t>>>([id] { return App::get().client->introSegment(id); },
                                                          [this](jelly::Result<std::pair<int64_t, int64_t>> r) {
                                                              if (!r.ok) return;
                                                              req_.introStart = static_cast<double>(r.value.first) / TICKS_PER_SECOND;
                                                              req_.introEnd = static_cast<double>(r.value.second) / TICKS_PER_SECOND;
                                                          });
    }

    void togglePause() {
        if (!player_) return;
        player_->setPaused(!player_->paused());
        if (player_->paused()) savePosition();
    }

    void toggleBottomScreen() {
        bottomOff_ = !bottomOff_;
        App::get().setBottomScreenOn(!bottomOff_);
    }

    void changeBoost(int delta) {
        // Volume boost: the 3DS slider alone is often too quiet with headphones.
        auto& cfg = App::get().config;
        cfg.volumeBoost = std::max(1, std::min(jelly::MAX_VOLUME_BOOST, cfg.volumeBoost + delta));
        if (player_) player_->setGain(static_cast<float>(cfg.volumeBoost));
        App::get().saveConfig();
    }

    void seekTo(double target) {
        if (!player_) return;
        double d = duration();
        target = std::max(0.0, d > 0 ? std::min(target, d - 2) : target);
        if (player_->seekable()) {
            player_->seek(target);
        } else {
            pendingSeek_ = target;
            pendingSeekAt_ = osGetTime() + 800;
        }
    }

    void seekBy(double delta) { seekTo(position() + delta); }

    void handleTap(int x, int y) {
        if (PREV.hit(x, y) && hasPrev()) playPrevNow();
        else if (BACK10.hit(x, y)) seekBy(-10);
        else if (FWD10.hit(x, y)) seekBy(10);
        else if (PLAY.hit(x, y)) togglePause();
        else if (NEXT.hit(x, y) && hasNext()) playNextNow();
        else if (SKIP.hit(x, y) && inIntro()) seekTo(req_.introEnd);
        else if (boostRect().hit(x, y))
            changeBoost(App::get().config.volumeBoost == jelly::MAX_VOLUME_BOOST ? 1 - jelly::MAX_VOLUME_BOOST : 1);
        else if (App::get().canTurnOffBottomScreen && screenRect().hit(x, y)) toggleBottomScreen();
        else if (y >= BAR_Y - 12 && y <= BAR_Y + BAR_H + 12 && duration() > 0) {
            seekTo(std::clamp((x - BAR_X) / BAR_W, 0.0f, 1.0f) * duration());
            dragTarget_ = -1;
        }
    }

    void savePosition() {
        if (state_ != State::Playing && state_ != State::Error) return;
        auto& a = App::get();
        a.progress->record(req_.itemId, static_cast<int64_t>(position() * TICKS_PER_SECOND), req_.runTimeTicks);
        a.progress->save();
    }

    void reportProgress() {
        auto id = req_.itemId, ms = req_.mediaSourceId, ps = playSessionId_;
        auto ticks = static_cast<int64_t>(position() * TICKS_PER_SECOND);
        bool paused = player_ && player_->paused();
        App::get().tasks.run([id, ms, ps, ticks, paused] { App::get().client->reportProgress(id, ms, ticks, ps, paused); });
    }

    /// Saves and reports the current position, then closes the player (stays on the stack until pop).
    void stopAndReport() {
        auto& a = App::get();
        savePosition();
        if (a.online()) {
            auto id = req_.itemId, ms = req_.mediaSourceId, ps = playSessionId_;
            auto ticks = static_cast<int64_t>(position() * TICKS_PER_SECOND);
            bool stream = streaming() && reportedStart_;
            bool transcoding = streaming() && streamStarted_;
            a.tasks.run([id, ms, ps, ticks, stream, transcoding] {
                if (stream) App::get().client->reportStopped(id, ms, ticks, ps);
                if (transcoding) App::get().client->stopEncoding(ps);  // free the server
                App::get().sync->pushItem(id);
            });
        }
        player_.reset();
    }

    void finish() {
        stopAndReport();
        App::get().pop();
    }

    void startNext() {
        auto& a = App::get();
        if (nextDownload_) {
            auto next = *nextDownload_;
            a.pop();
            playDownload(next, false);
        } else if (nextItem_) {
            auto next = *nextItem_;
            a.pop();
            playItem(next, false);
        } else {
            a.pop();
        }
    }

    void playNextNow() {
        stopAndReport();
        startNext();
    }

    void playPrevNow() {
        stopAndReport();
        auto& a = App::get();
        if (prevDownload_) {
            auto prev = *prevDownload_;
            a.pop();
            playDownload(prev, false);
        } else if (prevItem_) {
            auto prev = *prevItem_;
            a.pop();
            playItem(prev, false);
        }
    }

    void onEnded() {
        auto& a = App::get();
        // A stream that stops well before the end lost its connection: offer a retry, don't mark it watched.
        if (streaming() && duration() > 0 && position() < duration() - 30) {
            resumeAt_ = position();
            savePosition();
            error_ = "The stream ended early.\nA: continue from " + jelly::formatDuration(static_cast<int64_t>(resumeAt_));
            player_.reset();
            state_ = State::Error;
            return;
        }
        // Finished: record the full runtime so it counts as watched.
        a.progress->record(req_.itemId, static_cast<int64_t>(duration() * TICKS_PER_SECOND), req_.runTimeTicks);
        a.progress->save();
        if (a.online()) {
            std::string id = req_.itemId;
            a.tasks.run([id] { App::get().sync->pushItem(id); });
        }
        player_.reset();
        if (hasNext()) {
            state_ = State::NextUp;
            countdownUntil_ = osGetTime() + 5000;
        } else {
            a.pop();
        }
    }

    PlayRequest req_;
    std::shared_ptr<Player> player_;
    State state_ = State::Opening;
    std::string error_;
    std::string playSessionId_;
    double resumeAt_ = 0;
    double pendingSeek_ = -1;
    u64 pendingSeekAt_ = 0;
    double dragTarget_ = -1;
    u64 nextSave_ = 0;
    bool bottomOff_ = false;
    bool reportedStart_ = false;
    std::optional<jelly::DownloadItem> nextDownload_, prevDownload_;
    std::optional<jelly::Item> prevItem_;
    bool touchConsumed_ = false;
    bool streamStarted_ = false;
    std::optional<jelly::Item> nextItem_;
    u64 countdownUntil_ = 0;
};

}  // namespace

std::unique_ptr<Screen> makePlayerScreen(PlayRequest request) { return std::make_unique<PlayerScreen>(std::move(request)); }

}  // namespace app
