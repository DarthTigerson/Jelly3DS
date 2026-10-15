#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "fake_http.hpp"
#include "jelly/client.hpp"
#include "jelly/config.hpp"
#include "jelly/downloads.hpp"
#include "jelly/progress.hpp"
#include "jelly/sync.hpp"
#include "jelly/transcode.hpp"
#include "jelly/util.hpp"

#include <filesystem>

using namespace jelly;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    std::string path;
    TempDir() {
        path = (fs::temp_directory_path() / ("jelly3ds-test-" + randomHex(8))).string();
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
};

Item episode(const std::string& id, int season, int ep, const std::string& series = "series1") {
    Item i;
    i.id = id;
    i.name = "Episode " + std::to_string(ep);
    i.type = "Episode";
    i.seriesId = series;
    i.seriesName = "Show";
    i.parentIndexNumber = season;
    i.indexNumber = ep;
    i.runTimeTicks = 1320LL * TICKS_PER_SECOND;  // 22 minutes
    MediaSource src;
    src.id = id;
    src.streams = {{1, "Video", "", "1080p", "h264", true, false},
                   {2, "Audio", "eng", "English", "aac", true, false},
                   {3, "Audio", "jpn", "Japanese", "aac", false, false},
                   {4, "Subtitle", "eng", "English", "subrip", false, true}};
    i.mediaSources = {src};
    return i;
}

Config loggedInConfig() {
    Config c;
    c.serverUrl = "http://server:8096";
    c.userId = "user1";
    c.accessToken = "tok";
    c.deviceId = "dev1";
    return c;
}

}  // namespace

TEST_CASE("url encoding and queries") {
    CHECK(urlEncode("a b&c=d/é") == "a%20b%26c%3Dd%2F%C3%A9");
    CHECK(buildQuery({{"a", "1"}, {"b c", "x y"}}) == "a=1&b%20c=x%20y");
}

TEST_CASE("ISO dates round-trip and ignore Jellyfin's 'never'") {
    CHECK(parseIsoDate("2026-10-11T17:03:12.1234567Z") == 1791738192);
    CHECK(parseIsoDate(formatIsoDate(1791738192)) == 1791738192);
    CHECK(parseIsoDate("0001-01-01T00:00:00.0000000Z") == 0);
    CHECK(parseIsoDate("") == 0);
    CHECK(formatIsoDate(0) == "1970-01-01T00:00:00.0000000Z");
}

TEST_CASE("formatting helpers") {
    CHECK(formatDuration(65) == "1:05");
    CHECK(formatDuration(3725) == "1:02:05");
    CHECK(formatSize(5 * 1024 * 1024) == "5.0 MB");
    CHECK(normalizeServerUrl(" 192.168.1.2:8096/ ") == "http://192.168.1.2:8096");
    CHECK(normalizeServerUrl("https://jf.example.com") == "https://jf.example.com");
}

TEST_CASE("profiles resolve from hardware and override") {
    CHECK(DeviceProfile::resolve(ProfileChoice::Auto, true).id == "new3ds");
    CHECK(DeviceProfile::resolve(ProfileChoice::Auto, false).id == "old3ds");
    CHECK(DeviceProfile::resolve(ProfileChoice::Old3DS, true).id == "old3ds");
    CHECK(DeviceProfile::resolve(ProfileChoice::New3DS, false).id == "new3ds");
    CHECK(DeviceProfile::old3ds().maxWidth < DeviceProfile::new3ds().maxWidth);
}

