#include "curl_http.hpp"

#include <curl/curl.h>

#include <cstdio>
#include <memory>
#include <vector>

namespace jelly {

namespace {

struct CurlHandle {
    CURL* h = curl_easy_init();
    curl_slist* headers = nullptr;
    ~CurlHandle() {
        if (headers) curl_slist_free_all(headers);
        if (h) curl_easy_cleanup(h);
    }
};

size_t appendToString(char* ptr, size_t size, size_t n, void* user) {
    static_cast<std::string*>(user)->append(ptr, size * n);
    return size * n;
}

struct SinkCtx {
    const std::function<bool(const char*, size_t)>* sink;
};

size_t writeToSink(char* ptr, size_t size, size_t n, void* user) {
    auto* ctx = static_cast<SinkCtx*>(user);
    return (*ctx->sink)(ptr, size * n) ? size * n : 0;
}

struct ProgressCtx {
    const ProgressFn* progress;
    const std::atomic_bool* cancel;
    curl_off_t lastReported = -1;
};

int onProgress(void* user, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    auto* ctx = static_cast<ProgressCtx*>(user);
    if (ctx->cancel && ctx->cancel->load()) return 1;
    // Report every 256 KB to keep lock traffic low.
    if (ctx->progress && *ctx->progress && dlnow - ctx->lastReported >= 256 * 1024) {
        ctx->lastReported = dlnow;
        (*ctx->progress)(static_cast<int64_t>(dlnow), static_cast<int64_t>(dltotal));
    }
    return 0;
}

std::string transportError(CURLcode code, const char* errbuf) {
    if (code == CURLE_ABORTED_BY_CALLBACK || code == CURLE_WRITE_ERROR) return "cancelled";
    if (errbuf && errbuf[0]) return errbuf;
    return curl_easy_strerror(code);
}

}  // namespace

CurlHttp::CurlHttp(CurlOptions options) : options_(std::move(options)) {
    static bool initialised = [] { return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK; }();
    (void)initialised;
}

static void applyCommon(CURL* h, const CurlOptions& o, curl_slist*& list, const Headers& headers) {
    for (const auto& [k, v] : headers) list = curl_slist_append(list, (k + ": " + v).c_str());
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, o.connectTimeoutSec);
    curl_easy_setopt(h, CURLOPT_USERAGENT, "Jelly3DS/0.1");
    curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, o.verifyTls ? 1L : 0L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, o.verifyTls ? 2L : 0L);
    if (o.verifyTls && !o.caFile.empty()) curl_easy_setopt(h, CURLOPT_CAINFO, o.caFile.c_str());
}

