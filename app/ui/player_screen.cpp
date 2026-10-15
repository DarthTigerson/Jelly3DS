#include "screens.hpp"

#include "../player/player.hpp"

#include "jelly/transcode.hpp"
#include "jelly/util.hpp"

#include <optional>

namespace app {

namespace {

using jelly::TICKS_PER_SECOND;

constexpr float BAR_X = 16, BAR_Y = 118, BAR_W = BOTTOM_W - 32, BAR_H = 12;

class PlayerScreen : public Screen {
public:
    explicit PlayerScreen(PlayRequest req) : req_(std::move(req)) {
        playSessionId_ = jelly::randomHex(32);
        App::get().setPlaying(true);
        open(req_.startSeconds);
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
            if ((in.down & KEY_A) || now >= countdownUntil_) {
                auto next = *next_;
                App::get().pop();
                playDownload(next, false);
            } else if (in.down & KEY_B) {
                finish();
            }
            return;
        case State::Playing: break;
        }

        // --- Playing
        if (in.down & (KEY_A | KEY_DOWN | KEY_UP | KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R | KEY_Y)) showOsd();
        if (in.down & KEY_A) togglePause();
        if (in.repeat & KEY_LEFT) seekBy(-10);
        if (in.repeat & KEY_RIGHT) seekBy(10);
        if (in.repeat & KEY_L) seekBy(-60);
        if (in.repeat & KEY_R) seekBy(60);
        if (in.down & KEY_Y) osdPinned_ = !osdPinned_;
        if (in.down & (KEY_UP | KEY_DOWN)) {
            // Volume boost on the d-pad: the 3DS slider alone is often too quiet with headphones.
            auto& cfg = App::get().config;
            cfg.volumeBoost = std::max(1, std::min(3, cfg.volumeBoost + ((in.down & KEY_UP) ? 1 : -1)));
            player_->setGain(static_cast<float>(cfg.volumeBoost));
            App::get().saveConfig();
            App::get().toast(cfg.volumeBoost == 1 ? "Volume boost off" : "Volume boost " + std::to_string(cfg.volumeBoost) + "\xC3\x97");
        }
        if (in.down & KEY_X) toggleScreen();
        if (in.down & KEY_B) {
            finish();
            return;
        }
        if (in.tapped) handleTap(in.tapX, in.tapY);
        if (in.touching && in.touchY >= BAR_Y - 10 && in.touchY <= BAR_Y + BAR_H + 10 && duration() > 0) {
            // Dragging along the bar previews the target; it is applied on release (handleTap) or below.
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
            error_ = player_->error() + "\n\nPress A to retry from " + jelly::formatDuration(static_cast<int64_t>(resumeAt_));
            savePosition();
            player_.reset();
            state_ = State::Error;
            return;
        }
        if (player_->ended()) onEnded();
    }

    void drawTop(Gfx& g) override {
        g.rect(0, 0, TOP_W, SCREEN_H, color::black);
        if (player_ && player_->texture().valid) {
            float w = player_->videoWidth(), h = player_->videoHeight();
            float s = std::min(TOP_W / w, SCREEN_H / h);
            g.image(player_->texture(), (TOP_W - w * s) / 2, (SCREEN_H - h * s) / 2, w * s, h * s);
        }
        switch (state_) {
        case State::Opening:
            g.text(req_.title, TOP_W / 2.0f, 80, 0.6f, color::text, Align::Center, TOP_W - 20);
            drawLoading(g, 0, 100, TOP_W, 40, streaming() ? "Your server is converting the video" : "Opening");
            return;
        case State::Error:
            g.text("Playback stopped", TOP_W / 2.0f, 60, 0.65f, color::bad, Align::Center);
            g.wrapped(error_, 20, 95, 0.5f, color::text, TOP_W - 40, 6);
            return;
        case State::NextUp: {
            g.rect(0, 0, TOP_W, SCREEN_H, color::shade);
            int secs = static_cast<int>((countdownUntil_ - std::min(countdownUntil_, osGetTime())) / 1000) + 1;
            g.text("Up next in " + std::to_string(secs), TOP_W / 2.0f, 70, 0.6f, color::dim, Align::Center);
            g.text(next_->label(), TOP_W / 2.0f, 100, 0.7f, color::text, Align::Center, TOP_W - 20);
            g.text(std::string(glyph::A) + " play now    " + glyph::B + " stop", TOP_W / 2.0f, 150, 0.5f, color::dim, Align::Center);
            return;
        }
        case State::Playing: break;
        }
        if (player_ && player_->buffering()) drawLoading(g, 0, 100, TOP_W, 40, "Buffering");
        if (!osdVisible()) return;
        // On-screen display over the bottom of the video.
        g.rect(0, SCREEN_H - 40, TOP_W, 40, color::shade);
        g.text(req_.title, 8, SCREEN_H - 38, 0.48f, color::text, Align::Left, TOP_W - 120);
        g.text(timeText(), TOP_W - 8, SCREEN_H - 38, 0.48f, color::dim, Align::Right);
        g.progressBar(8, SCREEN_H - 14, TOP_W - 16, 4, fraction(), color::accent);
        if (player_ && player_->paused()) g.text("PAUSED", TOP_W / 2.0f, 12, 0.6f, color::text, Align::Center);
    }