TEST_CASE("stream URL asks the server for a 3DS-friendly transcode") {
    StreamRequest r;
    r.itemId = "abc";
    r.mediaSourceId = "src";
    r.audioStreamIndex = 3;
    r.subtitleStreamIndex = 4;
    r.startTimeTicks = 300000000;
    r.deviceId = "dev";
    r.playSessionId = "ps";
    auto url = buildStreamUrl("http://s", DeviceProfile::new3ds(), r);
    CHECK(url.rfind("http://s/Videos/abc/stream.ts?", 0) == 0);
    CHECK(contains(url, "videoCodec=h264"));
    CHECK(contains(url, "audioCodec=aac"));
    CHECK(contains(url, "maxWidth=400"));
    CHECK(contains(url, "maxHeight=240"));
    CHECK(contains(url, "h264-profile=baseline"));
    CHECK(contains(url, "allowVideoStreamCopy=false"));
    CHECK(contains(url, "audioStreamIndex=3"));
    CHECK(contains(url, "subtitleStreamIndex=4"));
    CHECK(contains(url, "subtitleMethod=Encode"));
    CHECK(contains(url, "startTimeTicks=300000000"));
    CHECK(contains(url, "mediaSourceId=src"));

    StreamRequest plain;
    plain.itemId = "abc";
    auto url2 = buildStreamUrl("http://s", DeviceProfile::old3ds(), plain);
    CHECK_FALSE(contains(url2, "subtitleStreamIndex"));
    CHECK_FALSE(contains(url2, "startTimeTicks"));
    CHECK(contains(url2, "maxWidth=320"));
    CHECK(contains(url2, "mediaSourceId=abc"));
}

TEST_CASE("audio and subtitle track choice follows language defaults") {
    auto src = episode("e1", 1, 1).mediaSources[0];
    CHECK(chooseAudioStream(src, "jpn") == 3);
    CHECK(chooseAudioStream(src, "JPN") == 3);
    CHECK(chooseAudioStream(src, "fre") == 2);  // falls back to first audio track
    src.defaultAudioStreamIndex = 3;
    CHECK(chooseAudioStream(src, "") == 3);
    CHECK(chooseSubtitleStream(src, "eng") == 4);
    CHECK(chooseSubtitleStream(src, "") == -1);
    CHECK(chooseSubtitleStream(src, "ger") == -1);
}

TEST_CASE("items parse from Jellyfin JSON with missing fields") {
    auto j = nlohmann::json::parse(R"({
        "Id":"i1","Name":"Pilot","Type":"Episode","SeriesName":"Show","SeriesId":"s1",
        "ParentIndexNumber":1,"IndexNumber":2,"RunTimeTicks":13200000000,
        "ImageTags":{"Primary":"tag1"},
        "UserData":{"PlaybackPositionTicks":50,"Played":false,"LastPlayedDate":"2026-01-01T00:00:00Z"},
        "MediaSources":[{"Id":"m1","MediaStreams":[{"Index":0,"Type":"Audio","Language":"eng"}]}],
        "ProductionYear":null
    })");
    auto item = j.get<Item>();
    CHECK(item.id == "i1");
    CHECK(item.primaryImageTag == "tag1");
    CHECK(item.userData.playbackPositionTicks == 50);
    CHECK(item.userData.lastPlayedDate == parseIsoDate("2026-01-01T00:00:00Z"));
    CHECK(item.mediaSources.at(0).streams.at(0).language == "eng");
    CHECK(item.productionYear == 0);
    CHECK(item.label() == "S01E02 \xC2\xB7 Pilot");
}

TEST_CASE("config round-trips through JSON") {
    TempDir dir;
    Config c = loggedInConfig();
    c.profile = ProfileChoice::Old3DS;
    c.subtitleLanguage = "eng";
    REQUIRE(c.save(dir.path + "/config.json"));
    Config d;
    REQUIRE(d.load(dir.path + "/config.json"));
    CHECK(d.serverUrl == c.serverUrl);
    CHECK(d.profile == ProfileChoice::Old3DS);
    CHECK(d.subtitleLanguage == "eng");
    CHECK(d.loggedIn());
}

TEST_CASE("progress store records, marks played and persists") {
    TempDir dir;
    int64_t now = 1000;
    ProgressStore store(dir.path + "/progress.json", [&] { return now; });
    int64_t runtime = 1000 * TICKS_PER_SECOND;

    store.record("a", 300 * TICKS_PER_SECOND, runtime);
    auto e = store.get("a");
    REQUIRE(e);
    CHECK(e->positionTicks == 300 * TICKS_PER_SECOND);
    CHECK_FALSE(e->played);
    CHECK(e->dirty);
    CHECK(e->updatedAt == 1000);

    store.record("b", 950 * TICKS_PER_SECOND, runtime);
    CHECK(store.get("b")->played);
    CHECK(store.get("b")->positionTicks == 0);

    REQUIRE(store.save());
    ProgressStore reloaded(dir.path + "/progress.json", [&] { return now; });
    reloaded.load();
    CHECK(reloaded.get("a")->positionTicks == 300 * TICKS_PER_SECOND);
    CHECK(reloaded.dirtyEntries().size() == 2);
}

