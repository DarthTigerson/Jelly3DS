#include "app.hpp"

#include "player/player.hpp"
#include "ui/screens.hpp"

#include "jelly/util.hpp"

#include <malloc.h>
#include <cstdlib>

namespace app {

namespace {

constexpr size_t SOC_BUFFER_SIZE = 0x100000;
u32* socBuffer = nullptr;

void aptHookCallback(APT_HookType hook, void* param) {
    if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONSLEEP) static_cast<App*>(param)->suspendTop();
}

}  // namespace

App& App::get() {
    static App instance;
    return instance;
}

bool App::init() {
    osSetSpeedupEnable(true);
    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    isNew3ds = n3ds;
    // Lets the audio thread use the second CPU core (the system keeps the rest).
    APT_SetAppCpuTimeLimit(80);

    romfsInit();
    if (!gfx.init()) return false;
    socBuffer = static_cast<u32*>(memalign(0x1000, SOC_BUFFER_SIZE));
    if (socBuffer) socInit(socBuffer, SOC_BUFFER_SIZE);
    acInit();
    gspLcdInit();
    ptmuInit();
    ptmSysm_ = R_SUCCEEDED(ptmSysmInit());
    Player::initAudio();
    hidSetRepeatParameters(18, 5);

    jelly::makeDirs(std::string(DATA_DIR) + "/downloads");
    config.load(configPath());

    jelly::CurlOptions curlOptions;
    curlOptions.caFile = "romfs:/cacert.pem";
    curlOptions.verifyTls = config.verifyTls;
    curlOptions.fileBufferSize = 256 * 1024;
    http = std::make_unique<jelly::CurlHttp>(curlOptions);
    client = std::make_unique<jelly::Client>(*http, config, isNew3ds ? "New Nintendo 3DS" : "Nintendo 3DS");
    progress = std::make_unique<jelly::ProgressStore>(std::string(DATA_DIR) + "/progress.json");
    progress->load();
    downloads = std::make_unique<jelly::DownloadManager>(*client, *http, std::string(DATA_DIR) + "/downloads");
    downloads->load();
    sync = std::make_unique<jelly::SyncEngine>(*client, *progress, *downloads);

    images.start(http.get(), [this] { return client->authHeaders(); });
    tasks.start();
    downloadThread_.start([this] { downloadLoop(); }, 0x33, -2, 128 * 1024);
    aptHook(&aptCookie_, aptHookCallback, this);

    if (config.loggedIn()) {
        stack_.push_back(makeHomeScreen());
        checkOnline(true);
    } else {
        stack_.push_back(makeLoginScreen());
    }
    return true;
}

void App::shutdown() {
    quitting_ = true;
    stack_.clear();
    aptUnhook(&aptCookie_);
    if (downloads)
        for (const auto& d : downloads->items())
            if (d.status == jelly::DownloadStatus::Downloading) downloads->cancel(d.itemId);
    downloadThread_.join();
    tasks.stop();
    images.stop();
    if (progress) progress->save();
    if (!bottomOn_) setBottomScreenOn(true);
    aptSetSleepAllowed(true);
    Player::exitAudio();
    if (lidDark_ || screensOff_) GSPLCD_PowerOnAllBacklights();
    if (ptmSysm_) ptmSysmExit();
    ptmuExit();
    gspLcdExit();
    acExit();
    socExit();
    gfx.exit();
    romfsExit();
}

void App::suspendTop() {
    if (!stack_.empty()) stack_.back()->onSuspend();
    progress->save();
}

void App::push(std::unique_ptr<Screen> s) { pendingPush_.push_back(std::move(s)); }
void App::pop() { ++pendingPops_; }
void App::replaceAll(std::unique_ptr<Screen> s) {
    pendingReplace_ = true;
    pendingPush_.clear();
    pendingPops_ = 0;
    pendingPush_.push_back(std::move(s));
}

void App::goHome() {
    pendingPush_.clear();
    pendingPops_ = static_cast<int>(stack_.size()) - 1;
}