HttpResponse CurlHttp::request(const std::string& method, const std::string& url, const Headers& headers,
                               const std::string* body, const std::string& contentType) {
    HttpResponse res;
    CurlHandle c;
    if (!c.h) {
        res.error = "curl init failed";
        return res;
    }
    Headers all = headers;
    if (body) all.emplace_back("Content-Type", contentType);
    applyCommon(c.h, options_, c.headers, all);
    char errbuf[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(c.h, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(c.h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c.h, CURLOPT_TIMEOUT, options_.requestTimeoutSec);
    curl_easy_setopt(c.h, CURLOPT_WRITEFUNCTION, appendToString);
    curl_easy_setopt(c.h, CURLOPT_WRITEDATA, &res.body);
    if (method == "POST") {
        curl_easy_setopt(c.h, CURLOPT_POST, 1L);
        curl_easy_setopt(c.h, CURLOPT_POSTFIELDS, body ? body->c_str() : "");
        curl_easy_setopt(c.h, CURLOPT_POSTFIELDSIZE, body ? static_cast<long>(body->size()) : 0L);
    } else if (method != "GET") {
        curl_easy_setopt(c.h, CURLOPT_CUSTOMREQUEST, method.c_str());
    }
    CURLcode code = curl_easy_perform(c.h);
    if (code != CURLE_OK) {
        res.error = transportError(code, errbuf);
        return res;
    }
    long status = 0;
    curl_easy_getinfo(c.h, CURLINFO_RESPONSE_CODE, &status);
    res.status = static_cast<int>(status);
    return res;
}

HttpResponse CurlHttp::get(const std::string& url, const Headers& headers) {
    return request("GET", url, headers, nullptr, "");
}

HttpResponse CurlHttp::post(const std::string& url, const Headers& headers, const std::string& body,
                            const std::string& contentType) {
    return request("POST", url, headers, &body, contentType);
}

HttpResponse CurlHttp::del(const std::string& url, const Headers& headers) {
    return request("DELETE", url, headers, nullptr, "");
}

HttpResponse CurlHttp::download(const std::string& url, const Headers& headers, const std::string& path,
                                const ProgressFn& progress, const std::atomic_bool& cancel) {
    HttpResponse res;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        res.error = "Cannot write " + path;
        return res;
    }
    std::vector<char> buffer(options_.fileBufferSize);
    std::setvbuf(f, buffer.data(), _IOFBF, buffer.size());

    std::function<bool(const char*, size_t)> sink = [f](const char* data, size_t len) {
        return std::fwrite(data, 1, len, f) == len;
    };
    ProgressCtx pctx{&progress, &cancel};
    {
        CurlHandle c;
        applyCommon(c.h, options_, c.headers, headers);
        char errbuf[CURL_ERROR_SIZE] = {0};
        SinkCtx sctx{&sink};
        curl_easy_setopt(c.h, CURLOPT_ERRORBUFFER, errbuf);
        curl_easy_setopt(c.h, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c.h, CURLOPT_WRITEFUNCTION, writeToSink);
        curl_easy_setopt(c.h, CURLOPT_WRITEDATA, &sctx);
        curl_easy_setopt(c.h, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c.h, CURLOPT_XFERINFOFUNCTION, onProgress);
        curl_easy_setopt(c.h, CURLOPT_XFERINFODATA, &pctx);
        curl_easy_setopt(c.h, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(c.h, CURLOPT_LOW_SPEED_TIME, options_.stallTimeoutSec);
        curl_easy_setopt(c.h, CURLOPT_FAILONERROR, 1L);
        CURLcode code = curl_easy_perform(c.h);
        long status = 0;
        curl_easy_getinfo(c.h, CURLINFO_RESPONSE_CODE, &status);
        res.status = static_cast<int>(status);
        if (code != CURLE_OK) res.error = cancel.load() ? "cancelled" : transportError(code, errbuf);
    }
    if (std::fclose(f) != 0 && res.error.empty()) res.error = "Write failed (SD card full?)";
    return res;
}

HttpResponse CurlHttp::stream(const std::string& url, const Headers& headers,
                              const std::function<bool(const char*, size_t)>& sink, const std::atomic_bool& cancel) {
    HttpResponse res;
    CurlHandle c;
    applyCommon(c.h, options_, c.headers, headers);
    char errbuf[CURL_ERROR_SIZE] = {0};
    SinkCtx sctx{&sink};
    ProgressFn none;
    ProgressCtx pctx{&none, &cancel};
    curl_easy_setopt(c.h, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(c.h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c.h, CURLOPT_WRITEFUNCTION, writeToSink);
    curl_easy_setopt(c.h, CURLOPT_WRITEDATA, &sctx);
    curl_easy_setopt(c.h, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c.h, CURLOPT_XFERINFOFUNCTION, onProgress);
    curl_easy_setopt(c.h, CURLOPT_XFERINFODATA, &pctx);
    curl_easy_setopt(c.h, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c.h, CURLOPT_LOW_SPEED_TIME, options_.stallTimeoutSec);
    curl_easy_setopt(c.h, CURLOPT_FAILONERROR, 1L);
    CURLcode code = curl_easy_perform(c.h);
    long status = 0;
    curl_easy_getinfo(c.h, CURLINFO_RESPONSE_CODE, &status);
    res.status = static_cast<int>(status);
    if (code != CURLE_OK) res.error = cancel.load() ? "cancelled" : transportError(code, errbuf);
    return res;
}

}  // namespace jelly