    void drawBottom(Gfx& g) override {
        g.text(req_.title, BOTTOM_W / 2.0f, 10, 0.58f, color::text, Align::Center, BOTTOM_W - 20);
        if (!req_.subtitle.empty()) g.text(req_.subtitle, BOTTOM_W / 2.0f, 32, 0.45f, color::dim, Align::Center, BOTTOM_W - 20);
        g.text(timeText(), BOTTOM_W / 2.0f, 64, 0.8f, color::text, Align::Center);

        // Seek bar
        float frac = dragTarget_ >= 0 && duration() > 0 ? static_cast<float>(dragTarget_ / duration()) : fraction();
        g.progressBar(BAR_X, BAR_Y, BAR_W, BAR_H, frac, color::accent);
        g.rect(BAR_X + BAR_W * frac - 2, BAR_Y - 4, 4, BAR_H + 8, color::text);
        if (pendingSeek_ >= 0 || dragTarget_ >= 0)
            g.text("Seek to " + jelly::formatDuration(static_cast<int64_t>(dragTarget_ >= 0 ? dragTarget_ : pendingSeek_)),
                   BOTTOM_W / 2.0f, BAR_Y + 18, 0.45f, color::accent2, Align::Center);

        std::string info = streaming() ? "Streaming" : "Offline file";
        if (player_) info += " \xC2\xB7 " + player_->decoderName();
        if (player_ && !player_->hasAudio()) info += " \xC2\xB7 no audio (dspfirm.cdc missing?)";
        g.text(info, BOTTOM_W / 2.0f, 156, 0.4f, color::dim, Align::Center, BOTTOM_W - 16);
        g.text(std::string(glyph::L) + glyph::R + " \xC2\xB1" "60s  " + glyph::DPad + " \xC2\xB1" "10s  " + glyph::Y + " info  " +
                   glyph::X + " screen off",
               BOTTOM_W / 2.0f, 174, 0.4f, color::dim, Align::Center, BOTTOM_W - 16);

        ButtonBar bar;
        bool paused = player_ && player_->paused();
        bar.buttons = {{"-10s", 0, nullptr}, {paused ? "Play" : "Pause", 0, nullptr}, {"+10s", 0, nullptr},
                       {"Screen off", 0, nullptr}, {"Stop", 0, nullptr}};
        bar.draw(g);
    }

private:
    enum class State { Opening, Playing, Error, NextUp };

    bool streaming() const { return req_.localPath.empty(); }

    double duration() const {
        if (player_ && player_->duration() > 0) return player_->duration();
        return static_cast<double>(req_.runTimeTicks) / TICKS_PER_SECOND;
    }

    double position() const {
        if (pendingSeek_ >= 0) return pendingSeek_;
        if (player_) return player_->position();
        return resumeAt_;
    }

    float fraction() const {
        double d = duration();
        return d > 0 ? static_cast<float>(position() / d) : 0;
    }