TEST_CASE("markClean ignores entries changed after they were read") {
    TempDir dir;
    ProgressStore store(dir.path + "/p.json", [] { return int64_t{5}; });
    store.record("a", 10, 100000);
    auto snapshot = store.get("a");
    store.record("a", 20, 100000);  // same second, newer position
    store.markClean("a", snapshot->revision);
    CHECK(store.get("a")->dirty);
    store.markClean("a", store.get("a")->revision);
    CHECK_FALSE(store.get("a")->dirty);
}

TEST_CASE("server state never overwrites unsynced local progress") {
    TempDir dir;
    ProgressStore store(dir.path + "/p.json", [] { return int64_t{100}; });
    store.record("a", 10, 1000);
    UserData server;
    server.playbackPositionTicks = 999;
    server.lastPlayedDate = 50;
    store.applyServer("a", server, 1000);
    CHECK(store.get("a")->positionTicks == 10);
    store.applyServer("b", server, 1000);
    CHECK(store.get("b")->positionTicks == 999);
    CHECK_FALSE(store.get("b")->dirty);
}

namespace {

struct DownloadFixture {
    TempDir dir;
    Config config = loggedInConfig();
    FakeHttp http;
    Client client{http, config, "test"};
    DownloadManager dm{client, http, dir.path + "/downloads", [] { return int64_t{42}; }};
    std::string videoBody = std::string(100 * 1024, 'v');

    DownloadFixture() {
        http.handler = [this](const FakeHttp::Call& c) -> HttpResponse {
            if (contains(c.url, "/Images/")) return {200, "jpg", ""};
            if (contains(c.url, "/stream.ts")) return {200, videoBody, ""};
            return {404, "", ""};
        };
        dm.load();
    }
};

}  // namespace

TEST_CASE("download queue completes a transcoded episode") {
    DownloadFixture f;
    DownloadOptions opts{DeviceProfile::old3ds(), 3, 4};
    REQUIRE(f.dm.enqueue(episode("e1", 1, 1), opts));
    CHECK_FALSE(f.dm.enqueue(episode("e1", 1, 1), opts));  // duplicate
    CHECK(f.dm.hasPending());

    REQUIRE(f.dm.runNext());
    auto d = f.dm.find("e1");
    REQUIRE(d);
    CHECK(d->status == DownloadStatus::Completed);
    CHECK(d->bytes == static_cast<int64_t>(f.videoBody.size()));
    CHECK(d->audioLabel == "Japanese");
    CHECK(d->subtitleLabel == "English");
    CHECK(fileExists(f.dm.videoPath("e1")));
    CHECK(fileExists(f.dm.thumbPath("e1")));
    CHECK_FALSE(fileExists(f.dm.videoPath("e1") + ".part"));
    CHECK_FALSE(f.dm.runNext());

    // The request carried the chosen profile and tracks.
    bool sawStream = false;
    for (const auto& c : f.http.calls) {
        if (!contains(c.url, "/stream.ts")) continue;
        sawStream = true;
        CHECK(contains(c.url, "maxWidth=320"));
        CHECK(contains(c.url, "audioStreamIndex=3"));
        CHECK(contains(c.url, "subtitleStreamIndex=4"));
        CHECK(contains(c.url, "deviceId=dev1-dl"));
    }
    CHECK(sawStream);
}

TEST_CASE("tiny or failed responses become Failed and can be retried") {
    DownloadFixture f;
    f.videoBody = "error";
    REQUIRE(f.dm.enqueue(episode("e1", 1, 1), {DeviceProfile::new3ds(), -1, -1}));
    f.dm.runNext();
    CHECK(f.dm.find("e1")->status == DownloadStatus::Failed);
    CHECK_FALSE(fileExists(f.dm.videoPath("e1")));

    f.videoBody = std::string(100 * 1024, 'v');
    f.dm.retry("e1");
    CHECK(f.dm.find("e1")->status == DownloadStatus::Queued);
    f.dm.runNext();
    CHECK(f.dm.find("e1")->status == DownloadStatus::Completed);
}

