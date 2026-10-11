#pragma once

#include "jelly/platform.hpp"

#include <fstream>
#include <functional>
#include <string>
#include <vector>

/// Scriptable IHttp: a single handler sees every request and returns the response.
struct FakeHttp : jelly::IHttp {
    struct Call {
        std::string method;
        std::string url;
        std::string body;
    };
    std::function<jelly::HttpResponse(const Call&)> handler;
    std::vector<Call> calls;
    /// Invoked during download() before writing, e.g. to simulate a cancel mid-transfer.
    std::function<void()> duringDownload;

    jelly::HttpResponse dispatch(Call c) {
        calls.push_back(c);
        return handler ? handler(c) : jelly::HttpResponse{404, "", ""};
    }

    jelly::HttpResponse get(const std::string& url, const jelly::Headers&) override { return dispatch({"GET", url, ""}); }
    jelly::HttpResponse post(const std::string& url, const jelly::Headers&, const std::string& body,
                             const std::string&) override {
        return dispatch({"POST", url, body});
    }
    jelly::HttpResponse del(const std::string& url, const jelly::Headers&) override {
        return dispatch({"DELETE", url, ""});
    }
    jelly::HttpResponse download(const std::string& url, const jelly::Headers&, const std::string& path,
                                 const jelly::ProgressFn& progress, const std::atomic_bool& cancel) override {
        auto r = dispatch({"DOWNLOAD", url, ""});
        if (duringDownload) duringDownload();
        if (cancel.load()) return {0, "", "cancelled"};
        std::ofstream(path, std::ios::binary) << r.body;
        if (progress) progress(static_cast<int64_t>(r.body.size()), 0);
        return r;
    }
};

inline bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}