    std::string timeText() const {
        std::string t = jelly::formatDuration(static_cast<int64_t>(position()));
        if (duration() > 0) t += " / " + jelly::formatDuration(static_cast<int64_t>(duration()));
        return t;
    }

    bool osdVisible() const { return osdPinned_ || osGetTime() < osdUntil_ || (player_ && player_->paused()); }
    void showOsd() { osdUntil_ = osGetTime() + 3000; }

    void open(double at) {
        auto& a = App::get();
        state_ = State::Opening;
        resumeAt_ = at;
        player_.reset();
        Player::Source src;
        if (streaming()) {
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
                    error_ = err + (streaming() ? "\n\nPress A to retry" : "");
                    state_ = State::Error;
                    return;
                }
                player_ = p;
                player_->setGain(static_cast<float>(App::get().config.volumeBoost));
                state_ = State::Playing;
                nextSave_ = osGetTime() + 10000;
                showOsd();
                if (streaming() && App::get().online() && !reportedStart_) {
                    reportedStart_ = true;
                    auto id = req_.itemId, ms = req_.mediaSourceId, ps = playSessionId_;
                    auto ticks = static_cast<int64_t>(resumeAt_ * TICKS_PER_SECOND);
                    App::get().tasks.run([id, ms, ps, ticks] { App::get().client->reportStart(id, ms, ticks, ps, false); });
                }
            });
    }

    void togglePause() {
        if (!player_) return;
        player_->setPaused(!player_->paused());
        if (player_->paused()) savePosition();
    }

    void toggleScreen() {
        bottomOff_ = !bottomOff_;
        App::get().setBottomScreenOn(!bottomOff_);
    }

    void seekTo(double target) {
        if (!player_) return;
        double d = duration();
        target = std::max(0.0, d > 0 ? std::min(target, d - 2) : target);
        showOsd();
        if (player_->seekable()) {
            player_->seek(target);
        } else {
            pendingSeek_ = target;
            pendingSeekAt_ = osGetTime() + 800;
        }
    }

    void seekBy(double delta) { seekTo(position() + delta); }

    void handleTap(int x, int y) {
        if (y >= SCREEN_H - ButtonBar::HEIGHT) {
            int button = x * 5 / BOTTOM_W;
            switch (button) {
            case 0: seekBy(-10); break;
            case 1: togglePause(); break;
            case 2: seekBy(10); break;
            case 3: toggleScreen(); break;
            case 4: finish(); break;
            }
            showOsd();
        } else if (y >= BAR_Y - 10 && y <= BAR_Y + BAR_H + 10 && duration() > 0) {
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

    void finish() {
        auto& a = App::get();
        savePosition();
        if (a.online()) {
            auto id = req_.itemId, ms = req_.mediaSourceId, ps = playSessionId_;
            auto ticks = static_cast<int64_t>(position() * TICKS_PER_SECOND);
            bool stream = streaming() && reportedStart_;
            a.tasks.run([id, ms, ps, ticks, stream] {
                if (stream) App::get().client->reportStopped(id, ms, ticks, ps);
                App::get().sync->pushItem(id);
            });
        }
        player_.reset();
        a.pop();
    }

    void onEnded() {
        auto& a = App::get();
        // A stream that stops well before the end lost its connection: offer a retry, don't mark it watched.
        if (streaming() && duration() > 0 && position() < duration() - 30) {
            resumeAt_ = position();
            savePosition();
            error_ = "The stream ended early.\n\nPress A to continue from " +
                     jelly::formatDuration(static_cast<int64_t>(resumeAt_));
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
        next_ = a.downloads->nextEpisode(req_.itemId);
        if (!streaming() && next_) {
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
    u64 osdUntil_ = 0;
    bool osdPinned_ = false;
    bool bottomOff_ = false;
    bool reportedStart_ = false;
    std::optional<jelly::DownloadItem> next_;
    u64 countdownUntil_ = 0;
};

}  // namespace

std::unique_ptr<Screen> makePlayerScreen(PlayRequest request) { return std::make_unique<PlayerScreen>(std::move(request)); }

}  // namespace app