TEST_CASE("cancel and remove during a running download") {
    DownloadFixture f;
    REQUIRE(f.dm.enqueue(episode("e1", 1, 1), {DeviceProfile::new3ds(), -1, -1}));
    f.http.duringDownload = [&] {
        if (f.dm.isDownloading()) f.dm.cancel("e1");
    };
    f.dm.runNext();
    CHECK(f.dm.find("e1")->status == DownloadStatus::Failed);
    CHECK(f.dm.find("e1")->error == "Cancelled");

    f.dm.retry("e1");
    f.http.duringDownload = [&] {
        if (f.dm.isDownloading()) f.dm.remove("e1");
    };
    f.dm.runNext();
    CHECK_FALSE(f.dm.find("e1"));
    CHECK_FALSE(fileExists(f.dm.root() + "/e1"));
}

TEST_CASE("index reload requeues interrupted downloads and flags missing files") {
    DownloadFixture f;
    REQUIRE(f.dm.enqueue(episode("e1", 1, 1), {DeviceProfile::new3ds(), -1, -1}));
    REQUIRE(f.dm.enqueue(episode("e2", 1, 2), {DeviceProfile::new3ds(), -1, -1}));
    f.dm.runNext();  // e1 completes
    removeAll(f.dm.videoPath("e1"));

    DownloadManager again(f.client, f.http, f.dm.root());
    again.load();
    CHECK(again.find("e1")->status == DownloadStatus::Failed);
    CHECK(again.find("e2")->status == DownloadStatus::Queued);
}

TEST_CASE("previous downloaded episode mirrors next") {
    DownloadFixture f;
    DownloadOptions o{DeviceProfile::new3ds(), -1, -1};
    for (auto& [id, s, e] : std::vector<std::tuple<std::string, int, int>>{{"s1e1", 1, 1}, {"s1e3", 1, 3}, {"s2e1", 2, 1}})
        f.dm.enqueue(episode(id, s, e), o);
    while (f.dm.runNext()) {
    }
    CHECK(f.dm.previousEpisode("s2e1")->itemId == "s1e3");
    CHECK(f.dm.previousEpisode("s1e3")->itemId == "s1e1");
    CHECK_FALSE(f.dm.previousEpisode("s1e1"));
}

TEST_CASE("next downloaded episode ordering skips gaps and other series") {
    DownloadFixture f;
    DownloadOptions o{DeviceProfile::new3ds(), -1, -1};
    for (auto& [id, s, e] : std::vector<std::tuple<std::string, int, int>>{{"s1e1", 1, 1}, {"s1e3", 1, 3}, {"s2e1", 2, 1}}) {
        f.dm.enqueue(episode(id, s, e), o);
    }
    f.dm.enqueue(episode("other", 1, 2, "series2"), o);
    while (f.dm.runNext()) {
    }
    CHECK(f.dm.nextEpisode("s1e1")->itemId == "s1e3");
    CHECK(f.dm.nextEpisode("s1e3")->itemId == "s2e1");
    CHECK_FALSE(f.dm.nextEpisode("s2e1"));
    CHECK_FALSE(f.dm.nextEpisode("other"));
}

namespace {

struct SyncFixture {
    TempDir dir;
    Config config = loggedInConfig();
    FakeHttp http;
    Client client{http, config, "test"};
    int64_t now = 1800000000;  // 2027
    ProgressStore progress{dir.path + "/p.json", [this] { return now; }};
    DownloadManager dm{client, http, dir.path + "/downloads"};
    std::string serverLastPlayed = "2020-01-01T00:00:00Z";
    bool online = true;
    bool userDataEndpoint = true;

