#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace jelly {

using Headers = std::vector<std::pair<std::string, std::string>>;

struct HttpResponse {
    int status = 0;       // HTTP status, 0 when the request never reached the server
    std::string body;
    std::string error;    // transport error message (empty on success)

    bool ok() const { return error.empty() && status >= 200 && status < 300; }
};

/// Called with (bytesDownloaded, bytesTotal or 0 when unknown).
using ProgressFn = std::function<void(int64_t, int64_t)>;

/// Abstract HTTP transport so core logic stays testable on the host.
class IHttp {
public:
    virtual ~IHttp() = default;
    virtual HttpResponse get(const std::string& url, const Headers& headers) = 0;
    virtual HttpResponse post(const std::string& url, const Headers& headers, const std::string& body,
                              const std::string& contentType = "application/json") = 0;
    virtual HttpResponse del(const std::string& url, const Headers& headers) = 0;
    /// Streams the response body into `path`. Aborts (returning error "cancelled") when `cancel` becomes true.
    virtual HttpResponse download(const std::string& url, const Headers& headers, const std::string& path,
                                  const ProgressFn& progress, const std::atomic_bool& cancel) = 0;
};

/// Wall clock in unix seconds; injectable for tests.
using ClockFn = std::function<int64_t()>;
int64_t systemClock();
int64_t systemClockMs();  // monotonic milliseconds

}  // namespace jelly