void App::applyNavigation() {
    bool changed = false;
    if (pendingReplace_) {
        stack_.clear();
        pendingReplace_ = false;
        changed = true;
    }
    while (pendingPops_ > 0 && !stack_.empty()) {
        stack_.pop_back();
        --pendingPops_;
        changed = true;
    }
    pendingPops_ = 0;
    bool pushed = !pendingPush_.empty();
    for (auto& s : pendingPush_) stack_.push_back(std::move(s));
    pendingPush_.clear();
    if (changed && !pushed && !stack_.empty()) stack_.back()->onResume();
}

void App::toast(const std::string& message) {
    toast_ = message;
    toastUntil_ = osGetTime() + 3000;
}

void App::saveConfig() {
    config.save(configPath());
    http->setVerifyTls(config.verifyTls);
}

jelly::DeviceProfile App::profile() const { return jelly::DeviceProfile::resolve(config.profile, isNew3ds); }

std::string App::profileLabel() const {
    auto p = profile();
    std::string base = p.id == "new3ds" ? "New 3DS" : "Old 3DS";
    return base + " (" + std::to_string(p.maxWidth) + "\xC3\x97" + std::to_string(p.maxHeight) + ", " +
           std::to_string(p.maxFramerate) + " fps)";
}

void App::checkOnline(bool thenSync) {
    if (!config.loggedIn() || checking_.exchange(true)) return;
    tasks.run([this, thenSync] {
        auto r = client->ping();
        online_ = r.ok;
        std::string message;
        if (r.ok && thenSync) {
            auto report = sync->run();
            if (report.pushed > 0)
                message = "Synced " + std::to_string(report.pushed) + " offline watch update" +
                          (report.pushed == 1 ? "" : "s");
        } else if (!r.ok && r.error.find("Not authorized") != std::string::npos) {
            message = "Login expired - log in again in Settings";
        }
        checking_ = false;
        tasks.post([this, message] {
            if (!message.empty()) toast(message);
            if (!stack_.empty()) stack_.back()->onResume();
        });
    });
}

void App::logout() {
    online_ = false;
    config.accessToken.clear();
    config.userId.clear();
    saveConfig();
    replaceAll(makeLoginScreen());
}

void App::pollWifi() {
    u64 now = osGetTime();
    if (now < nextWifiPoll_) return;
    nextWifiPoll_ = now + 2000;
    u32 status = 0;
    bool connected = R_SUCCEEDED(ACU_GetWifiStatus(&status)) && status != 0;
    bool was = wifi_.exchange(connected);
    if (connected && !was) checkOnline(true);
    if (!connected && was && online_) {
        online_ = false;
        if (!stack_.empty()) stack_.back()->onResume();
    }
}

void App::downloadLoop() {
    while (!quitting_) {
        // On an Old 3DS the CPU is fully busy decoding video: hold the queue while something plays.
        bool allowed = online_ && (isNew3ds || !playing_);
        if (allowed && downloads->hasPending()) {
            if (downloads->runNext()) continue;
        }
        svcSleepThread(500LL * 1000 * 1000);
    }
}

void App::manageSleep() {
    // Sleep mode suspends the app and its Wi-Fi, so while the queue runs the console must stay awake.
    bool block = online_ && downloads->active();
    if (block != sleepBlocked_) {
        aptSetSleepAllowed(!block);
        sleepBlocked_ = block;
    }
    u64 now = osGetTime();
    if (now < nextLidPoll_) return;
    nextLidPoll_ = now + 500;
    u8 shell = 1;
    PTMU_GetShellState(&shell);  // 0 = lid closed
    bool closed = shell == 0;
    if (closed && block && !lidDark_) {
        GSPLCD_PowerOffAllBacklights();
        lidDark_ = true;
    } else if (!closed && lidDark_) {
        GSPLCD_PowerOnAllBacklights();
        if (!bottomOn_) GSPLCD_PowerOffBacklight(GSPLCD_SCREEN_BOTTOM);
        lidDark_ = false;
    } else if (closed && lidDark_ && !block) {
        // Queue finished with the lid shut: let the console sleep now.
        GSPLCD_PowerOnAllBacklights();
        lidDark_ = false;
        if (ptmSysm_) PTMSYSM_RequestSleep();
    }
}

void App::screensOff() {
    GSPLCD_PowerOffAllBacklights();
    screensOff_ = true;
}