    SyncFixture() {
        http.handler = [this](const FakeHttp::Call& c) -> HttpResponse {
            if (!online) return {0, "", "Couldn't connect"};
            if (contains(c.url, "/Users/Me")) return {200, R"({"Id":"user1"})", ""};
            if (contains(c.url, "/UserData")) return userDataEndpoint ? HttpResponse{200, "{}", ""} : HttpResponse{404, "", ""};
            if (contains(c.url, "/Sessions/Playing")) return {204, "", ""};
            if (contains(c.url, "/UserPlayedItems/")) return {200, "{}", ""};
            if (contains(c.url, "/Items/e1")) {
                nlohmann::json j = {{"Id", "e1"},
                                    {"RunTimeTicks", 1000},
                                    {"UserData",
                                     {{"PlaybackPositionTicks", 777}, {"Played", false}, {"LastPlayedDate", serverLastPlayed}}}};
                return {200, j.dump(), ""};
            }
            if (contains(c.url, "/Items?")) return {200, R"({"Items":[]})", ""};
            return {404, "", ""};
        };
        dm.load();
    }
};

}  // namespace

TEST_CASE("sync pushes offline progress with the real watch time") {
    SyncFixture f;
    f.progress.record("e1", 500, 1000);
    auto report = SyncEngine(f.client, f.progress, f.dm).run();
    CHECK(report.reachedServer);
    CHECK(report.pushed == 1);
    CHECK_FALSE(f.progress.get("e1")->dirty);
    bool sawUserData = false;
    for (const auto& c : f.http.calls) {
        if (!contains(c.url, "/UserItems/e1/UserData")) continue;
        sawUserData = true;
        auto body = nlohmann::json::parse(c.body);
        CHECK(body["PlaybackPositionTicks"] == 500);
        CHECK(body["LastPlayedDate"] == formatIsoDate(1800000000));
    }
    CHECK(sawUserData);
}

TEST_CASE("sync falls back to playback reports on older servers") {
    SyncFixture f;
    f.userDataEndpoint = false;
    f.progress.record("e1", 500, 1000);
    auto report = SyncEngine(f.client, f.progress, f.dm).run();
    CHECK(report.pushed == 1);
    bool sawStopped = false;
    for (const auto& c : f.http.calls)
        if (contains(c.url, "/Sessions/Playing/Stopped")) sawStopped = true;
    CHECK(sawStopped);
}

TEST_CASE("sync lets newer server state win") {
    SyncFixture f;
    f.progress.record("e1", 500, 1000);                    // watched offline at `now`
    f.serverLastPlayed = formatIsoDate(1800003000);             // then watched on the TV later
    auto report = SyncEngine(f.client, f.progress, f.dm).run();
    CHECK(report.adopted == 1);
    CHECK(f.progress.get("e1")->positionTicks == 777);
    CHECK_FALSE(f.progress.get("e1")->dirty);
}

TEST_CASE("sync while offline keeps everything dirty") {
    SyncFixture f;
    f.online = false;
    f.progress.record("e1", 500, 1000);
    auto report = SyncEngine(f.client, f.progress, f.dm).run();
    CHECK_FALSE(report.reachedServer);
    CHECK(f.progress.get("e1")->dirty);
}

TEST_CASE("image fields: series poster and backdrop fall back to the parent") {
    auto ep = nlohmann::json::parse(R"({
        "Id":"e1","Type":"Episode","SeriesId":"s1","SeriesPrimaryImageTag":"sp",
        "BackdropImageTags":[],"ParentBackdropItemId":"s1","ParentBackdropImageTags":["pb"]
    })").get<Item>();
    CHECK(ep.seriesPrimaryImageTag == "sp");
    CHECK(ep.backdropItemId == "s1");
    CHECK(ep.backdropImageTag == "pb");

    auto movie = nlohmann::json::parse(R"({"Id":"m1","Type":"Movie","BackdropImageTags":["own"]})").get<Item>();
    CHECK(movie.backdropItemId == "m1");
    CHECK(movie.backdropImageTag == "own");

    auto bare = nlohmann::json::parse(R"({"Id":"x","Type":"Episode"})").get<Item>();
    CHECK(bare.backdropItemId.empty());
    CHECK(bare.seriesPrimaryImageTag.empty());
}

