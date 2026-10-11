#include "screens.hpp"

namespace app {

namespace {

class LoginScreen : public Screen {
public:
    LoginScreen() {
        auto& c = App::get().config;
        server_ = c.serverUrl;
        user_ = c.userName;
        buildRows();
    }

    void update(const Input& in) override {
        if (busy_) return;
        list_.update(in, 150);
        if (in.down & KEY_START) {
            App::get().quit();
            return;
        }
        int tapped = -1;
        if (in.tapped && in.tapY >= 150 && in.tapY < 190) tapped = 3;  // big Connect button
        if ((in.down & KEY_A) || tapped >= 0) {
            int row = tapped >= 0 ? tapped : list_.selected;
            switch (row) {
            case 0: keyboard("Server address, e.g. 192.168.1.20:8096", server_); break;
            case 1: keyboard("Username", user_); break;
            case 2: keyboard("Password", password_, true); break;
            case 3: connect(); break;
            }
            buildRows();
        }
    }

    void drawTop(Gfx& g) override {
        drawHeader(g, "Jelly3DS", App::get().isNew3ds ? "New 3DS" : "3DS");
        g.text("Connect to your Jellyfin server", 12, 36, 0.55f, color::text);
        list_.draw(g, 0, 62, TOP_W, 150);
        if (!error_.empty()) g.text(error_, TOP_W / 2.0f, 216, 0.48f, color::bad, Align::Center, TOP_W - 20);
    }

    void drawBottom(Gfx& g) override {
        g.text("Jelly3DS", BOTTOM_W / 2.0f, 20, 0.9f, color::accent, Align::Center);
        g.text("Watch your Jellyfin library offline", BOTTOM_W / 2.0f, 58, 0.48f, color::dim, Align::Center);
        g.text(std::string(glyph::DPad) + " choose   " + glyph::A + " edit", BOTTOM_W / 2.0f, 100, 0.48f, color::dim,
               Align::Center);
        g.roundRect(60, 150, 200, 40, busy_ ? color::panelHi : color::accent);
        if (busy_)
            drawLoading(g, 60, 150, 200, 40, "Connecting");
        else
            g.text("Connect", BOTTOM_W / 2.0f, 160, 0.6f, color::text, Align::Center);
        g.text(std::string(glyph::Start) + " exit", BOTTOM_W / 2.0f, 214, 0.45f, color::dim, Align::Center);
    }

private:
    void buildRows() {
        auto masked = std::string(password_.size(), '*');
        std::vector<ListRow> rows = {
            {"Server", server_.empty() ? "not set" : server_},
            {"Username", user_.empty() ? "not set" : user_},
            {"Password", password_.empty() ? "(empty)" : masked},
            {"Connect", ""},
        };
        list_.setRows(std::move(rows));
    }

    void connect() {
        if (server_.empty() || user_.empty()) {
            error_ = "Enter the server address and username first";
            return;
        }
        busy_ = true;
        error_.clear();
        std::string server = server_, user = user_, pass = password_;
        async<jelly::Result<jelly::AuthResult>>(
            [server, user, pass] { return App::get().client->authenticate(server, user, pass); },
            [this](jelly::Result<jelly::AuthResult> r) {
                busy_ = false;
                if (!r.ok) {
                    error_ = r.error;
                    return;
                }
                auto& a = App::get();
                a.saveConfig();
                a.toast("Signed in as " + r.value.user.name);
                a.replaceAll(makeHomeScreen());
                a.checkOnline(true);
            });
    }

    ListView list_;
    std::string server_, user_, password_, error_;
    bool busy_ = false;
};

}  // namespace

std::unique_ptr<Screen> makeLoginScreen() { return std::make_unique<LoginScreen>(); }

}  // namespace app