void App::setBottomScreenOn(bool on) {
    bottomOn_ = on;
    if (on)
        GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTTOM);
    else
        GSPLCD_PowerOffBacklight(GSPLCD_SCREEN_BOTTOM);
}

void App::readInput(Input& in) {
    hidScanInput();
    in.down = hidKeysDown();
    in.held = hidKeysHeld();
    in.repeat = hidKeysDownRepeat();
    u32 up = hidKeysUp();
    in.tapped = false;
    in.touching = in.held & KEY_TOUCH;

    if (screensOff_ && (in.down & ~KEY_TOUCH)) {
        GSPLCD_PowerOnAllBacklights();
        if (!bottomOn_) GSPLCD_PowerOffBacklight(GSPLCD_SCREEN_BOTTOM);
        screensOff_ = false;
        in.down = 0;  // the wake-up press does nothing else
    }
    // Any button wakes the bottom screen back up (except X, which toggles it in the player).
    if (!bottomOn_ && (in.down & ~(KEY_X | KEY_TOUCH)) && !(in.held & KEY_X)) {
        bool keepOff = !stack_.empty() && stack_.back()->keepsBottomOff();
        if (!keepOff) setBottomScreenOn(true);
    }

    if (in.touching) {
        touchPosition t;
        hidTouchRead(&t);
        if (in.down & KEY_TOUCH) {
            touchStartX_ = t.px;
            touchStartY_ = t.py;
            touchMoved_ = false;
        }
        if (std::abs(t.px - touchStartX_) > 10 || std::abs(t.py - touchStartY_) > 10) touchMoved_ = true;
        in.touchX = t.px;
        in.touchY = t.py;
        lastTouchX_ = t.px;
        lastTouchY_ = t.py;
    }
    if ((up & KEY_TOUCH) && !touchMoved_) {
        in.tapped = true;
        in.tapX = lastTouchX_;
        in.tapY = lastTouchY_;
    }
}

void App::drawToast() {
    if (toast_.empty() || osGetTime() > toastUntil_) return;
    float w = std::min(300.0f, gfx.textWidth(toast_, 0.48f) + 20);
    float x = (BOTTOM_W - w) / 2;
    gfx.rect(x, 6, w, 24, color::shade);
    gfx.rect(x, 29, w, 1, color::accent);
    gfx.text(toast_, BOTTOM_W / 2.0f, 10, 0.48f, color::text, Align::Center, w - 12);
}

void App::run() {
    Input in;
    while (aptMainLoop() && !quitting_) {
        readInput(in);
        pollWifi();
        tasks.drainMain();
        if (stack_.empty()) break;

        if ((in.down & KEY_START) && stack_.size() > 1 && stack_.back()->startGoesHome()) {
            goHome();
            in.down &= ~KEY_START;
        }
        stack_.back()->update(in);
        applyNavigation();
        if (stack_.empty()) break;

        manageSleep();

        gfx.beginFrame();
        images.pump();
        Screen* top = stack_.back().get();
        Screen* under = stack_.size() > 1 && top->overlay() ? stack_[stack_.size() - 2].get() : nullptr;
        gfx.beginTop();
        if (under) under->drawTop(gfx);
        top->drawTop(gfx);
        gfx.beginBottom();
        if (under) under->drawBottom(gfx);
        top->drawBottom(gfx);
        drawToast();
        gfx.endFrame();
    }
}

void App::quit() { quitting_ = true; }

bool keyboard(const std::string& hint, std::string& value, bool password, int maxLength) {
    SwkbdState kb;
    swkbdInit(&kb, SWKBD_TYPE_WESTERN, 2, maxLength);
    swkbdSetHintText(&kb, hint.c_str());
    swkbdSetInitialText(&kb, value.c_str());
    if (password) swkbdSetPasswordMode(&kb, SWKBD_PASSWORD_HIDE_DELAY);
    swkbdSetValidation(&kb, SWKBD_ANYTHING, 0, 0);
    std::vector<char> buf(static_cast<size_t>(maxLength) * 4 + 1, 0);
    SwkbdButton b = swkbdInputText(&kb, buf.data(), buf.size());
    if (b != SWKBD_BUTTON_CONFIRM) return false;
    value = buf.data();
    return true;
}

}  // namespace app