TEST_CASE("image URLs carry the image type") {
    Config c = loggedInConfig();
    FakeHttp http;
    Client client(http, c, "t");
    CHECK(contains(client.imageUrl("s1", "pb", 400, 240, "Backdrop"), "/Items/s1/Images/Backdrop?"));
    CHECK(contains(client.imageUrl("e1", "", 64, 96), "/Items/e1/Images/Primary?"));
}

TEST_CASE("cover view choice is remembered per page") {
    TempDir dir;
    Config c = loggedInConfig();
    CHECK(c.view("continue", "shelf") == "shelf");
    c.views["continue"] = "hero";
    c.views["movies"] = "grid";
    REQUIRE(c.save(dir.path + "/c.json"));
    Config d;
    REQUIRE(d.load(dir.path + "/c.json"));
    CHECK(d.view("continue", "shelf") == "hero");
    CHECK(d.view("movies", "hero") == "grid");
    CHECK(d.view("tvshows", "grid") == "grid");
}

TEST_CASE("volume boost defaults to 2x and is clamped") {
    TempDir dir;
    Config c = loggedInConfig();
    CHECK(c.volumeBoost == 2);
    c.volumeBoost = 3;
    REQUIRE(c.save(dir.path + "/c.json"));
    Config d;
    REQUIRE(d.load(dir.path + "/c.json"));
    CHECK(d.volumeBoost == 3);
    REQUIRE(saveJson(dir.path + "/bad.json", nlohmann::json{{"volumeBoost", 9}}));
    Config e;
    REQUIRE(e.load(dir.path + "/bad.json"));
    CHECK(e.volumeBoost == MAX_VOLUME_BOOST);
}

TEST_CASE("library queries carry sort and filters; latest reads a bare array") {
    Config c = loggedInConfig();
    FakeHttp http;
    std::string lastUrl;
    http.handler = [&](const FakeHttp::Call& call) -> HttpResponse {
        lastUrl = call.url;
        if (contains(call.url, "/Items/Latest")) return {200, R"([{"Id":"a","Name":"New","Type":"Movie","OfficialRating":"PG","Genres":["Drama"]}])", ""};
        return {200, R"({"Items":[],"TotalRecordCount":0})", ""};
    };
    Client client(http, c, "t");
    ItemQuery q;
    q.parentId = "lib";
    q.sortBy = "DateCreated";
    q.sortOrder = "Descending";
    q.filters = "IsUnplayed";
    client.items(q);
    CHECK(contains(lastUrl, "sortBy=DateCreated"));
    CHECK(contains(lastUrl, "sortOrder=Descending"));
    CHECK(contains(lastUrl, "filters=IsUnplayed"));
    auto latest = client.latest("lib", 5);
    REQUIRE(latest.ok);
    REQUIRE(latest.value.size() == 1);
    CHECK(latest.value[0].officialRating == "PG");
    CHECK(latest.value[0].genres.at(0) == "Drama");
}

TEST_CASE("queue: move up, pause and retry all") {
    DownloadFixture f;
    DownloadOptions o{DeviceProfile::new3ds(), -1, -1};
    f.dm.enqueue(episode("a", 1, 1), o);
    f.dm.enqueue(episode("b", 1, 2), o);
    f.dm.enqueue(episode("c", 1, 3), o);
    f.dm.moveUp("c");
    auto ids = [&] {
        std::string s;
        for (const auto& d : f.dm.items()) s += d.itemId;
        return s;
    };
    CHECK(ids() == "acb");
    f.dm.moveUp("a");  // already first: no change
    CHECK(ids() == "acb");

    f.dm.setPaused(true);
    CHECK_FALSE(f.dm.active());
    CHECK_FALSE(f.dm.runNext());
    f.dm.setPaused(false);
    CHECK(f.dm.active());
    REQUIRE(f.dm.runNext());
    CHECK(f.dm.find("a")->status == DownloadStatus::Completed);

    f.videoBody = "x";  // next two fail
    f.dm.runNext();
    f.dm.runNext();
    CHECK(f.dm.find("b")->status == DownloadStatus::Failed);
    f.dm.retryFailed();
    CHECK(f.dm.find("b")->status == DownloadStatus::Queued);
    CHECK(f.dm.find("c")->status == DownloadStatus::Queued);
}

