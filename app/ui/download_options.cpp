#include "screens.hpp"

#include "jelly/transcode.hpp"
#include "jelly/util.hpp"

namespace app {

namespace {

struct Choice {
    std::string label;
    int index = -1;  // stream index in the first item (-1 = none/default)
    std::string language;
};

class DownloadOptionsScreen : public Screen {
public:
    explicit DownloadOptionsScreen(std::vector<jelly::Item> items) : items_(std::move(items)) {
        auto& a = App::get();
        const jelly::MediaSource* src = nullptr;
        for (const auto& i : items_)
            if (!i.mediaSources.empty()) {
                src = &i.mediaSources.front();
                break;
            }
        if (src) {
            int defAudio = jelly::chooseAudioStream(*src, a.config.audioLanguage);
            int defSub = jelly::chooseSubtitleStream(*src, a.config.subtitleLanguage);
            for (const auto& s : src->streams) {
                if (s.type == "Audio") {
                    if (s.index == defAudio) audioSel_ = static_cast<int>(audio_.size());
                    audio_.push_back({s.displayTitle.empty() ? s.language : s.displayTitle, s.index, s.language});
                }
            }
            subs_.push_back({"None", -1, ""});
            for (const auto& s : src->streams) {
                if (s.type == "Subtitle") {
                    if (s.index == defSub) subSel_ = static_cast<int>(subs_.size());
                    subs_.push_back({s.displayTitle.empty() ? s.language : s.displayTitle, s.index, s.language});
                }
            }
        }
        if (audio_.empty()) audio_.push_back({"Default", -1, ""});
        profileNew_ = a.profile().id == "new3ds";
        list_.paging = false;  // left/right cycle the options instead
        build();
    }

    void update(const Input& in) override {
        if (in.down & KEY_B) {
            App::get().pop();
            return;
        }
        list_.update(in, 140);
        int dir = 0;
        if (in.repeat & KEY_RIGHT) dir = 1;
        if (in.repeat & KEY_LEFT) dir = -1;
        if (in.down & KEY_A) dir = 1;
        if (dir) {
            switch (list_.selected) {
            case 0: audioSel_ = (audioSel_ + dir + static_cast<int>(audio_.size())) % static_cast<int>(audio_.size()); break;
            case 1: subSel_ = (subSel_ + dir + static_cast<int>(subs_.size())) % static_cast<int>(subs_.size()); break;
            case 2: profileNew_ = !profileNew_; break;
            case 3:
                if (in.down & KEY_A) start();
                return;
            }
            build();
        }
        if (in.tapped && in.tapY >= 160 && in.tapY < 200) start();
    }

    void drawTop(Gfx& g) override {
        std::string title = items_.size() == 1 ? items_.front().label() : std::to_string(items_.size()) + " episodes";
        drawHeader(g, "Download \xC2\xB7 " + title);
        list_.draw(g, 0, 34, TOP_W, 140);
        g.text(std::string(glyph::DPad) + " change   " + glyph::A + " select   " + glyph::B + " cancel", TOP_W / 2.0f, 214, 0.45f,
               color::dim, Align::Center);
    }

    void drawBottom(Gfx& g) override {
        auto p = profileNew_ ? jelly::DeviceProfile::new3ds() : jelly::DeviceProfile::old3ds();
        int64_t total = 0;
        for (const auto& i : items_) {
            jelly::DownloadItem d;
            d.runTimeTicks = i.runTimeTicks;
            d.profileId = p.id;
            total += d.estimatedBytes();
        }
        int64_t free = sdFreeBytes();
        float y = 12;
        g.text("The server converts each video for the 3DS", 12, y, 0.46f, color::dim);
        y += 22;
        g.text(std::to_string(p.maxWidth) + "\xC3\x97" + std::to_string(p.maxHeight) + " \xC2\xB7 " +
                   std::to_string(p.maxFramerate) + " fps \xC2\xB7 H.264 + AAC",
               12, y, 0.5f, color::text);
        y += 26;
        g.text("Estimated size: ~" + jelly::formatSize(total), 12, y, 0.5f, color::text);
        y += 20;
        if (free >= 0)
            g.text("Free on SD card: " + jelly::formatSize(free), 12, y, 0.46f, free < total ? color::bad : color::dim);
        y += 22;
        if (!profileNew_ && App::get().isNew3ds)
            g.text("Old 3DS quality also plays on this New 3DS", 12, y, 0.42f, color::dim);
        if (profileNew_ && !App::get().isNew3ds)
            g.text("Warning: New 3DS quality may stutter on this console", 12, y, 0.42f, color::warn);
        g.roundRect(50, 160, 220, 40, color::accent);
        g.text(items_.size() == 1 ? "Download" : "Download " + std::to_string(items_.size()) + " episodes", BOTTOM_W / 2.0f, 171,
               0.56f, color::text, Align::Center);
    }

private:
    void build() {
        std::vector<ListRow> rows = {
            {"Audio", "< " + audio_[audioSel_].label + " >"},
            {"Subtitles (burned into the video)", "< " + subs_[subSel_].label + " >"},
            {"Quality", std::string("< ") + (profileNew_ ? "New 3DS (400\xC3\x97" "240)" : "Old 3DS (320\xC3\x97" "192)") + " >"},
            {"Start download", ""},
        };
        list_.setRows(std::move(rows));
    }

    /// Same track in every episode: match by language, fall back to the same index, then the default.
    static int pick(const jelly::MediaSource& src, const Choice& c, const char* type) {
        if (c.index < 0) return -1;
        if (!c.language.empty())
            for (const auto& s : src.streams)
                if (s.type == type && s.language == c.language) return s.index;
        for (const auto& s : src.streams)
            if (s.type == type && s.index == c.index) return s.index;
        return std::string(type) == "Audio" ? jelly::chooseAudioStream(src, "") : -1;
    }

    void start() {
        auto& a = App::get();
        auto profile = profileNew_ ? jelly::DeviceProfile::new3ds() : jelly::DeviceProfile::old3ds();
        const Choice& audio = audio_[audioSel_];
        const Choice& sub = subs_[subSel_];
        int queued = 0;
        for (const auto& item : items_) {
            jelly::DownloadOptions o{profile, -1, -1};
            if (!item.mediaSources.empty()) {
                o.audioStreamIndex = pick(item.mediaSources.front(), audio, "Audio");
                o.subtitleStreamIndex = pick(item.mediaSources.front(), sub, "Subtitle");
            }
            if (a.downloads->enqueue(item, o)) ++queued;
        }
        // Remember the choice for next time.
        if (!audio.language.empty()) a.config.audioLanguage = audio.language;
        a.config.subtitleLanguage = sub.index >= 0 ? sub.language : "";
        a.saveConfig();
        a.toast(queued == 1 ? "Download queued" : std::to_string(queued) + " downloads queued");
        a.pop();
    }

    std::vector<jelly::Item> items_;
    std::vector<Choice> audio_, subs_;
    int audioSel_ = 0, subSel_ = 0;
    bool profileNew_ = true;
    ListView list_;
};

}  // namespace

std::unique_ptr<Screen> makeDownloadOptions(std::vector<jelly::Item> items) {
    return std::make_unique<DownloadOptionsScreen>(std::move(items));
}

}  // namespace app
