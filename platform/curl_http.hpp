#pragma once

#include "jelly/platform.hpp"

#include <string>

namespace jelly {

struct CurlOptions {
    std::string caFile;            // PEM bundle used when verifyTls is on
    bool verifyTls = false;
    size_t fileBufferSize = 64 * 1024;  // SD writes are slow in small chunks
    long connectTimeoutSec = 15;
    long requestTimeoutSec = 30;   // API calls only; downloads use a low-speed watchdog instead
    long stallTimeoutSec = 180;    // transcodes with burned-in subtitles can take a while to start
};

/// libcurl implementation shared by the host tools and the 3DS app (a fresh easy handle per request).
class CurlHttp : public IHttp {
public:
    explicit CurlHttp(CurlOptions options = {});

    void setVerifyTls(bool verify) { options_.verifyTls = verify; }

    HttpResponse get(const std::string& url, const Headers& headers) override;
    HttpResponse post(const std::string& url, const Headers& headers, const std::string& body,
                      const std::string& contentType) override;
    HttpResponse del(const std::string& url, const Headers& headers) override;
    HttpResponse download(const std::string& url, const Headers& headers, const std::string& path,
                          const ProgressFn& progress, const std::atomic_bool& cancel) override;

    /// Streams the body to `sink` (return false to abort). Used by the player for network playback.
    HttpResponse stream(const std::string& url, const Headers& headers,
                        const std::function<bool(const char*, size_t)>& sink, const std::atomic_bool& cancel);

private:
    HttpResponse request(const std::string& method, const std::string& url, const Headers& headers,
                         const std::string* body, const std::string& contentType);

    CurlOptions options_;
};

}  // namespace jelly