TEST_CASE("downloads save the show poster and intro timings for offline use") {
    DownloadFixture f;
    std::vector<std::string> urls;
    f.http.handler = [&](const FakeHttp::Call& c) -> HttpResponse {
        urls.push_back(c.url);
        if (contains(c.url, "/MediaSegments/"))
            return {200, R"({"Items":[{"Type":"Intro","StartTicks":350000000,"EndTicks":1300000000}]})", ""};
        if (contains(c.url, "/Images/")) return {200, "jpg", ""};
        if (contains(c.url, "/stream.ts")) return {200, f.videoBody, ""};
        return {404, "", ""};
    };
    Item ep = episode("e1", 1, 1);
    ep.seriesPrimaryImageTag = "ptag";
    REQUIRE(f.dm.enqueue(ep, {DeviceProfile::new3ds(), -1, -1}));
    f.dm.runNext();
    auto d = f.dm.find("e1");
    CHECK(d->introStartTicks == 350000000);
    CHECK(d->introEndTicks == 1300000000);
    CHECK(fileExists(f.dm.posterPath("e1")));
    bool seriesPoster = false;
    for (const auto& u : urls)
        if (contains(u, "/Items/series1/Images/Primary") && contains(u, "tag=ptag")) seriesPoster = true;
    CHECK(seriesPoster);
    // Persisted across restarts.
    DownloadManager again(f.client, f.http, f.dm.root());
    again.load();
    CHECK(again.find("e1")->introEndTicks == 1300000000);
}

TEST_CASE("responses are saved and served from the SD cache without the network") {
    TempDir dir;
    Config c = loggedInConfig();
    FakeHttp http;
    int calls = 0;
    http.handler = [&](const FakeHttp::Call&) -> HttpResponse {
        ++calls;
        return {200, R"({"Items":[{"Id":"s1","Name":"Show","Type":"Series"}],"TotalRecordCount":1})", ""};
    };
    ResponseCache cache(dir.path + "/api");
    Client client(http, c, "t");
    client.setCache(&cache);
    ItemQuery q;
    q.parentId = "lib";

    CHECK_FALSE(client.items(q, Source::Cache).ok);  // nothing saved yet
    CHECK(calls == 0);
    REQUIRE(client.items(q).ok);
    CHECK(calls == 1);
    auto cached = client.items(q, Source::Cache);
    REQUIRE(cached.ok);
    CHECK(calls == 1);
    CHECK(cached.value.items.at(0).name == "Show");

    // A different query is a different cache entry.
    q.sortBy = "DateCreated";
    CHECK_FALSE(client.items(q, Source::Cache).ok);
}

TEST_CASE("lists skip media streams; episodes and details keep them") {
    Config c = loggedInConfig();
    FakeHttp http;
    std::vector<std::string> urls;
    http.handler = [&](const FakeHttp::Call& call) -> HttpResponse {
        urls.push_back(call.url);
        return {200, R"({"Items":[]})", ""};
    };
    Client client(http, c, "t");
    client.items(ItemQuery{});
    client.nextUp();
    client.episodes("s1");
    CHECK_FALSE(contains(urls.at(0), "MediaSources"));
    CHECK_FALSE(contains(urls.at(1), "MediaSources"));
    CHECK(contains(urls.at(2), "MediaSources"));
}

TEST_CASE("stopping a stream's transcode targets its device and play session") {
    Config c = loggedInConfig();
    FakeHttp http;
    FakeHttp::Call last;
    http.handler = [&](const FakeHttp::Call& call) -> HttpResponse {
        last = call;
        return {204, "", ""};
    };
    Client client(http, c, "t");
    REQUIRE(client.stopEncoding("ps123").ok);
    CHECK(last.method == "DELETE");
    CHECK(contains(last.url, "/Videos/ActiveEncodings?"));
    CHECK(contains(last.url, "deviceId=dev1"));
    CHECK(contains(last.url, "playSessionId=ps123"));
}
