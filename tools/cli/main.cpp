// Host-side driver for the Jelly3DS core: exercises the exact download/sync code the 3DS runs.
//   jelly-cli <dataDir> login <server> <user> <password>
//   jelly-cli <dataDir> views | ls <parentId> | episodes <seriesId>
//   jelly-cli <dataDir> download <itemId> <new3ds|old3ds> [audioLang] [subLang]
//   jelly-cli <dataDir> run            (process the download queue)
//   jelly-cli <dataDir> watch <itemId> <seconds>   (record offline progress)
//   jelly-cli <dataDir> sync | status
#include "curl_http.hpp"
#include "jelly/client.hpp"
#include "jelly/downloads.hpp"
#include "jelly/progress.hpp"
#include "jelly/sync.hpp"
#include "jelly/util.hpp"

#include <cstdio>
#include <string>

using namespace jelly;

static int usage() {
    std::fprintf(stderr, "usage: jelly-cli <dataDir> <login|views|ls|episodes|download|run|watch|sync|status> ...\n");
    return 2;
}

static void printItems(const std::vector<Item>& items) {
    for (const auto& i : items)
        std::printf("%-34s %-10s %s\n", i.id.c_str(), i.type.c_str(), i.label().c_str());
}

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    std::string data = argv[1], cmd = argv[2];
    makeDirs(data);

    Config config;
    config.load(data + "/config.json");
    CurlHttp http;
    Client client(http, config, "Jelly3DS CLI");
    ProgressStore progress(data + "/progress.json");
    progress.load();
    DownloadManager downloads(client, http, data + "/downloads");
    downloads.load();

    if (cmd == "login" && argc == 6) {
        auto r = client.authenticate(argv[3], argv[4], argv[5]);
        if (!r.ok) {
            std::fprintf(stderr, "login failed: %s\n", r.error.c_str());
            return 1;
        }
        config.save(data + "/config.json");
        std::printf("logged in as %s (%s)\n", r.value.user.name.c_str(), r.value.user.id.c_str());
        return 0;
    }
    if (!config.loggedIn()) {
        std::fprintf(stderr, "not logged in\n");
        return 1;
    }
    if (cmd == "views") {
        auto r = client.views();
        if (!r.ok) return std::fprintf(stderr, "%s\n", r.error.c_str()), 1;
        printItems(r.value);
    } else if (cmd == "ls" && argc == 4) {
        ItemQuery q;
        q.parentId = argv[3];
        auto r = client.items(q);
        if (!r.ok) return std::fprintf(stderr, "%s\n", r.error.c_str()), 1;
        printItems(r.value.items);
    } else if (cmd == "episodes" && argc == 4) {
        auto r = client.episodes(argv[3]);
        if (!r.ok) return std::fprintf(stderr, "%s\n", r.error.c_str()), 1;
        printItems(r.value);
    } else if (cmd == "download" && argc >= 5) {
        auto item = client.item(argv[3]);
        if (!item.ok) return std::fprintf(stderr, "%s\n", item.error.c_str()), 1;
        DownloadOptions o{DeviceProfile::byId(argv[4]), -1, -1};
        if (!item.value.mediaSources.empty()) {
            const auto& src = item.value.mediaSources.front();
            o.audioStreamIndex = chooseAudioStream(src, argc > 5 ? argv[5] : "");
            o.subtitleStreamIndex = chooseSubtitleStream(src, argc > 6 ? argv[6] : "");
        }
        std::printf("queued=%d audio=%d sub=%d\n", downloads.enqueue(item.value, o), o.audioStreamIndex,
                    o.subtitleStreamIndex);
    } else if (cmd == "run") {
        while (downloads.runNext()) {
        }
        for (const auto& d : downloads.items())
            std::printf("%s %s %s %s\n", d.itemId.c_str(), statusName(d.status), formatSize(d.bytes).c_str(),
                        d.error.c_str());
    } else if (cmd == "watch" && argc == 5) {
        auto d = downloads.find(argv[3]);
        progress.record(argv[3], std::stoll(argv[4]) * TICKS_PER_SECOND, d ? d->runTimeTicks : 0);
        progress.save();
    } else if (cmd == "sync") {
        SyncEngine sync(client, progress, downloads);
        auto r = sync.run();
        std::printf("reached=%d pushed=%d adopted=%d refreshed=%d failed=%d %s\n", r.reachedServer, r.pushed,
                    r.adopted, r.refreshed, r.failed, r.error.c_str());
    } else if (cmd == "status") {
        for (const auto& d : downloads.items()) {
            auto p = progress.get(d.itemId);
            std::printf("%s %-11s %-30s pos=%llds played=%d dirty=%d\n", d.itemId.c_str(), statusName(d.status),
                        d.label().c_str(), p ? static_cast<long long>(p->positionTicks / TICKS_PER_SECOND) : 0LL,
                        p ? p->played : 0, p ? p->dirty : 0);
        }
    } else {
        return usage();
    }
    return 0;
}
